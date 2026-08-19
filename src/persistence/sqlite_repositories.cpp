#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr std::int64_t reveal_reservation_lifetime_ms = 5 * 60 * 1'000;

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

void validate_key(const std::string_view value) {
  if (value.empty() || value.size() > 160) {
    throw std::invalid_argument{"Idempotency keys must be 1-160 bytes."};
  }
}

void validate_notice_content(const PendingNoticeContent &content) {
  if (content.title.empty() || content.title.size() > 100 ||
      content.body.empty() || content.body.size() > 1'500 ||
      content.actions.size() > 2) {
    throw std::invalid_argument{"Pending notice content is invalid."};
  }
  for (const auto &action : content.actions) {
    if (action.custom_id.empty() || action.custom_id.size() > 100 ||
        action.label.empty() || action.label.size() > 80) {
      throw std::invalid_argument{"Pending notice action is invalid."};
    }
  }
}

[[nodiscard]] std::string
encode_notice_content(const PendingNoticeContent &content) {
  auto actions = nlohmann::json::array();
  for (const auto &action : content.actions) {
    actions.push_back({{"custom_id", action.custom_id},
                       {"label", action.label}});
  }
  return nlohmann::json{{"title", content.title},
                        {"body", content.body},
                        {"actions", std::move(actions)}}.dump();
}

[[nodiscard]] PendingNoticeContent
decode_notice_content(const std::string &payload) {
  try {
    const auto parsed = nlohmann::json::parse(payload);
    if (!parsed.is_object() || !parsed.contains("title") ||
        !parsed.contains("body") || !parsed.at("title").is_string() ||
        !parsed.at("body").is_string()) {
      throw std::runtime_error{"shape"};
    }
    PendingNoticeContent content{parsed.at("title").get<std::string>(),
                                 parsed.at("body").get<std::string>()};
    if (parsed.contains("actions")) {
      if (!parsed.at("actions").is_array()) {
        throw std::runtime_error{"shape"};
      }
      for (const auto &action : parsed.at("actions")) {
        content.actions.push_back(PendingNoticeContent::Action{
            .custom_id = action.at("custom_id").get<std::string>(),
            .label = action.at("label").get<std::string>(),
        });
      }
    }
    validate_notice_content(content);
    return content;
  } catch (const std::exception &) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Pending notice payload is incompatible."};
  }
}

[[nodiscard]] PendingNoticeState notice_state(const std::string_view value) {
  if (value == "pending") {
    return PendingNoticeState::pending;
  }
  if (value == "opened") {
    return PendingNoticeState::opened;
  }
  if (value == "consumed") {
    return PendingNoticeState::consumed;
  }
  if (value == "expired") {
    return PendingNoticeState::expired;
  }
  if (value == "cancelled") {
    return PendingNoticeState::cancelled;
  }
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Pending notice state is incompatible."};
}

[[nodiscard]] PendingNoticeRecord notice_record(SqliteStatement &statement) {
  return PendingNoticeRecord{
      .notice_id = statement.column_text(0),
      .target_user_id = DiscordSnowflake::parse(statement.column_text(1)),
      .notice_type = statement.column_text(2),
      .content = decode_notice_content(statement.column_text(3)),
      .state = notice_state(statement.column_text(4)),
      .expires_at_ms =
          statement.column_is_null(5)
              ? std::nullopt
              : std::optional<std::int64_t>{statement.column_int64(5)},
      .opened_at_ms =
          statement.column_is_null(6)
              ? std::nullopt
              : std::optional<std::int64_t>{statement.column_int64(6)},
      .consumed_at_ms =
          statement.column_is_null(7)
              ? std::nullopt
              : std::optional<std::int64_t>{statement.column_int64(7)},
      .created_at_ms = statement.column_int64(8),
  };
}

