#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] std::int64_t boolean_integer(const bool value) noexcept {
  return value ? 1 : 0;
}

void validate_timestamp(const std::int64_t value) {
  if (value < 0) {
    throw std::invalid_argument{"Persistent timestamps must be nonnegative."};
  }
}

void validate_cache(const std::optional<std::string> &value) {
  if (value.has_value() && (value->empty() || value->size() > 128)) {
    throw std::invalid_argument{"Discord name caches must be 1-128 bytes."};
  }
}

} // namespace

SqliteRepositoryContext::SqliteRepositoryContext(Database database)
    : database_{std::move(database)} {}

SqliteConnection &SqliteRepositoryContext::connection() noexcept {
  return database_.connection();
}

SqliteApplicationInstanceRepository::SqliteApplicationInstanceRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_) {
    throw std::invalid_argument{"SQLite repository context is required."};
  }
}

void SqliteApplicationInstanceRepository::record_start(
    const ApplicationInstanceRecord &record) {
  if (!valid_uuid_v4(record.instance_id) ||
      record.application_version.empty() ||
      record.application_version.size() > 128 || record.git_revision.empty() ||
      record.git_revision.size() > 128 || record.hostname.empty() ||
      record.hostname.size() > 255 || record.process_id <= 0) {
    throw std::invalid_argument{"Application instance metadata is invalid."};
  }
  validate_timestamp(record.started_at_ms);
  auto statement = context_->connection().prepare(
      "INSERT INTO application_instance "
      "(instance_id, application_version, git_revision, hostname, process_id, "
      " started_at_ms) VALUES (?, ?, ?, ?, ?, ?)");
  statement.bind(1, record.instance_id);
  statement.bind(2, record.application_version);
  statement.bind(3, record.git_revision);
  statement.bind(4, record.hostname);
  statement.bind(5, record.process_id);
  statement.bind(6, record.started_at_ms);
  statement.execute();
}

void SqliteApplicationInstanceRepository::record_stop(
    const std::string &instance_id, const std::int64_t stopped_at_ms,
    const ApplicationStopReason reason) {
  if (!valid_uuid_v4(instance_id)) {
    throw std::invalid_argument{"Application instance ID is invalid."};
  }
  validate_timestamp(stopped_at_ms);
  auto statement = context_->connection().prepare(
      "UPDATE application_instance SET stopped_at_ms = ?, stop_reason = ? "
      "WHERE instance_id = ? AND stopped_at_ms IS NULL");
  statement.bind(1, stopped_at_ms);
  statement.bind(2, application_stop_reason_name(reason));
  statement.bind(3, instance_id);
  statement.execute();
  if (context_->connection().changes() != 0) {
    return;
  }

  auto existing = context_->connection().prepare(
      "SELECT 1 FROM application_instance WHERE instance_id = ?");
  existing.bind(1, instance_id);
  if (!existing.step()) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Application instance stop target does not exist."};
  }
}

SqliteCoreIdentityRepository::SqliteCoreIdentityRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_) {
    throw std::invalid_argument{"SQLite repository context is required."};
  }
}

void SqliteCoreIdentityRepository::initialize_or_validate_scope(
    const ServerScopeConfiguration &scope, const std::int64_t now_ms) {
  if (!scope.guild_id.is_set() || !scope.primary_channel_id.is_set() ||
      !scope.owner_user_id.is_set()) {
    throw std::invalid_argument{"Configured server scope must be complete."};
  }
  validate_timestamp(now_ms);

  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto current = context_->connection().prepare(
      "SELECT guild_id, primary_channel_id, owner_user_id FROM guild_config "
      "WHERE singleton = 1");
  if (current.step()) {
    const bool matches =
        current.column_text(0) == scope.guild_id.str() &&
        current.column_text(1) == scope.primary_channel_id.str() &&
        current.column_text(2) == scope.owner_user_id.str();
    if (!matches || current.step()) {
      throw DatabaseError{
          DatabaseErrorCategory::incompatible, SQLITE_CONSTRAINT,
          SQLITE_CONSTRAINT,
          "Configured Discord scope does not match the database."};
    }
  } else {
    ensure_user_uncommitted(DiscordUserRecord{
        .user_id = scope.owner_user_id,
        .display_name = std::nullopt,
        .username = std::nullopt,
        .is_bot = false,
        .observed_at_ms = now_ms,
    });
    auto insert = context_->connection().prepare(
        "INSERT INTO guild_config "
        "(guild_id, primary_channel_id, owner_user_id, created_at_ms, "
        " updated_at_ms) VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, scope.guild_id.str());
    insert.bind(2, scope.primary_channel_id.str());
    insert.bind(3, scope.owner_user_id.str());
    insert.bind(4, now_ms);
    insert.bind(5, now_ms);
    insert.execute();
  }
  transaction.commit();
}

void SqliteCoreIdentityRepository::ensure_user(const DiscordUserRecord &user) {
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  ensure_user_uncommitted(user);
  transaction.commit();
}

std::optional<UserPreferences> SqliteCoreIdentityRepository::load_preferences(
    const DiscordSnowflake &user_id) {
  if (!user_id.is_set()) {
    throw std::invalid_argument{"Discord user ID must be set."};
  }
  auto statement = context_->connection().prepare(
      "SELECT chronicle_opt_in, memory_callback_opt_in, "
      "appearance_callback_opt_in, voice_input_opt_in, "
      "public_tarot_results_opt_in, quiet_until_ms, updated_at_ms "
      "FROM user_preference WHERE user_id = ?");
  statement.bind(1, user_id.str());
  if (!statement.step()) {
    return std::nullopt;
  }
  UserPreferences result{
      .chronicle_opt_in = statement.column_int64(0) != 0,
      .memory_callback_opt_in = statement.column_int64(1) != 0,
      .appearance_callback_opt_in = statement.column_int64(2) != 0,
      .voice_input_opt_in = statement.column_int64(3) != 0,
      .public_tarot_results_opt_in = statement.column_int64(4) != 0,
      .quiet_until_ms =
          statement.column_is_null(5)
              ? std::nullopt
              : std::optional<std::int64_t>{statement.column_int64(5)},
      .updated_at_ms = statement.column_int64(6),
  };
  if (statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "User preference query returned duplicate rows."};
  }
  return result;
}

void SqliteCoreIdentityRepository::ensure_user_uncommitted(
    const DiscordUserRecord &user) {
  if (!user.user_id.is_set()) {
    throw std::invalid_argument{"Discord user ID must be set."};
  }
  validate_timestamp(user.observed_at_ms);
  validate_cache(user.display_name);
  validate_cache(user.username);

  auto upsert = context_->connection().prepare(
      "INSERT INTO discord_user "
      "(user_id, display_name_cache, username_cache, is_bot, first_seen_at_ms, "
      " last_seen_at_ms, created_at_ms, updated_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(user_id) DO UPDATE SET "
      "display_name_cache = CASE "
      "  WHEN excluded.last_seen_at_ms >= discord_user.last_seen_at_ms "
      "  THEN COALESCE(excluded.display_name_cache, "
      "                discord_user.display_name_cache) "
      "  ELSE discord_user.display_name_cache END, "
      "username_cache = CASE "
      "  WHEN excluded.last_seen_at_ms >= discord_user.last_seen_at_ms "
      "  THEN COALESCE(excluded.username_cache, "
      "                discord_user.username_cache) "
      "  ELSE discord_user.username_cache END, "
      "is_bot = CASE "
      "  WHEN excluded.last_seen_at_ms >= discord_user.last_seen_at_ms "
      "  THEN excluded.is_bot ELSE discord_user.is_bot END, "
      "first_seen_at_ms = min(discord_user.first_seen_at_ms, "
      "                       excluded.first_seen_at_ms), "
      "last_seen_at_ms = max(discord_user.last_seen_at_ms, "
      "                      excluded.last_seen_at_ms), "
      "updated_at_ms = max(discord_user.updated_at_ms, "
      "excluded.updated_at_ms)");
  upsert.bind(1, user.user_id.str());
  if (user.display_name.has_value()) {
    upsert.bind(2, *user.display_name);
  } else {
    upsert.bind_null(2);
  }
  if (user.username.has_value()) {
    upsert.bind(3, *user.username);
  } else {
    upsert.bind_null(3);
  }
  upsert.bind(4, boolean_integer(user.is_bot));
  upsert.bind(5, user.observed_at_ms);
  upsert.bind(6, user.observed_at_ms);
  upsert.bind(7, user.observed_at_ms);
  upsert.bind(8, user.observed_at_ms);
  upsert.execute();

  auto preference = context_->connection().prepare(
      "INSERT INTO user_preference (user_id, updated_at_ms) VALUES (?, ?) "
      "ON CONFLICT(user_id) DO NOTHING");
  preference.bind(1, user.user_id.str());
  preference.bind(2, user.observed_at_ms);
  preference.execute();
}

} // namespace sanguinius::persistence