[[nodiscard]] std::optional<PendingNoticeRecord>
load_notice(SqliteConnection &connection, const std::string_view notice_id) {
  auto statement = connection.prepare(
      "SELECT notice_id, target_user_id, notice_type, payload_json, state, "
      "expires_at_ms, opened_at_ms, consumed_at_ms, created_at_ms "
      "FROM pending_notice WHERE notice_id = ?");
  statement.bind(1, notice_id);
  if (!statement.step()) {
    return std::nullopt;
  }
  auto result = notice_record(statement);
  if (statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Pending notice query returned duplicate rows."};
  }
  return result;
}

void expire_notice(SqliteConnection &connection,
                   const std::string_view notice_id,
                   const std::int64_t now_ms) {
  auto statement = connection.prepare(
      "UPDATE pending_notice SET state = 'expired' "
      "WHERE notice_id = ? AND state IN ('pending', 'opened') "
      "AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
  statement.bind(1, notice_id);
  statement.bind(2, now_ms);
  statement.execute();
}

[[nodiscard]] OpenPendingNoticeResult
opened_result(PendingNoticeRecord notice) {
  switch (notice.state) {
  case PendingNoticeState::pending:
  case PendingNoticeState::opened:
    return {.status = OpenPendingNoticeStatus::opened,
            .notice = std::move(notice),
            .delivery_idempotency_key = std::nullopt};
  case PendingNoticeState::expired:
    return {.status = OpenPendingNoticeStatus::expired,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  case PendingNoticeState::consumed:
  case PendingNoticeState::cancelled:
    return {.status = OpenPendingNoticeStatus::unavailable,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  return {.status = OpenPendingNoticeStatus::unavailable,
          .notice = std::nullopt,
          .delivery_idempotency_key = std::nullopt};
}

struct NoticeRevealAttempt {
  std::string interaction_kind;
  DiscordSnowflake target_user_id;
  std::optional<std::string> notice_id;
  std::optional<std::string> token_id;
  std::string result_kind;
  std::string delivery_state;
};

[[nodiscard]] std::optional<NoticeRevealAttempt>
load_reveal_attempt(SqliteConnection &connection,
                    const std::string_view idempotency_key) {
  auto statement = connection.prepare(
      "SELECT interaction_kind, target_user_id, notice_id, token_id, "
      "result_kind, delivery_state FROM notice_reveal_attempt "
      "WHERE idempotency_key = ?");
  statement.bind(1, idempotency_key);
  if (!statement.step()) {
    return std::nullopt;
  }
  NoticeRevealAttempt attempt{
      .interaction_kind = statement.column_text(0),
      .target_user_id = DiscordSnowflake::parse(statement.column_text(1)),
      .notice_id = statement.column_is_null(2)
                       ? std::nullopt
                       : std::optional<std::string>{statement.column_text(2)},
      .token_id = statement.column_is_null(3)
                      ? std::nullopt
                      : std::optional<std::string>{statement.column_text(3)},
      .result_kind = statement.column_text(4),
      .delivery_state = statement.column_text(5),
  };
  if (statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Notice reveal attempt query returned duplicate rows."};
  }
  return attempt;
}

[[nodiscard]] std::int64_t reservation_expiry(const std::int64_t now_ms) {
  if (now_ms > std::numeric_limits<std::int64_t>::max() -
                   reveal_reservation_lifetime_ms) {
    throw std::invalid_argument{"Notice reveal timestamp is too large."};
  }
  return now_ms + reveal_reservation_lifetime_ms;
}

void release_expired_reveal_attempts(SqliteConnection &connection,
                                     const std::int64_t now_ms) {
  auto statement = connection.prepare(
      "UPDATE notice_reveal_attempt SET delivery_state = 'failed', "
      "completed_at_ms = ? WHERE delivery_state = 'prepared' "
      "AND reservation_expires_at_ms <= ?");
  statement.bind(1, now_ms);
  statement.bind(2, now_ms);
  statement.execute();
}

void reprepare_failed_attempt(SqliteConnection &connection,
                              const std::string_view idempotency_key,
                              const std::int64_t now_ms) {
  auto statement = connection.prepare(
      "UPDATE notice_reveal_attempt SET delivery_state = 'prepared', "
      "reservation_expires_at_ms = ?, completed_at_ms = NULL "
      "WHERE idempotency_key = ? AND delivery_state = 'failed'");
  statement.bind(1, reservation_expiry(now_ms));
  statement.bind(2, idempotency_key);
  statement.execute();
}

} // namespace

SqliteRepositoryContext::SqliteRepositoryContext(Database database)
    : database_{std::move(database)} {}

SqliteConnection &SqliteRepositoryContext::connection() noexcept {
  return database_.connection();
}

std::mutex &SqliteRepositoryContext::mutex() noexcept { return mutex_; }

SqliteApplicationInstanceRepository::SqliteApplicationInstanceRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_) {
    throw std::invalid_argument{"SQLite repository context is required."};
  }
}

void SqliteApplicationInstanceRepository::record_start(
    const ApplicationInstanceRecord &record) {
  const std::scoped_lock lock{context_->mutex()};
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
  const std::scoped_lock lock{context_->mutex()};
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
  const std::scoped_lock lock{context_->mutex()};
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
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  ensure_user_uncommitted(user);
  transaction.commit();
}

std::optional<UserPreferences> SqliteCoreIdentityRepository::load_preferences(
    const DiscordSnowflake &user_id) {
  const std::scoped_lock lock{context_->mutex()};
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

SqlitePendingNoticeRepository::SqlitePendingNoticeRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_) {
    throw std::invalid_argument{"SQLite repository context is required."};
  }
}

CreatePendingNoticeResult SqlitePendingNoticeRepository::create_with_token(
    const CreatePendingNoticeRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  if (!valid_uuid_v4(request.notice_id) || !valid_uuid_v4(request.token_id) ||
      !request.target_user_id.is_set() || !request.guild_id.is_set() ||
      !request.channel_id.is_set() || request.notice_type.empty() ||
      request.notice_type.size() > 64 ||
      request.expires_at_ms < request.created_at_ms ||
      request.created_at_ms < 0 ||
      request.source_aggregate_type.has_value() !=
          request.source_aggregate_id.has_value()) {
    throw std::invalid_argument{"Pending notice creation request is invalid."};
  }
  if ((request.source_aggregate_type.has_value() &&
       (request.source_aggregate_type->empty() ||
        request.source_aggregate_type->size() > 64 ||
        request.source_aggregate_id->empty() ||
        request.source_aggregate_id->size() > 128))) {
    throw std::invalid_argument{"Pending notice source is invalid."};
  }
  validate_notice_content(request.content);
  validate_key(request.notice_idempotency_key);
  validate_key(request.token_idempotency_key);
  const auto payload = encode_notice_content(request.content);
  if (payload.size() > 8'192) {
    throw std::invalid_argument{"Pending notice payload is too large."};
  }

  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT n.notice_id, n.target_user_id, n.notice_type, n.payload_json, "
      "n.state, n.expires_at_ms, n.opened_at_ms, n.consumed_at_ms, "
      "n.created_at_ms, t.token_id, t.expected_user_id, t.guild_id, "
      "t.channel_id, t.expires_at_ms "
      "FROM pending_notice n JOIN interaction_token t "
      "ON t.entity_type = 'pending_notice' AND t.entity_id = n.notice_id "
      "AND t.action = 'notice.open' "
      "WHERE n.idempotency_key = ?");
  existing.bind(1, request.notice_idempotency_key);
  if (existing.step()) {
    auto notice = notice_record(existing);
    const auto token_id = existing.column_text(9);
    const bool matches =
        notice.target_user_id == request.target_user_id &&
        notice.notice_type == request.notice_type &&
        notice.content == request.content &&
        notice.expires_at_ms ==
            std::optional<std::int64_t>{request.expires_at_ms} &&
        existing.column_text(10) == request.target_user_id.str() &&
        existing.column_text(11) == request.guild_id.str() &&
        existing.column_text(12) == request.channel_id.str() &&
        existing.column_int64(13) == request.expires_at_ms;
    if (!matches || existing.step()) {
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Pending notice idempotency key conflicts."};
    }
    transaction.commit();
    return {std::move(notice), token_id, false};
  }

  auto notice = connection.prepare(
      "INSERT INTO pending_notice "
      "(notice_id, target_user_id, notice_type, payload_json, "
      "source_aggregate_type, source_aggregate_id, state, expires_at_ms, "
      "idempotency_key, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, 'pending', ?, ?, ?)");
  notice.bind(1, request.notice_id);
  notice.bind(2, request.target_user_id.str());
  notice.bind(3, request.notice_type);
  notice.bind(4, payload);
  if (request.source_aggregate_type.has_value()) {
    notice.bind(5, *request.source_aggregate_type);
    notice.bind(6, *request.source_aggregate_id);
  } else {
    notice.bind_null(5);
    notice.bind_null(6);
  }
  notice.bind(7, request.expires_at_ms);
  notice.bind(8, request.notice_idempotency_key);
  notice.bind(9, request.created_at_ms);
  notice.execute();

  auto token = connection.prepare(
      "INSERT INTO interaction_token "
      "(token_id, token_version, interaction_kind, action, entity_type, "
      "entity_id, expected_user_id, guild_id, channel_id, state, "
      "expires_at_ms, idempotency_key, created_at_ms) "
      "VALUES (?, 1, 'button', 'notice.open', 'pending_notice', ?, ?, ?, ?, "
      "'active', ?, ?, ?)");
  token.bind(1, request.token_id);
  token.bind(2, request.notice_id);
  token.bind(3, request.target_user_id.str());
  token.bind(4, request.guild_id.str());
  token.bind(5, request.channel_id.str());
  token.bind(6, request.expires_at_ms);
  token.bind(7, request.token_idempotency_key);
  token.bind(8, request.created_at_ms);
  token.execute();
  transaction.commit();

  return {PendingNoticeRecord{
              .notice_id = request.notice_id,
              .target_user_id = request.target_user_id,
              .notice_type = request.notice_type,
              .content = request.content,
              .state = PendingNoticeState::pending,
              .expires_at_ms = request.expires_at_ms,
              .opened_at_ms = std::nullopt,
              .consumed_at_ms = std::nullopt,
              .created_at_ms = request.created_at_ms,
          },
          request.token_id, true};
}

OpenPendingNoticeResult SqlitePendingNoticeRepository::open_by_token(
    const OpenNoticeByTokenRequest &request) {
  if (!valid_uuid_v4(request.token_id) || !request.guild_id.is_set() ||
      !request.channel_id.is_set() || !request.user_id.is_set() ||
      request.now_ms < 0) {
    return {.status = OpenPendingNoticeStatus::invalid_token,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  validate_key(request.interaction_idempotency_key);

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  release_expired_reveal_attempts(connection, request.now_ms);
  auto token = connection.prepare(
      "SELECT interaction_kind, action, entity_type, entity_id, "
      "expected_user_id, guild_id, channel_id, state, expires_at_ms "
      "FROM interaction_token WHERE token_id = ? AND token_version = 1");
  token.bind(1, request.token_id);
  if (!token.step()) {
    return {.status = OpenPendingNoticeStatus::invalid_token,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  const auto kind = token.column_text(0);
  const auto action = token.column_text(1);
  const auto entity_type = token.column_text(2);
  const auto notice_id = token.column_text(3);
  const auto expected_user = token.column_text(4);
  const auto guild = token.column_text(5);
  const auto channel = token.column_text(6);
  const auto token_state = token.column_text(7);
  const auto token_expiry = token.column_int64(8);
  if (token.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Interaction token query returned duplicate rows."};
  }
  if (kind != interaction_token_kind_name(request.interaction_kind)) {
    return {.status = OpenPendingNoticeStatus::wrong_kind,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  if (action != "notice.open" || entity_type != "pending_notice") {
    return {.status = OpenPendingNoticeStatus::invalid_token,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  if (guild != request.guild_id.str() || channel != request.channel_id.str()) {
    return {.status = OpenPendingNoticeStatus::wrong_scope,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  if (expected_user != request.user_id.str()) {
    return {.status = OpenPendingNoticeStatus::wrong_user,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  if (token_state == "cancelled") {
    return {.status = OpenPendingNoticeStatus::unavailable,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }
  expire_notice(connection, notice_id, request.now_ms);
  if (token_expiry <= request.now_ms) {
    transaction.commit();
    return {.status = OpenPendingNoticeStatus::expired,
            .notice = std::nullopt,
            .delivery_idempotency_key = std::nullopt};
  }

  auto notice = load_notice(connection, notice_id);
  if (!notice.has_value()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Interaction token target does not exist."};
  }
  auto result = opened_result(*notice);
  if (result.status != OpenPendingNoticeStatus::opened) {
    transaction.commit();
    return result;
  }

  const auto existing =
      load_reveal_attempt(connection, request.interaction_idempotency_key);
  if (existing.has_value()) {
    const bool matches =
        existing->interaction_kind == "button" &&
        existing->target_user_id == request.user_id &&
        existing->notice_id == std::optional<std::string>{notice_id} &&
        existing->token_id == std::optional<std::string>{request.token_id} &&
        existing->result_kind == "notice";
    if (!matches) {
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Notice reveal idempotency key conflicts."};
    }
    reprepare_failed_attempt(connection, request.interaction_idempotency_key,
                             request.now_ms);
  } else {
    auto prepare = connection.prepare(
        "INSERT INTO notice_reveal_attempt "
        "(idempotency_key, interaction_kind, target_user_id, notice_id, "
        "token_id, result_kind, delivery_state, reservation_expires_at_ms, "
        "created_at_ms) VALUES (?, 'button', ?, ?, ?, 'notice', 'prepared', "
        "?, ?)");
    prepare.bind(1, request.interaction_idempotency_key);
    prepare.bind(2, request.user_id.str());
    prepare.bind(3, notice_id);
    prepare.bind(4, request.token_id);
    prepare.bind(5, reservation_expiry(request.now_ms));
    prepare.bind(6, request.now_ms);
    prepare.execute();
  }
  transaction.commit();
  result.delivery_idempotency_key = request.interaction_idempotency_key;
  return result;
}

OpenPendingNoticeResult
SqlitePendingNoticeRepository::open_next(const OpenNextNoticeRequest &request) {
  if (!request.user_id.is_set() || request.now_ms < 0) {
    throw std::invalid_argument{"Pending notice inbox request is invalid."};
  }
  validate_key(request.interaction_idempotency_key);

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  release_expired_reveal_attempts(connection, request.now_ms);
  const auto replay =
      load_reveal_attempt(connection, request.interaction_idempotency_key);
  if (replay.has_value()) {
    if (replay->interaction_kind != "inbox" ||
        replay->target_user_id != request.user_id ||
        replay->token_id.has_value()) {
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Pending notice open idempotency key conflicts."};
    }
    reprepare_failed_attempt(connection, request.interaction_idempotency_key,
                             request.now_ms);
    if (replay->result_kind == "no_pending_notice" &&
        !replay->notice_id.has_value()) {
      transaction.commit();
      return {OpenPendingNoticeStatus::no_pending_notice, std::nullopt,
              request.interaction_idempotency_key};
    }
    if (replay->result_kind != "notice" || !replay->notice_id.has_value()) {
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "Pending notice open result is incompatible."};
    }
    expire_notice(connection, *replay->notice_id, request.now_ms);
    auto notice = load_notice(connection, *replay->notice_id);
    if (!notice.has_value() || notice->target_user_id != request.user_id) {
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "Pending notice open result target is incompatible."};
    }
    auto result = opened_result(std::move(*notice));
    transaction.commit();
    if (result.status == OpenPendingNoticeStatus::opened) {
      result.delivery_idempotency_key = request.interaction_idempotency_key;
    }
    return result;
  }

  auto expire = connection.prepare(
      "UPDATE pending_notice SET state = 'expired' "
      "WHERE target_user_id = ? AND state IN ('pending', 'opened') "
      "AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
  expire.bind(1, request.user_id.str());
  expire.bind(2, request.now_ms);
  expire.execute();

  auto next = connection.prepare(
      "SELECT notice_id, target_user_id, notice_type, payload_json, state, "
      "expires_at_ms, opened_at_ms, consumed_at_ms, created_at_ms "
      "FROM pending_notice WHERE target_user_id = ? AND state = 'pending' "
      "AND NOT EXISTS (SELECT 1 FROM notice_reveal_attempt a "
      "WHERE a.notice_id = pending_notice.notice_id "
      "AND a.delivery_state = 'prepared') "
      "ORDER BY created_at_ms, notice_id LIMIT 1");
  next.bind(1, request.user_id.str());
  if (!next.step()) {
    auto prepare_empty = connection.prepare(
        "INSERT INTO notice_reveal_attempt "
        "(idempotency_key, interaction_kind, target_user_id, notice_id, "
        "token_id, result_kind, delivery_state, reservation_expires_at_ms, "
        "created_at_ms) VALUES (?, 'inbox', ?, NULL, NULL, "
        "'no_pending_notice', 'prepared', ?, ?)");
    prepare_empty.bind(1, request.interaction_idempotency_key);
    prepare_empty.bind(2, request.user_id.str());
    prepare_empty.bind(3, reservation_expiry(request.now_ms));
    prepare_empty.bind(4, request.now_ms);
    prepare_empty.execute();
    transaction.commit();
    return {OpenPendingNoticeStatus::no_pending_notice, std::nullopt,
            request.interaction_idempotency_key};
  }
  auto notice = notice_record(next);
  auto prepare = connection.prepare(
      "INSERT INTO notice_reveal_attempt "
      "(idempotency_key, interaction_kind, target_user_id, notice_id, "
      "token_id, result_kind, delivery_state, reservation_expires_at_ms, "
      "created_at_ms) VALUES (?, 'inbox', ?, ?, NULL, 'notice', 'prepared', "
      "?, ?)");
  prepare.bind(1, request.interaction_idempotency_key);
  prepare.bind(2, request.user_id.str());
  prepare.bind(3, notice.notice_id);
  prepare.bind(4, reservation_expiry(request.now_ms));
  prepare.bind(5, request.now_ms);
  prepare.execute();
  transaction.commit();
  auto result = opened_result(std::move(notice));
  result.delivery_idempotency_key = request.interaction_idempotency_key;
  return result;
}

PendingNoticeMutationStatus
SqlitePendingNoticeRepository::confirm_open_delivery(
    const std::string &interaction_idempotency_key, const std::int64_t now_ms) {
  validate_key(interaction_idempotency_key);
  validate_timestamp(now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto attempt =
      load_reveal_attempt(connection, interaction_idempotency_key);
  if (!attempt.has_value()) {
    return PendingNoticeMutationStatus::not_found;
  }
  if (attempt->delivery_state == "delivered") {
    transaction.commit();
    return PendingNoticeMutationStatus::unchanged;
  }
  if (attempt->delivery_state != "prepared") {
    return PendingNoticeMutationStatus::invalid_state;
  }

  if (attempt->notice_id.has_value()) {
    expire_notice(connection, *attempt->notice_id, now_ms);
    auto notice = load_notice(connection, *attempt->notice_id);
    if (!notice.has_value() ||
        notice->target_user_id != attempt->target_user_id) {
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "Notice reveal target is incompatible."};
    }
    if (notice->state == PendingNoticeState::pending) {
      auto open = connection.prepare(
          "UPDATE pending_notice SET state = 'opened', opened_at_ms = ?, "
          "opened_idempotency_key = ? WHERE notice_id = ? "
          "AND state = 'pending'");
      open.bind(1, now_ms);
      open.bind(2, interaction_idempotency_key);
      open.bind(3, *attempt->notice_id);
      open.execute();
    }
    if (attempt->token_id.has_value()) {
      auto use = connection.prepare(
          "UPDATE interaction_token SET state = 'used', used_at_ms = ? "
          "WHERE token_id = ? AND state = 'active'");
      use.bind(1, now_ms);
      use.bind(2, *attempt->token_id);
      use.execute();
    }
  }

  auto complete = connection.prepare(
      "UPDATE notice_reveal_attempt SET delivery_state = 'delivered', "
      "completed_at_ms = ? WHERE idempotency_key = ? "
      "AND delivery_state = 'prepared'");
  complete.bind(1, now_ms);
  complete.bind(2, interaction_idempotency_key);
  complete.execute();
  transaction.commit();
  return PendingNoticeMutationStatus::applied;
}

PendingNoticeMutationStatus
SqlitePendingNoticeRepository::release_open_delivery(
    const std::string &interaction_idempotency_key, const std::int64_t now_ms) {
  validate_key(interaction_idempotency_key);
  validate_timestamp(now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto attempt =
      load_reveal_attempt(connection, interaction_idempotency_key);
  if (!attempt.has_value()) {
    return PendingNoticeMutationStatus::not_found;
  }
  if (attempt->delivery_state == "failed") {
    transaction.commit();
    return PendingNoticeMutationStatus::unchanged;
  }
  if (attempt->delivery_state != "prepared") {
    return PendingNoticeMutationStatus::invalid_state;
  }
  auto release = connection.prepare(
      "UPDATE notice_reveal_attempt SET delivery_state = 'failed', "
      "completed_at_ms = ? WHERE idempotency_key = ? "
      "AND delivery_state = 'prepared'");
  release.bind(1, now_ms);
  release.bind(2, interaction_idempotency_key);
  release.execute();
  transaction.commit();
  return PendingNoticeMutationStatus::applied;
}

std::size_t SqlitePendingNoticeRepository::recover_incomplete_open_deliveries(
    const std::int64_t now_ms) {
  validate_timestamp(now_ms);
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto statement = context_->connection().prepare(
      "UPDATE notice_reveal_attempt SET delivery_state = 'failed', "
      "completed_at_ms = ? WHERE delivery_state = 'prepared'");
  statement.bind(1, now_ms);
  statement.execute();
  const auto changed = context_->connection().changes();
  transaction.commit();
  return static_cast<std::size_t>(changed);
}

PendingNoticeMutationStatus
SqlitePendingNoticeRepository::consume(const std::string &notice_id,
                                       const DiscordSnowflake &user_id,
                                       const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  if (!valid_uuid_v4(notice_id) || !user_id.is_set() || now_ms < 0) {
    throw std::invalid_argument{"Pending notice consume request is invalid."};
  }
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  expire_notice(connection, notice_id, now_ms);
  const auto notice = load_notice(connection, notice_id);
  if (!notice.has_value()) {
    return PendingNoticeMutationStatus::not_found;
  }
  if (notice->target_user_id != user_id) {
    return PendingNoticeMutationStatus::wrong_user;
  }
  if (notice->state == PendingNoticeState::consumed) {
    transaction.commit();
    return PendingNoticeMutationStatus::unchanged;
  }
  if (notice->state == PendingNoticeState::expired) {
    transaction.commit();
    return PendingNoticeMutationStatus::expired;
  }
  if (notice->state != PendingNoticeState::opened) {
    return PendingNoticeMutationStatus::invalid_state;
  }
  auto update = connection.prepare(
      "UPDATE pending_notice SET state = 'consumed', consumed_at_ms = ? "
      "WHERE notice_id = ? AND state = 'opened'");
  update.bind(1, now_ms);
  update.bind(2, notice_id);
  update.execute();
  transaction.commit();
  return PendingNoticeMutationStatus::applied;
}

PendingNoticeMutationStatus
SqlitePendingNoticeRepository::cancel(const std::string &notice_id,
                                      const DiscordSnowflake &user_id,
                                      const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  if (!valid_uuid_v4(notice_id) || !user_id.is_set() || now_ms < 0) {
    throw std::invalid_argument{"Pending notice cancel request is invalid."};
  }
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  expire_notice(connection, notice_id, now_ms);
  const auto notice = load_notice(connection, notice_id);
  if (!notice.has_value()) {
    return PendingNoticeMutationStatus::not_found;
  }
  if (notice->target_user_id != user_id) {
    return PendingNoticeMutationStatus::wrong_user;
  }
  if (notice->state == PendingNoticeState::cancelled) {
    transaction.commit();
    return PendingNoticeMutationStatus::unchanged;
  }
  if (notice->state == PendingNoticeState::expired) {
    transaction.commit();
    return PendingNoticeMutationStatus::expired;
  }
  if (notice->state == PendingNoticeState::consumed) {
    return PendingNoticeMutationStatus::invalid_state;
  }
  auto update = connection.prepare(
      "UPDATE pending_notice SET state = 'cancelled' "
      "WHERE notice_id = ? AND state IN ('pending', 'opened')");
  update.bind(1, notice_id);
  update.execute();
  auto cancel_token = connection.prepare(
      "UPDATE interaction_token SET state = 'cancelled' "
      "WHERE entity_type = 'pending_notice' AND entity_id = ? "
      "AND state = 'active'");
  cancel_token.bind(1, notice_id);
  cancel_token.execute();
  transaction.commit();
  return PendingNoticeMutationStatus::applied;
}

std::size_t
SqlitePendingNoticeRepository::expire_due(const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  validate_timestamp(now_ms);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto statement = context_->connection().prepare(
      "UPDATE pending_notice SET state = 'expired' "
      "WHERE state IN ('pending', 'opened') AND expires_at_ms IS NOT NULL "
      "AND expires_at_ms <= ?");
  statement.bind(1, now_ms);
  statement.execute();
  const auto changed = context_->connection().changes();
  transaction.commit();
  return static_cast<std::size_t>(changed);
}

std::size_t
SqlitePendingNoticeRepository::pending_count(const DiscordSnowflake &user_id,
                                             const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  if (!user_id.is_set()) {
    throw std::invalid_argument{"Discord user ID must be set."};
  }
  validate_timestamp(now_ms);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto expire = context_->connection().prepare(
      "UPDATE pending_notice SET state = 'expired' "
      "WHERE target_user_id = ? AND state IN ('pending', 'opened') "
      "AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
  expire.bind(1, user_id.str());
  expire.bind(2, now_ms);
  expire.execute();
  auto count = context_->connection().prepare(
      "SELECT count(*) FROM pending_notice "
      "WHERE target_user_id = ? AND state = 'pending'");
  count.bind(1, user_id.str());
  if (!count.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Pending notice count failed."};
  }
  const auto result = count.column_int64(0);
  transaction.commit();
  return static_cast<std::size_t>(result);
}

std::size_t
SqlitePendingNoticeRepository::pending_count_all(const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  validate_timestamp(now_ms);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto expire = context_->connection().prepare(
      "UPDATE pending_notice SET state = 'expired' "
      "WHERE state IN ('pending', 'opened') "
      "AND expires_at_ms IS NOT NULL AND expires_at_ms <= ?");
  expire.bind(1, now_ms);
  expire.execute();
  auto count = context_->connection().prepare(
      "SELECT count(*) FROM pending_notice WHERE state = 'pending'");
  if (!count.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Pending notice count failed."};
  }
  const auto result = count.column_int64(0);
  transaction.commit();
  return static_cast<std::size_t>(result);
}

} // namespace sanguinius::persistence
