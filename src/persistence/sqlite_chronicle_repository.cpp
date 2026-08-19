#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"

#include "sqlite_durable_work_writes.hpp"

#include "sanguinius/pending_notice.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t stale_approval_age_ms = 7LL * 24 * 60 * 60 * 1'000;

[[nodiscard]] std::optional<std::string>
optional_text(SqliteStatement &statement, const int index) {
  return statement.column_is_null(index)
             ? std::nullopt
             : std::optional<std::string>{statement.column_text(index)};
}

[[nodiscard]] std::optional<std::int64_t>
optional_integer(SqliteStatement &statement, const int index) {
  return statement.column_is_null(index)
             ? std::nullopt
             : std::optional<std::int64_t>{statement.column_int64(index)};
}

void bind_optional(SqliteStatement &statement, const std::size_t index,
                   const std::optional<std::string> &value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.bind_null(index);
}

void require_id(const std::string &value) {
  if (!valid_uuid_v4(value))
    throw std::invalid_argument{"Invalid Chronicle ID."};
}

void require_key(const std::string_view value) {
  if (value.empty() || value.size() > 160) {
    throw std::invalid_argument{"Invalid Chronicle idempotency key."};
  }
}

void require_context(const DiscordSnowflake &guild,
                     const DiscordSnowflake &channel,
                     const DiscordSnowflake &actor,
                     const std::string_view correlation_id,
                     const std::int64_t now_ms) {
  if (!guild.is_set() || !channel.is_set() || !actor.is_set() || now_ms < 0 ||
      correlation_id.empty() || correlation_id.size() > 160) {
    throw std::invalid_argument{"Invalid Chronicle request context."};
  }
}

[[nodiscard]] bool valid_tag(const std::string_view tag) {
  return !tag.empty() && tag.size() <= 32 &&
         std::all_of(tag.begin(), tag.end(), [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return (byte >= static_cast<unsigned char>('a') &&
                   byte <= static_cast<unsigned char>('z')) ||
                  (byte >= static_cast<unsigned char>('0') &&
                   byte <= static_cast<unsigned char>('9')) ||
                  character == '_' || character == '-';
         });
}

[[nodiscard]] bool has_tag(const ChronicleEntry &entry,
                           const std::string_view tag) {
  return std::find(entry.tags.begin(), entry.tags.end(), tag) !=
         entry.tags.end();
}

[[nodiscard]] std::string bounded_summary(const std::string_view value,
                                          const std::size_t maximum) {
  if (value.size() <= maximum)
    return std::string{value};
  if (maximum <= 3)
    return std::string{value.substr(0, maximum)};
  auto end = maximum - 3;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return std::string{value.substr(0, end)} + "...";
}

void require_fresh_durable_write(const bool inserted) {
  if (!inserted) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Chronicle durable-write idempotency conflict."};
  }
}

[[nodiscard]] ChronicleEntryType entry_type(const std::string_view value) {
  const auto parsed = parse_chronicle_entry_type(value);
  if (!parsed)
    throw std::runtime_error{"Invalid stored Chronicle entry type."};
  return *parsed;
}

[[nodiscard]] ChronicleVisibility
entry_visibility(const std::string_view value) {
  const auto parsed = parse_chronicle_visibility(value);
  if (!parsed)
    throw std::runtime_error{"Invalid stored Chronicle visibility."};
  return *parsed;
}

[[nodiscard]] ChronicleEntryStatus entry_status(const std::string_view value) {
  if (value == "proposed")
    return ChronicleEntryStatus::proposed;
  if (value == "canon")
    return ChronicleEntryStatus::canon;
  if (value == "retracted")
    return ChronicleEntryStatus::retracted;
  throw std::runtime_error{"Invalid stored Chronicle status."};
}

[[nodiscard]] MemoryStatus memory_status(const std::string_view value) {
  if (value == "confirmed")
    return MemoryStatus::confirmed;
  if (value == "retracted")
    return MemoryStatus::retracted;
  if (value == "expired")
    return MemoryStatus::expired;
  throw std::runtime_error{"Invalid stored memory status."};
}

[[nodiscard]] MemoryVisibility memory_visibility(const std::string_view value) {
  const auto parsed = parse_memory_visibility(value);
  if (!parsed)
    throw std::runtime_error{"Invalid stored memory visibility."};
  return *parsed;
}

[[nodiscard]] MemorySensitivity
memory_sensitivity(const std::string_view value) {
  const auto parsed = parse_memory_sensitivity(value);
  if (!parsed)
    throw std::runtime_error{"Invalid stored memory sensitivity."};
  return *parsed;
}

void ensure_user(SqliteConnection &connection, const ContextUserSnapshot &user,
                 const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO discord_user (user_id, display_name_cache, username_cache, "
      "is_bot, first_seen_at_ms, last_seen_at_ms, created_at_ms, "
      "updated_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(user_id) DO UPDATE SET "
      "display_name_cache=excluded.display_name_cache, "
      "username_cache=excluded.username_cache, "
      "last_seen_at_ms=max(discord_user.last_seen_at_ms, "
      "excluded.last_seen_at_ms), "
      "updated_at_ms=max(discord_user.updated_at_ms, excluded.updated_at_ms)");
  insert.bind(1, user.user_id.str());
  if (user.display_name.empty())
    insert.bind_null(2);
  else
    insert.bind(2, user.display_name);
  if (user.username.empty())
    insert.bind_null(3);
  else
    insert.bind(3, user.username);
  insert.bind(4, static_cast<std::int64_t>(user.is_bot));
  insert.bind(5, now_ms);
  insert.bind(6, now_ms);
  insert.bind(7, now_ms);
  insert.bind(8, now_ms);
  insert.execute();
  auto preference = connection.prepare(
      "INSERT INTO user_preference (user_id, updated_at_ms) VALUES (?, ?) "
      "ON CONFLICT(user_id) DO NOTHING");
  preference.bind(1, user.user_id.str());
  preference.bind(2, now_ms);
  preference.execute();
}

[[nodiscard]] bool opted_in(SqliteConnection &connection,
                            const DiscordSnowflake &user) {
  auto query = connection.prepare(
      "SELECT chronicle_opt_in FROM user_preference WHERE user_id=?");
  query.bind(1, user.str());
  return query.step() && query.column_int64(0) == 1;
}

[[nodiscard]] ChronicleEntry read_entry_base(SqliteStatement &row) {
  return ChronicleEntry{
      .entry_id = row.column_text(0),
      .type = entry_type(row.column_text(1)),
      .title = row.column_text(2),
      .body = row.column_text(3),
      .visibility = entry_visibility(row.column_text(4)),
      .status = entry_status(row.column_text(5)),
      .created_by_user_id = DiscordSnowflake::parse(row.column_text(6)),
      .source_author_user_id = DiscordSnowflake::parse(row.column_text(7)),
      .source_guild_id = DiscordSnowflake::parse(row.column_text(8)),
      .source_channel_id = DiscordSnowflake::parse(row.column_text(9)),
      .source_message_id = DiscordSnowflake::parse(row.column_text(10)),
      .source_text = row.column_text(11),
      .source_text_truncated = row.column_int64(12) != 0,
      .occurred_at_ms = row.column_int64(13),
      .created_at_ms = row.column_int64(14),
      .submitted_at_ms = optional_integer(row, 15),
      .approved_at_ms = optional_integer(row, 16),
      .retracted_at_ms = optional_integer(row, 17),
      .revision = static_cast<std::size_t>(row.column_int64(18)),
  };
}

constexpr std::string_view entry_columns =
    "entry_id, entry_type, title, body, visibility, status, "
    "created_by_user_id, source_author_user_id, source_guild_id, "
    "source_channel_id, source_message_id, source_text, source_text_truncated, "
    "occurred_at_ms, created_at_ms, submitted_at_ms, approved_at_ms, "
    "retracted_at_ms, revision";

void enrich_entry(SqliteConnection &connection, ChronicleEntry &entry) {
  auto participants = connection.prepare(
      "SELECT DISTINCT user_id FROM chronicle_participant WHERE entry_id=? "
      "ORDER BY user_id");
  participants.bind(1, entry.entry_id);
  while (participants.step()) {
    entry.participants.push_back(
        DiscordSnowflake::parse(participants.column_text(0)));
  }
  auto tags = connection.prepare(
      "SELECT tag FROM chronicle_tag WHERE entry_id=? ORDER BY tag");
  tags.bind(1, entry.entry_id);
  while (tags.step())
    entry.tags.push_back(tags.column_text(0));
  auto attachments = connection.prepare(
      "SELECT attachment_id, filename, content_type, byte_size, width, height, "
      "is_ephemeral, is_spoiler FROM chronicle_attachment WHERE entry_id=? "
      "ORDER BY position");
  attachments.bind(1, entry.entry_id);
  while (attachments.step()) {
    entry.attachments.push_back(ChronicleAttachment{
        .attachment_id = DiscordSnowflake::parse(attachments.column_text(0)),
        .filename = attachments.column_text(1),
        .content_type = optional_text(attachments, 2),
        .byte_size = static_cast<std::uint64_t>(attachments.column_int64(3)),
        .width = attachments.column_is_null(4)
                     ? std::nullopt
                     : std::optional{static_cast<std::uint32_t>(
                           attachments.column_int64(4))},
        .height = attachments.column_is_null(5)
                      ? std::nullopt
                      : std::optional{static_cast<std::uint32_t>(
                            attachments.column_int64(5))},
        .ephemeral = attachments.column_int64(6) != 0,
        .spoiler = attachments.column_int64(7) != 0,
    });
  }
}

[[nodiscard]] std::optional<ChronicleEntry>
load_entry(SqliteConnection &connection, const std::string_view entry_id) {
  auto query = connection.prepare("SELECT " + std::string{entry_columns} +
                                  " FROM chronicle_entry WHERE entry_id=?");
  query.bind(1, entry_id);
  if (!query.step())
    return std::nullopt;
  auto result = read_entry_base(query);
  enrich_entry(connection, result);
  return result;
}

[[nodiscard]] ExplicitMemory read_memory(SqliteStatement &row) {
  return ExplicitMemory{
      .memory_id = row.column_text(0),
      .text = row.column_text(1),
      .visibility = memory_visibility(row.column_text(2)),
      .sensitivity = memory_sensitivity(row.column_text(3)),
      .status = memory_status(row.column_text(4)),
      .subject_user_id = DiscordSnowflake::parse(row.column_text(5)),
      .created_by_user_id = DiscordSnowflake::parse(row.column_text(6)),
      .created_at_ms = row.column_int64(7),
      .expires_at_ms = optional_integer(row, 8),
      .revision = static_cast<std::size_t>(row.column_int64(9)),
  };
}

[[nodiscard]] EventJournalEntry
event_for(std::string id, std::string type, std::string aggregate_type,
          std::string aggregate_id, const DiscordSnowflake actor,
          const DiscordSnowflake guild, const DiscordSnowflake channel,
          std::optional<DiscordSnowflake> source_message,
          const std::int64_t now_ms, std::string correlation,
          std::string idempotency, Json payload) {
  return EventJournalEntry{
      .event_id = std::move(id),
      .event_type = std::move(type),
      .aggregate_type = std::move(aggregate_type),
      .aggregate_id = std::move(aggregate_id),
      .actor_user_id = actor,
      .guild_id = guild,
      .channel_id = channel,
      .source_message_id = source_message,
      .occurred_at_ms = now_ms,
      .recorded_at_ms = now_ms,
      .correlation_id = std::move(correlation),
      .causation_id = std::nullopt,
      .idempotency_key = std::move(idempotency),
      .payload_json = payload.dump(),
  };
}

void insert_token(SqliteConnection &connection, const std::string &token_id,
                  const std::string_view kind, const std::string_view action,
                  const std::string_view entity_type,
                  const std::string_view entity_id,
                  const DiscordSnowflake &expected_user,
                  const DiscordSnowflake &guild,
                  const DiscordSnowflake &channel, const std::size_t revision,
                  const std::int64_t expires_at_ms,
                  const std::string &idempotency_key,
                  const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO interaction_token (token_id, token_version, "
      "interaction_kind, "
      "action, entity_type, entity_id, expected_user_id, guild_id, channel_id, "
      "state, expires_at_ms, idempotency_key, created_at_ms, "
      "expected_entity_revision) VALUES (?,1,?,?,?,?,?,?,?,'active',?,?,?,?)");
  insert.bind(1, token_id);
  insert.bind(2, kind);
  insert.bind(3, action);
  insert.bind(4, entity_type);
  insert.bind(5, entity_id);
  insert.bind(6, expected_user.str());
  insert.bind(7, guild.str());
  insert.bind(8, channel.str());
  insert.bind(9, expires_at_ms);
  insert.bind(10, idempotency_key);
  insert.bind(11, now_ms);
  insert.bind(12, static_cast<std::int64_t>(revision));
  insert.execute();
}

struct TokenRecord {
  std::string action;
  std::string entity_type;
  std::string entity_id;
  std::size_t expected_revision{};
};

[[nodiscard]] std::optional<TokenRecord>
load_active_token(SqliteConnection &connection, const std::string &token_id,
                  const DiscordSnowflake &guild,
                  const DiscordSnowflake &channel,
                  const DiscordSnowflake &actor, const std::string_view kind,
                  const std::int64_t now_ms) {
  auto query = connection.prepare(
      "SELECT action, entity_type, entity_id, expected_entity_revision FROM "
      "interaction_token WHERE token_id=? AND interaction_kind=? AND "
      "expected_user_id=? AND guild_id=? AND channel_id=? AND state='active' "
      "AND expires_at_ms>?");
  query.bind(1, token_id);
  query.bind(2, kind);
  query.bind(3, actor.str());
  query.bind(4, guild.str());
  query.bind(5, channel.str());
  query.bind(6, now_ms);
  if (!query.step() || query.column_is_null(3))
    return std::nullopt;
  return TokenRecord{.action = query.column_text(0),
                     .entity_type = query.column_text(1),
                     .entity_id = query.column_text(2),
                     .expected_revision =
                         static_cast<std::size_t>(query.column_int64(3))};
}

[[nodiscard]] std::optional<TokenRecord>
load_expired_token(SqliteConnection &connection, const std::string &token_id,
                   const DiscordSnowflake &guild,
                   const DiscordSnowflake &channel,
                   const DiscordSnowflake &actor, const std::string_view kind,
                   const std::int64_t now_ms) {
  auto query = connection.prepare(
      "SELECT action, entity_type, entity_id, expected_entity_revision FROM "
      "interaction_token WHERE token_id=? AND interaction_kind=? AND "
      "expected_user_id=? AND guild_id=? AND channel_id=? AND state='active' "
      "AND expires_at_ms<=?");
  query.bind(1, token_id);
  query.bind(2, kind);
  query.bind(3, actor.str());
  query.bind(4, guild.str());
  query.bind(5, channel.str());
  query.bind(6, now_ms);
  if (!query.step() || query.column_is_null(3))
    return std::nullopt;
  return TokenRecord{.action = query.column_text(0),
                     .entity_type = query.column_text(1),
                     .entity_id = query.column_text(2),
                     .expected_revision =
                         static_cast<std::size_t>(query.column_int64(3))};
}

[[nodiscard]] std::optional<TokenRecord>
load_used_token(SqliteConnection &connection, const std::string &token_id,
                const DiscordSnowflake &guild, const DiscordSnowflake &channel,
                const DiscordSnowflake &actor, const std::string_view kind) {
  auto query = connection.prepare(
      "SELECT action, entity_type, entity_id, expected_entity_revision FROM "
      "interaction_token WHERE token_id=? AND interaction_kind=? AND "
      "expected_user_id=? AND guild_id=? AND channel_id=? AND state='used'");
  query.bind(1, token_id);
  query.bind(2, kind);
  query.bind(3, actor.str());
  query.bind(4, guild.str());
  query.bind(5, channel.str());
  if (!query.step() || query.column_is_null(3))
    return std::nullopt;
  return TokenRecord{.action = query.column_text(0),
                     .entity_type = query.column_text(1),
                     .entity_id = query.column_text(2),
                     .expected_revision =
                         static_cast<std::size_t>(query.column_int64(3))};
}

void use_token(SqliteConnection &connection, const std::string &token_id,
               const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE interaction_token SET state='used', used_at_ms=? "
      "WHERE token_id=? AND state='active'");
  update.bind(1, now_ms);
  update.bind(2, token_id);
  update.execute();
}

[[nodiscard]] std::string notice_payload(const PendingNoticeContent &content) {
  Json actions = Json::array();
  for (const auto &action : content.actions) {
    actions.push_back(
        {{"custom_id", action.custom_id}, {"label", action.label}});
  }
  return Json{{"title", content.title},
              {"body", content.body},
              {"actions", std::move(actions)}}
      .dump();
}

void insert_notice(SqliteConnection &connection,
                   const CreatePendingNoticeRequest &request) {
  auto notice = connection.prepare(
      "INSERT INTO pending_notice "
      "(notice_id,target_user_id,notice_type,payload_json,"
      "source_aggregate_type,source_aggregate_id,state,expires_at_ms,"
      "idempotency_key,created_at_ms) VALUES (?,?,?,?,?,?,'pending',?,?,?)");
  notice.bind(1, request.notice_id);
  notice.bind(2, request.target_user_id.str());
  notice.bind(3, request.notice_type);
  notice.bind(4, notice_payload(request.content));
  bind_optional(notice, 5, request.source_aggregate_type);
  bind_optional(notice, 6, request.source_aggregate_id);
  notice.bind(7, request.expires_at_ms);
  notice.bind(8, request.notice_idempotency_key);
  notice.bind(9, request.created_at_ms);
  notice.execute();
  insert_token(connection, request.token_id, "button", "notice.open",
               "pending_notice", request.notice_id, request.target_user_id,
               request.guild_id, request.channel_id, 1, request.expires_at_ms,
               request.token_idempotency_key, request.created_at_ms);
}

[[nodiscard]] PendingNoticeContent
approval_notice_content(const ChronicleEntry &entry) {
  const bool owner_test = has_tag(entry, "owner-test");
  return PendingNoticeContent{
      .title = "Chronicle approval requested",
      .body = (owner_test ? "**TEST DATA — OWNER SELF-APPROVAL**\n"
                          : std::string{}) +
              "**" + entry.title + "**\n" + bounded_summary(entry.body, 180) +
              "\n" + render_chronicle_provenance(entry, 1'100) +
              "\nApprove only if this should become canon.",
      .actions = {},
  };
}

[[nodiscard]] InteractionMessage
approval_notice_card(const ChronicleEntry &entry,
                     const DiscordSnowflake &reviewer,
                     const std::string_view notice_open_token_id) {
  const bool owner_test = has_tag(entry, "owner-test");
  return InteractionMessage{
      .content = (owner_test ? "**[TEST DATA]** " : std::string{}) + "<@" +
                 reviewer.str() + ">, a sealed notice awaits.",
      .embed =
          EmbedPayload{
              .color = 0x8B0000U,
              .title = owner_test ? "TEST DATA — sealed notice"
                                  : "A sealed notice awaits",
              .description =
                  owner_test ? "A test-mode private Chronicle decision awaits "
                               "the owner."
                             : "A private Chronicle decision awaits the "
                               "addressed recipient."},
      .buttons = {ButtonPayload{.custom_id =
                                    make_component_id(notice_open_token_id),
                                .label = "Open sealed notice"}},
      .allowed_user_mentions = {reviewer}};
}

void insert_public_outbox(
    SqliteConnection &connection, const std::string &outbox_id,
    const DiscordSnowflake &guild, const DiscordSnowflake &channel,
    const std::string &aggregate_id, const std::string &idempotency_key,
    const std::int64_t now_ms, const std::string &correlation_id,
    const std::optional<std::string> &causation, InteractionMessage message) {
  auto latest =
      connection.prepare("SELECT max(created_at_ms) FROM outbox_message WHERE "
                         "aggregate_type='chronicle_entry' AND aggregate_id=?");
  latest.bind(1, aggregate_id);
  if (!latest.step()) {
    throw std::runtime_error{"Chronicle outbox ordering query failed."};
  }
  auto ordered_at_ms = now_ms;
  if (!latest.column_is_null(0) && latest.column_int64(0) >= ordered_at_ms) {
    const auto previous = latest.column_int64(0);
    if (previous == std::numeric_limits<std::int64_t>::max()) {
      throw std::overflow_error{"Chronicle outbox timestamp overflow."};
    }
    ordered_at_ms = previous + 1;
  }
  const PublicOutboxPayload payload{
      .request = PublicMessageRequest{.guild_id = guild,
                                      .channel_id = channel,
                                      .message = std::move(message)}};
  require_fresh_durable_write(detail::insert_outbox_uncommitted(
      connection,
      OutboxEnqueue{.outbox_id = outbox_id,
                    .kind = std::string{public_discord_outbox_kind},
                    .aggregate_type = "chronicle_entry",
                    .aggregate_id = aggregate_id,
                    .target_guild_id = guild,
                    .target_channel_id = channel,
                    .target_user_id = std::nullopt,
                    .available_at_ms = ordered_at_ms,
                    .max_attempts = 5,
                    .idempotency_key = idempotency_key,
                    .provider_nonce = discord_nonce_from_uuid(outbox_id),
                    .created_at_ms = ordered_at_ms},
      detail::encode_public_payload(payload, correlation_id, causation)));
}

void cancel_approval_artifacts(SqliteConnection &connection,
                               const std::string_view entry_id,
                               const std::int64_t now_ms) {
  auto cancel_tokens = connection.prepare(
      "UPDATE interaction_token SET state='cancelled' WHERE state='active' "
      "AND ((entity_type='chronicle_entry' AND entity_id=?) OR "
      "(entity_type='chronicle_approval' AND entity_id IN "
      "(SELECT approval_id FROM chronicle_approval WHERE entry_id=?)) OR "
      "(entity_type='pending_notice' AND entity_id IN "
      "(SELECT notice_id FROM chronicle_approval WHERE entry_id=? AND "
      "notice_id IS NOT NULL)))");
  cancel_tokens.bind(1, entry_id);
  cancel_tokens.bind(2, entry_id);
  cancel_tokens.bind(3, entry_id);
  cancel_tokens.execute();

  auto cancel_notices = connection.prepare(
      "UPDATE pending_notice SET state='cancelled' WHERE state IN "
      "('pending','opened') AND notice_id IN (SELECT notice_id FROM "
      "chronicle_approval WHERE entry_id=? AND notice_id IS NOT NULL)");
  cancel_notices.bind(1, entry_id);
  cancel_notices.execute();

  auto cancel_announcements = connection.prepare(
      "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms) "
      "WHERE aggregate_type='chronicle_entry' AND aggregate_id=? AND "
      "idempotency_key LIKE 'outbox:chronicle:notice:%' AND "
      "(state='pending' OR (state='claimed' AND "
      "submission_started_at_ms IS NULL))");
  cancel_announcements.bind(1, now_ms);
  cancel_announcements.bind(2, now_ms);
  cancel_announcements.bind(3, entry_id);
  cancel_announcements.execute();
}

void cancel_notice_announcement(SqliteConnection &connection,
                                const std::string_view entry_id,
                                const std::string_view approval_id,
                                const std::int64_t now_ms) {
  auto cancel = connection.prepare(
      "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms) "
      "WHERE aggregate_type='chronicle_entry' AND aggregate_id=? AND "
      "idempotency_key=? AND (state='pending' OR (state='claimed' AND "
      "submission_started_at_ms IS NULL))");
  cancel.bind(1, now_ms);
  cancel.bind(2, now_ms);
  cancel.bind(3, entry_id);
  cancel.bind(4, "outbox:chronicle:notice:" + std::string{approval_id});
  cancel.execute();
}

void cancel_renewed_notice_announcement(SqliteConnection &connection,
                                        const std::string_view entry_id,
                                        const std::string_view notice_id,
                                        const std::int64_t now_ms) {
  auto cancel = connection.prepare(
      "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms) "
      "WHERE aggregate_type='chronicle_entry' AND aggregate_id=? AND "
      "idempotency_key=? AND (state='pending' OR (state='claimed' AND "
      "submission_started_at_ms IS NULL))");
  cancel.bind(1, now_ms);
  cancel.bind(2, now_ms);
  cancel.bind(3, entry_id);
  cancel.bind(4, "outbox:chronicle:notice:renewed:" + std::string{notice_id});
  cancel.execute();
}

[[nodiscard]] bool event_replayed(SqliteConnection &connection,
                                  const std::string_view key,
                                  const std::string_view event_type,
                                  const std::string_view aggregate_id,
                                  const DiscordSnowflake &actor,
                                  const DiscordSnowflake &guild,
                                  const DiscordSnowflake &channel) {
  auto query = connection.prepare(
      "SELECT event_type,aggregate_id,actor_user_id,guild_id,channel_id FROM "
      "event_journal WHERE idempotency_key=?");
  query.bind(1, key);
  if (!query.step())
    return false;
  if (query.column_text(0) != event_type ||
      query.column_text(1) != aggregate_id ||
      optional_text(query, 2) != std::optional<std::string>{actor.str()} ||
      query.column_text(3) != guild.str() ||
      optional_text(query, 4) != std::optional<std::string>{channel.str()}) {
    throw DatabaseError{
        DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT, SQLITE_CONSTRAINT,
        "Chronicle idempotency key conflicts with existing data."};
  }
  return true;
}

} // namespace

SqliteChronicleRepository::SqliteChronicleRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite context is required."};
}

ProposalResult SqliteChronicleRepository::create_or_get_proposal(
    const CreateProposalRequest &request) {
  require_id(request.entry_id);
  require_id(request.event_id);
  require_id(request.actions.edit_token_id);
  require_id(request.actions.submit_token_id);
  require_id(request.actions.retract_token_id);
  require_key(request.idempotency_key);
  require_context(request.source.reference.guild_id,
                  request.source.reference.channel_id, request.proposer_user_id,
                  request.correlation_id, request.now_ms);
  if (!request.proposer_user_id.is_set() ||
      !request.source.reference.guild_id.is_set() ||
      !request.source.reference.channel_id.is_set() ||
      !request.source.reference.message_id.is_set() ||
      !request.source.author.user_id.is_set() || request.source.author.is_bot ||
      request.now_ms < 0 || request.action_expires_at_ms <= request.now_ms ||
      !valid_chronicle_text(request.title, maximum_chronicle_title_size) ||
      !valid_chronicle_text(request.body, maximum_chronicle_body_size) ||
      request.source.occurred_at_ms < 0 ||
      !valid_chronicle_snapshot_text(request.source.content,
                                     maximum_chronicle_source_size) ||
      request.source.attachments.size() > maximum_chronicle_attachments ||
      request.source.mentioned_users.size() > maximum_chronicle_mentions) {
    throw std::invalid_argument{"Invalid Chronicle proposal."};
  }
  for (const auto &attachment : request.source.attachments) {
    if (!attachment.attachment_id.is_set() ||
        !valid_chronicle_text(attachment.filename, 255) ||
        (attachment.content_type &&
         !valid_chronicle_text(*attachment.content_type, 127)) ||
        attachment.byte_size > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
      throw std::invalid_argument{"Invalid Chronicle attachment metadata."};
    }
  }

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  ensure_user(connection, request.source.author, request.now_ms);
  for (const auto &participant : request.source.mentioned_users) {
    if (participant.user_id.is_set())
      ensure_user(connection, participant, request.now_ms);
  }
  if (!opted_in(connection, request.proposer_user_id) ||
      !opted_in(connection, request.source.author.user_id)) {
    transaction.commit();
    return {.code = ChronicleResultCode::opted_out};
  }
  if (request.visibility == ChronicleVisibility::participant_only) {
    for (const auto &participant : request.source.mentioned_users) {
      if (!participant.is_bot && participant.user_id.is_set() &&
          !opted_in(connection, participant.user_id)) {
        transaction.commit();
        return {.code = ChronicleResultCode::opted_out};
      }
    }
  }

  auto existing = connection.prepare(
      "SELECT entry_id FROM chronicle_entry WHERE source_guild_id=? AND "
      "source_channel_id=? AND source_message_id=?");
  existing.bind(1, request.source.reference.guild_id.str());
  existing.bind(2, request.source.reference.channel_id.str());
  existing.bind(3, request.source.reference.message_id.str());
  if (existing.step()) {
    const auto entry_id = existing.column_text(0);
    auto entry = load_entry(connection, entry_id);
    std::optional<ProposalActionIds> actions;
    ProposalControlMode control_mode =
        entry && entry->submitted_at_ms
            ? ProposalControlMode::awaiting_confirmations
            : ProposalControlMode::edit_submit_retract;
    if (entry && entry->status == ChronicleEntryStatus::proposed &&
        !entry->submitted_at_ms &&
        entry->created_by_user_id == request.proposer_user_id) {
      auto cancel = connection.prepare(
          "UPDATE interaction_token SET state='cancelled' WHERE "
          "entity_type='chronicle_entry' AND entity_id=? AND state='active'");
      cancel.bind(1, entry_id);
      cancel.execute();
      insert_token(connection, request.actions.edit_token_id, "modal",
                   "chronicle.entry.edit", "chronicle_entry", entry_id,
                   request.proposer_user_id, request.source.reference.guild_id,
                   request.source.reference.channel_id, entry->revision,
                   request.action_expires_at_ms,
                   "token:chronicle:edit:" + request.actions.edit_token_id,
                   request.now_ms);
      insert_token(connection, request.actions.submit_token_id, "button",
                   "chronicle.entry.submit", "chronicle_entry", entry_id,
                   request.proposer_user_id, request.source.reference.guild_id,
                   request.source.reference.channel_id, entry->revision,
                   request.action_expires_at_ms,
                   "token:chronicle:submit:" + request.actions.submit_token_id,
                   request.now_ms);
      insert_token(connection, request.actions.retract_token_id, "button",
                   "chronicle.entry.retract", "chronicle_entry", entry_id,
                   request.proposer_user_id, request.source.reference.guild_id,
                   request.source.reference.channel_id, entry->revision,
                   request.action_expires_at_ms,
                   "token:chronicle:retract:" +
                       request.actions.retract_token_id,
                   request.now_ms);
      actions = request.actions;
    } else if (entry && entry->status == ChronicleEntryStatus::proposed &&
               entry->submitted_at_ms &&
               entry->visibility == ChronicleVisibility::shared &&
               request.owner_user_id.is_set() &&
               entry->created_by_user_id != request.owner_user_id &&
               request.proposer_user_id == request.owner_user_id) {
      auto stale = connection.prepare(
          "SELECT approval_id,approval_role,notice_id FROM chronicle_approval "
          "WHERE entry_id=? AND state='pending' AND "
          "(approval_role='owner_stale' OR requested_at_ms<=?) "
          "ORDER BY requested_at_ms,approval_id LIMIT 1");
      stale.bind(1, entry_id);
      stale.bind(2, request.now_ms - stale_approval_age_ms);
      if (stale.step()) {
        auto approval_id = stale.column_text(0);
        const auto replaced_approval_id = approval_id;
        const auto approval_role = stale.column_text(1);
        const auto notice_id = optional_text(stale, 2);
        auto cancel_tokens = connection.prepare(
            "UPDATE interaction_token SET state='cancelled' WHERE "
            "entity_type='chronicle_approval' AND entity_id=? AND "
            "state='active'");
        cancel_tokens.bind(1, approval_id);
        cancel_tokens.execute();
        if (notice_id) {
          auto cancel_notice = connection.prepare(
              "UPDATE pending_notice SET state='cancelled' WHERE notice_id=? "
              "AND state IN ('pending','opened')");
          cancel_notice.bind(1, *notice_id);
          cancel_notice.execute();
          auto cancel_notice_token = connection.prepare(
              "UPDATE interaction_token SET state='cancelled' WHERE "
              "entity_type='pending_notice' AND entity_id=? AND "
              "state='active'");
          cancel_notice_token.bind(1, *notice_id);
          cancel_notice_token.execute();
          cancel_renewed_notice_announcement(connection, entry_id, *notice_id,
                                             request.now_ms);
        }
        cancel_notice_announcement(connection, entry_id, replaced_approval_id,
                                   request.now_ms);
        if (approval_role == "owner_stale") {
          auto renew = connection.prepare(
              "UPDATE chronicle_approval SET requested_at_ms=?,notice_id=NULL "
              "WHERE approval_id=? AND state='pending'");
          renew.bind(1, request.now_ms);
          renew.bind(2, approval_id);
          renew.execute();
        } else {
          auto cancel = connection.prepare(
              "UPDATE chronicle_approval SET state='cancelled',acted_at_ms=? "
              "WHERE approval_id=? AND state='pending'");
          cancel.bind(1, request.now_ms);
          cancel.bind(2, approval_id);
          cancel.execute();
          approval_id = request.actions.edit_token_id;
          auto owner_approval = connection.prepare(
              "INSERT INTO chronicle_approval (approval_id,entry_id,"
              "reviewer_user_id,approval_role,state,entry_revision,requested_"
              "at_ms) "
              "VALUES (?,?,?,'owner_stale','pending',?,?)");
          owner_approval.bind(1, approval_id);
          owner_approval.bind(2, entry_id);
          owner_approval.bind(3, request.owner_user_id.str());
          owner_approval.bind(4, static_cast<std::int64_t>(entry->revision));
          owner_approval.bind(5, request.now_ms);
          owner_approval.execute();
        }
        insert_token(
            connection, request.actions.submit_token_id, "button",
            "chronicle.entry.approve", "chronicle_approval", approval_id,
            request.owner_user_id, request.source.reference.guild_id,
            request.source.reference.channel_id, entry->revision,
            request.action_expires_at_ms,
            "token:chronicle:stale-approve:" + request.actions.submit_token_id,
            request.now_ms);
        insert_token(
            connection, request.actions.retract_token_id, "button",
            "chronicle.entry.decline", "chronicle_approval", approval_id,
            request.owner_user_id, request.source.reference.guild_id,
            request.source.reference.channel_id, entry->revision,
            request.action_expires_at_ms,
            "token:chronicle:stale-decline:" + request.actions.retract_token_id,
            request.now_ms);
        require_fresh_durable_write(detail::insert_event_uncommitted(
            connection,
            event_for(request.event_id,
                      "chronicle.owner_stale_resolution_requested.v1",
                      "chronicle_entry", entry_id, request.owner_user_id,
                      request.source.reference.guild_id,
                      request.source.reference.channel_id,
                      request.source.reference.message_id, request.now_ms,
                      request.correlation_id,
                      "event:chronicle:owner-stale:" +
                          request.actions.submit_token_id,
                      Json{{"entry_id", entry_id},
                           {"status", "proposed"},
                           {"visibility", "shared"},
                           {"revision", entry->revision},
                           {"reason", "required_reviewer_stale"}})));
        actions = request.actions;
        control_mode = ProposalControlMode::owner_stale_resolution;
      }
    } else if (entry && entry->status == ChronicleEntryStatus::proposed &&
               entry->submitted_at_ms &&
               entry->created_by_user_id == request.proposer_user_id) {
      struct PendingRenewal {
        std::string approval_id;
        DiscordSnowflake reviewer;
        std::optional<std::string> notice_id;
      };
      std::vector<PendingRenewal> renewals;
      auto stale = connection.prepare(
          "SELECT approval_id,reviewer_user_id,notice_id FROM "
          "chronicle_approval WHERE entry_id=? AND state='pending' AND "
          "requested_at_ms<=? ORDER BY requested_at_ms,approval_id");
      stale.bind(1, entry_id);
      stale.bind(2, request.now_ms - stale_approval_age_ms);
      while (stale.step()) {
        renewals.push_back(PendingRenewal{
            .approval_id = stale.column_text(0),
            .reviewer = DiscordSnowflake::parse(stale.column_text(1)),
            .notice_id = optional_text(stale, 2),
        });
      }
      if (!renewals.empty()) {
        if (renewals.size() > maximum_chronicle_mentions + 2 ||
            request.renewal_dispatches.size() < renewals.size() ||
            request.notice_expires_at_ms <= request.now_ms) {
          throw std::invalid_argument{
              "Invalid Chronicle approval renewal dispatch."};
        }
        if (!opted_in(connection, entry->created_by_user_id) ||
            !opted_in(connection, entry->source_author_user_id)) {
          transaction.commit();
          return {.code = ChronicleResultCode::opted_out,
                  .entry = std::move(entry),
                  .control_mode = control_mode};
        }
        if (entry->visibility == ChronicleVisibility::participant_only &&
            !std::all_of(entry->participants.begin(), entry->participants.end(),
                         [&connection](const DiscordSnowflake &participant) {
                           return opted_in(connection, participant);
                         })) {
          transaction.commit();
          return {.code = ChronicleResultCode::opted_out,
                  .entry = std::move(entry),
                  .control_mode = control_mode};
        }
        for (std::size_t index = 0; index < renewals.size(); ++index) {
          const auto &dispatch = request.renewal_dispatches[index];
          require_id(dispatch.notice_id);
          require_id(dispatch.notice_open_token_id);
          require_id(dispatch.approve_token_id);
          require_id(dispatch.decline_token_id);
          require_id(dispatch.notice_event_id);
          require_id(dispatch.notice_outbox_id);
          if (!opted_in(connection, renewals[index].reviewer)) {
            transaction.commit();
            return {.code = ChronicleResultCode::opted_out,
                    .entry = std::move(entry),
                    .control_mode = control_mode};
          }
        }
        bool wake_outbox = false;
        for (std::size_t index = 0; index < renewals.size(); ++index) {
          const auto &renewal = renewals[index];
          const auto &dispatch = request.renewal_dispatches[index];

          auto cancel_approval_tokens = connection.prepare(
              "UPDATE interaction_token SET state='cancelled' WHERE "
              "entity_type='chronicle_approval' AND entity_id=? AND "
              "state='active'");
          cancel_approval_tokens.bind(1, renewal.approval_id);
          cancel_approval_tokens.execute();
          cancel_notice_announcement(connection, entry_id, renewal.approval_id,
                                     request.now_ms);
          if (renewal.notice_id) {
            auto cancel_notice = connection.prepare(
                "UPDATE pending_notice SET state='cancelled' WHERE "
                "notice_id=? AND state IN ('pending','opened')");
            cancel_notice.bind(1, *renewal.notice_id);
            cancel_notice.execute();
            auto cancel_notice_token = connection.prepare(
                "UPDATE interaction_token SET state='cancelled' WHERE "
                "entity_type='pending_notice' AND entity_id=? AND "
                "state='active'");
            cancel_notice_token.bind(1, *renewal.notice_id);
            cancel_notice_token.execute();
            cancel_renewed_notice_announcement(
                connection, entry_id, *renewal.notice_id, request.now_ms);
          }

          auto content = approval_notice_content(*entry);
          content.actions = {
              PendingNoticeContent::Action{
                  .custom_id = make_chronicle_component(
                      chronicle_component_prefix, dispatch.approve_token_id),
                  .label = "Approve"},
              PendingNoticeContent::Action{
                  .custom_id = make_chronicle_component(
                      chronicle_component_prefix, dispatch.decline_token_id),
                  .label = "Decline"}};
          insert_notice(
              connection,
              CreatePendingNoticeRequest{
                  .notice_id = dispatch.notice_id,
                  .token_id = dispatch.notice_open_token_id,
                  .target_user_id = renewal.reviewer,
                  .guild_id = request.source.reference.guild_id,
                  .channel_id = request.source.reference.channel_id,
                  .notice_type = "chronicle_approval",
                  .content = std::move(content),
                  .source_aggregate_type = "chronicle_entry",
                  .source_aggregate_id = entry_id,
                  .expires_at_ms = request.notice_expires_at_ms,
                  .notice_idempotency_key =
                      "notice:chronicle:renewed:" + dispatch.notice_id,
                  .token_idempotency_key =
                      "token:chronicle:notice-renewed:" + dispatch.notice_id,
                  .created_at_ms = request.now_ms,
              });
          auto update_approval = connection.prepare(
              "UPDATE chronicle_approval SET notice_id=?,requested_at_ms=? "
              "WHERE approval_id=? AND state='pending'");
          update_approval.bind(1, dispatch.notice_id);
          update_approval.bind(2, request.now_ms);
          update_approval.bind(3, renewal.approval_id);
          update_approval.execute();
          insert_token(connection, dispatch.approve_token_id, "button",
                       "chronicle.entry.approve", "chronicle_approval",
                       renewal.approval_id, renewal.reviewer,
                       request.source.reference.guild_id,
                       request.source.reference.channel_id, entry->revision,
                       request.notice_expires_at_ms,
                       "token:chronicle:approve-renewed:" + dispatch.notice_id,
                       request.now_ms);
          insert_token(connection, dispatch.decline_token_id, "button",
                       "chronicle.entry.decline", "chronicle_approval",
                       renewal.approval_id, renewal.reviewer,
                       request.source.reference.guild_id,
                       request.source.reference.channel_id, entry->revision,
                       request.notice_expires_at_ms,
                       "token:chronicle:decline-renewed:" + dispatch.notice_id,
                       request.now_ms);
          require_fresh_durable_write(detail::insert_event_uncommitted(
              connection,
              event_for(dispatch.notice_event_id, "chronicle.notice_renewed.v1",
                        "chronicle_entry", entry_id, request.proposer_user_id,
                        request.source.reference.guild_id,
                        request.source.reference.channel_id,
                        request.source.reference.message_id, request.now_ms,
                        request.correlation_id,
                        "event:chronicle:notice-renewed:" + dispatch.notice_id,
                        Json{{"entry_id", entry_id},
                             {"approval_id", renewal.approval_id},
                             {"status", "pending"},
                             {"revision", entry->revision},
                             {"reason", "approval_notice_expired"},
                             {"test", has_tag(*entry, "owner-test")}})));
          if (entry->visibility == ChronicleVisibility::shared) {
            insert_public_outbox(
                connection, dispatch.notice_outbox_id,
                request.source.reference.guild_id,
                request.source.reference.channel_id, entry_id,
                "outbox:chronicle:notice:renewed:" + dispatch.notice_id,
                request.now_ms, request.correlation_id,
                dispatch.notice_event_id,
                approval_notice_card(*entry, renewal.reviewer,
                                     dispatch.notice_open_token_id));
            wake_outbox = true;
          }
        }
        control_mode = ProposalControlMode::confirmations_reissued;
        transaction.commit();
        return {.code = ChronicleResultCode::existing,
                .entry = std::move(entry),
                .control_mode = control_mode,
                .wake_outbox = wake_outbox};
      }
    }
    transaction.commit();
    return {.code = ChronicleResultCode::existing,
            .entry = std::move(entry),
            .actions = std::move(actions),
            .control_mode = control_mode};
  }

  auto insert = connection.prepare(
      "INSERT INTO chronicle_entry (entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,source_guild_id,"
      "source_channel_id,source_message_id,source_author_user_id,source_text,"
      "source_text_truncated,source_attachment_count,revision) "
      "VALUES (?,?,?,?,?,'proposed',?,?,?,?,?,?,?,?,?,?,1)");
  insert.bind(1, request.entry_id);
  insert.bind(2, chronicle_entry_type_name(request.type));
  insert.bind(3, request.title);
  insert.bind(4, request.body);
  insert.bind(5, chronicle_visibility_name(request.visibility));
  insert.bind(6, request.source.occurred_at_ms);
  insert.bind(7, request.now_ms);
  insert.bind(8, request.proposer_user_id.str());
  insert.bind(9, request.source.reference.guild_id.str());
  insert.bind(10, request.source.reference.channel_id.str());
  insert.bind(11, request.source.reference.message_id.str());
  insert.bind(12, request.source.author.user_id.str());
  insert.bind(13, request.source.content);
  insert.bind(14, static_cast<std::int64_t>(request.source.content_truncated));
  insert.bind(15, static_cast<std::int64_t>(request.source.attachments.size()));
  insert.execute();

  auto add_participant = connection.prepare(
      "INSERT OR IGNORE INTO chronicle_participant (entry_id,user_id,role) "
      "VALUES (?,?,?)");
  const auto add = [&](const DiscordSnowflake &user,
                       const std::string_view role) {
    add_participant.bind(1, request.entry_id);
    add_participant.bind(2, user.str());
    add_participant.bind(3, role);
    add_participant.execute();
    add_participant.reset();
  };
  add(request.proposer_user_id, "proposer");
  add(request.source.author.user_id, "source_author");
  for (const auto &participant : request.source.mentioned_users) {
    if (!participant.is_bot && participant.user_id.is_set()) {
      add(participant.user_id, "subject");
    }
  }
  for (std::size_t index = 0; index < request.source.attachments.size();
       ++index) {
    const auto &attachment = request.source.attachments[index];
    auto add_attachment = connection.prepare(
        "INSERT INTO chronicle_attachment (entry_id,position,attachment_id,"
        "filename,content_type,byte_size,width,height,is_ephemeral,is_spoiler) "
        "VALUES (?,?,?,?,?,?,?,?,?,?)");
    add_attachment.bind(1, request.entry_id);
    add_attachment.bind(2, static_cast<std::int64_t>(index));
    add_attachment.bind(3, attachment.attachment_id.str());
    add_attachment.bind(4, attachment.filename);
    if (attachment.content_type)
      add_attachment.bind(5, *attachment.content_type);
    else
      add_attachment.bind_null(5);
    add_attachment.bind(6, static_cast<std::int64_t>(attachment.byte_size));
    if (attachment.width)
      add_attachment.bind(7, static_cast<std::int64_t>(*attachment.width));
    else
      add_attachment.bind_null(7);
    if (attachment.height)
      add_attachment.bind(8, static_cast<std::int64_t>(*attachment.height));
    else
      add_attachment.bind_null(8);
    add_attachment.bind(9, static_cast<std::int64_t>(attachment.ephemeral));
    add_attachment.bind(10, static_cast<std::int64_t>(attachment.spoiler));
    add_attachment.execute();
  }
  if (request.owner_test) {
    auto tag = connection.prepare(
        "INSERT INTO chronicle_tag (entry_id,tag) VALUES (?,'owner-test')");
    tag.bind(1, request.entry_id);
    tag.execute();
  }
  insert_token(
      connection, request.actions.edit_token_id, "modal",
      "chronicle.entry.edit", "chronicle_entry", request.entry_id,
      request.proposer_user_id, request.source.reference.guild_id,
      request.source.reference.channel_id, 1, request.action_expires_at_ms,
      "token:chronicle:edit:" + request.actions.edit_token_id, request.now_ms);
  insert_token(connection, request.actions.submit_token_id, "button",
               "chronicle.entry.submit", "chronicle_entry", request.entry_id,
               request.proposer_user_id, request.source.reference.guild_id,
               request.source.reference.channel_id, 1,
               request.action_expires_at_ms,
               "token:chronicle:submit:" + request.actions.submit_token_id,
               request.now_ms);
  insert_token(connection, request.actions.retract_token_id, "button",
               "chronicle.entry.retract", "chronicle_entry", request.entry_id,
               request.proposer_user_id, request.source.reference.guild_id,
               request.source.reference.channel_id, 1,
               request.action_expires_at_ms,
               "token:chronicle:retract:" + request.actions.retract_token_id,
               request.now_ms);
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(
          request.event_id, "chronicle.proposal_created.v1", "chronicle_entry",
          request.entry_id, request.proposer_user_id,
          request.source.reference.guild_id,
          request.source.reference.channel_id,
          request.source.reference.message_id, request.now_ms,
          request.correlation_id, request.idempotency_key,
          Json{{"entry_id", request.entry_id},
               {"status", "proposed"},
               {"visibility", chronicle_visibility_name(request.visibility)},
               {"revision", 1},
               {"test", request.owner_test}})));
  auto result = load_entry(connection, request.entry_id);
  transaction.commit();
  return {.code = ChronicleResultCode::created,
          .entry = std::move(result),
          .actions = request.actions};
}

ChronicleMutationResult
SqliteChronicleRepository::edit_proposal(const EditProposalRequest &request) {
  require_id(request.token_id);
  require_id(request.event_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.correlation_id, request.now_ms);
  std::set<std::string> unique_tags;
  const bool tags_valid =
      std::all_of(request.tags.begin(), request.tags.end(),
                  [&unique_tags](const auto &tag) {
                    return valid_tag(tag) && unique_tags.insert(tag).second;
                  });
  if (!valid_chronicle_text(request.title, maximum_chronicle_title_size) ||
      !valid_chronicle_text(request.body, maximum_chronicle_body_size) ||
      request.tags.size() > maximum_chronicle_tags || !tags_valid ||
      request.now_ms < 0) {
    throw std::invalid_argument{"Invalid Chronicle edit."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto token = load_active_token(
      connection, request.token_id, request.guild_id, request.channel_id,
      request.actor_user_id, "modal", request.now_ms);
  if (!token || token->action != "chronicle.entry.edit" ||
      token->entity_type != "chronicle_entry") {
    if (!token) {
      const auto expired = load_expired_token(
          connection, request.token_id, request.guild_id, request.channel_id,
          request.actor_user_id, "modal", request.now_ms);
      if (expired && expired->action == "chronicle.entry.edit" &&
          expired->entity_type == "chronicle_entry") {
        transaction.commit();
        return {.code = ChronicleResultCode::expired};
      }
    }
    const auto used =
        load_used_token(connection, request.token_id, request.guild_id,
                        request.channel_id, request.actor_user_id, "modal");
    if (used && used->action == "chronicle.entry.edit" &&
        used->entity_type == "chronicle_entry" &&
        event_replayed(connection, request.interaction_idempotency_key,
                       "chronicle.proposal_edited.v1", used->entity_id,
                       request.actor_user_id, request.guild_id,
                       request.channel_id)) {
      auto replayed = load_entry(connection, used->entity_id);
      if (!replayed) {
        transaction.commit();
        return {.code = ChronicleResultCode::not_found};
      }
      auto requested_tags = request.tags;
      auto stored_tags = replayed->tags;
      std::erase(requested_tags, "owner-test");
      std::erase(stored_tags, "owner-test");
      std::sort(requested_tags.begin(), requested_tags.end());
      std::sort(stored_tags.begin(), stored_tags.end());
      if (replayed->title != request.title || replayed->body != request.body ||
          replayed->type != request.type ||
          replayed->visibility != request.visibility ||
          stored_tags != requested_tags) {
        throw DatabaseError{DatabaseErrorCategory::constraint,
                            SQLITE_CONSTRAINT, SQLITE_CONSTRAINT,
                            "Chronicle edit idempotency conflict."};
      }
      transaction.commit();
      return {.code = ChronicleResultCode::unchanged,
              .entry = std::move(replayed)};
    }
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_token};
  }
  auto entry = load_entry(connection, token->entity_id);
  if (!entry) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  if (entry->revision != token->expected_revision) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision, .entry = entry};
  }
  if (entry->status != ChronicleEntryStatus::proposed ||
      entry->submitted_at_ms ||
      entry->created_by_user_id != request.actor_user_id) {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_state, .entry = entry};
  }
  const bool owner_test_entry = has_tag(*entry, "owner-test");
  if (!owner_test_entry && unique_tags.contains("owner-test")) {
    transaction.commit();
    return {.code = ChronicleResultCode::unauthorized, .entry = entry};
  }
  auto effective_tags = request.tags;
  if (owner_test_entry && !unique_tags.contains("owner-test")) {
    if (effective_tags.size() == maximum_chronicle_tags) {
      transaction.commit();
      return {.code = ChronicleResultCode::invalid_state, .entry = entry};
    }
    effective_tags.emplace_back("owner-test");
  }
  if (request.visibility == ChronicleVisibility::participant_only) {
    auto participant = connection.prepare(
        "SELECT user_id FROM chronicle_participant WHERE entry_id=?");
    participant.bind(1, entry->entry_id);
    while (participant.step()) {
      if (!opted_in(connection,
                    DiscordSnowflake::parse(participant.column_text(0)))) {
        transaction.commit();
        return {.code = ChronicleResultCode::opted_out, .entry = entry};
      }
    }
  }
  if (event_replayed(connection, request.interaction_idempotency_key,
                     "chronicle.proposal_edited.v1", entry->entry_id,
                     request.actor_user_id, request.guild_id,
                     request.channel_id)) {
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged, .entry = entry};
  }
  auto update = connection.prepare(
      "UPDATE chronicle_entry SET entry_type=?,title=?,body=?,visibility=?,"
      "revision=revision+1 WHERE entry_id=? AND status='proposed' AND "
      "submitted_at_ms IS NULL AND revision=?");
  update.bind(1, chronicle_entry_type_name(request.type));
  update.bind(2, request.title);
  update.bind(3, request.body);
  update.bind(4, chronicle_visibility_name(request.visibility));
  update.bind(5, entry->entry_id);
  update.bind(6, static_cast<std::int64_t>(entry->revision));
  update.execute();
  auto remove_tags =
      connection.prepare("DELETE FROM chronicle_tag WHERE entry_id=?");
  remove_tags.bind(1, entry->entry_id);
  remove_tags.execute();
  for (const auto &tag : effective_tags) {
    auto insert = connection.prepare(
        "INSERT INTO chronicle_tag (entry_id,tag) VALUES (?,?)");
    insert.bind(1, entry->entry_id);
    insert.bind(2, tag);
    insert.execute();
  }
  use_token(connection, request.token_id, request.now_ms);
  auto refresh = connection.prepare(
      "UPDATE interaction_token SET expected_entity_revision=? WHERE "
      "entity_type='chronicle_entry' AND entity_id=? AND state='active'");
  refresh.bind(1, static_cast<std::int64_t>(entry->revision + 1));
  refresh.bind(2, entry->entry_id);
  refresh.execute();
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(
          request.event_id, "chronicle.proposal_edited.v1", "chronicle_entry",
          entry->entry_id, request.actor_user_id, request.guild_id,
          request.channel_id, entry->source_message_id, request.now_ms,
          request.correlation_id, request.interaction_idempotency_key,
          Json{{"entry_id", entry->entry_id},
               {"status", "proposed"},
               {"visibility", chronicle_visibility_name(request.visibility)},
               {"revision", entry->revision + 1}})));
  entry = load_entry(connection, entry->entry_id);
  transaction.commit();
  return {.code = ChronicleResultCode::updated, .entry = std::move(entry)};
}

ChronicleMutationResult SqliteChronicleRepository::submit_proposal(
    const SubmitProposalRequest &request) {
  require_id(request.token_id);
  require_id(request.submit_event_id);
  require_id(request.immediate_canon_event_id);
  require_id(request.proposer_approval_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.correlation_id, request.now_ms);
  if (request.now_ms < 0 || request.notice_expires_at_ms <= request.now_ms) {
    throw std::invalid_argument{"Invalid Chronicle submission."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto token = load_active_token(
      connection, request.token_id, request.guild_id, request.channel_id,
      request.actor_user_id, "button", request.now_ms);
  if (!token || token->action != "chronicle.entry.submit" ||
      token->entity_type != "chronicle_entry") {
    if (!token) {
      const auto expired = load_expired_token(
          connection, request.token_id, request.guild_id, request.channel_id,
          request.actor_user_id, "button", request.now_ms);
      if (expired && expired->action == "chronicle.entry.submit" &&
          expired->entity_type == "chronicle_entry") {
        transaction.commit();
        return {.code = ChronicleResultCode::expired};
      }
    }
    const auto used =
        load_used_token(connection, request.token_id, request.guild_id,
                        request.channel_id, request.actor_user_id, "button");
    if (used && used->action == "chronicle.entry.submit" &&
        used->entity_type == "chronicle_entry" &&
        event_replayed(connection, request.interaction_idempotency_key,
                       "chronicle.proposal_submitted.v1", used->entity_id,
                       request.actor_user_id, request.guild_id,
                       request.channel_id)) {
      auto replayed = load_entry(connection, used->entity_id);
      transaction.commit();
      return {.code = ChronicleResultCode::unchanged,
              .entry = std::move(replayed)};
    }
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_token};
  }
  auto entry = load_entry(connection, token->entity_id);
  if (!entry) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  if (entry->revision != token->expected_revision) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision, .entry = entry};
  }
  if (entry->status != ChronicleEntryStatus::proposed ||
      entry->submitted_at_ms ||
      entry->created_by_user_id != request.actor_user_id) {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_state, .entry = entry};
  }

  const bool owner_test_entry = has_tag(*entry, "owner-test");
  if (owner_test_entry && (request.actor_user_id != request.owner_user_id ||
                           !request.owner_user_id.is_set())) {
    transaction.commit();
    return {.code = ChronicleResultCode::unauthorized, .entry = entry};
  }

  std::vector<std::pair<DiscordSnowflake, std::string>> reviewers;
  if (entry->visibility == ChronicleVisibility::shared) {
    const auto reviewer =
        owner_test_entry
            ? request.owner_user_id
            : (entry->source_author_user_id != request.actor_user_id
                   ? entry->source_author_user_id
                   : request.owner_user_id);
    if (reviewer == request.actor_user_id && !owner_test_entry) {
      transaction.commit();
      return {.code = ChronicleResultCode::invalid_state, .entry = entry};
    }
    if (!opted_in(connection, request.actor_user_id) ||
        !opted_in(connection, entry->source_author_user_id) ||
        !opted_in(connection, reviewer)) {
      transaction.commit();
      return {.code = ChronicleResultCode::opted_out, .entry = entry};
    }
    reviewers.emplace_back(
        reviewer,
        owner_test_entry
            ? "owner_test"
            : (reviewer == request.owner_user_id ? "owner" : "participant"));
  } else {
    for (const auto &participant : entry->participants) {
      if (!opted_in(connection, participant)) {
        transaction.commit();
        return {.code = ChronicleResultCode::opted_out, .entry = entry};
      }
      if (participant != request.actor_user_id) {
        reviewers.emplace_back(participant, "participant");
      }
    }
    if (owner_test_entry) {
      reviewers.emplace_back(request.owner_user_id, "owner_test");
    }
    std::sort(reviewers.begin(), reviewers.end(),
              [](const auto &left, const auto &right) {
                return left.first.str() < right.first.str();
              });
    reviewers.erase(std::unique(reviewers.begin(), reviewers.end(),
                                [](const auto &left, const auto &right) {
                                  return left.first == right.first;
                                }),
                    reviewers.end());
  }
  if (request.reviewer_dispatches.size() < reviewers.size()) {
    throw std::invalid_argument{
        "Insufficient Chronicle reviewer dispatch IDs."};
  }

  const auto submitted_revision = entry->revision + 1;
  auto update = connection.prepare(
      "UPDATE chronicle_entry SET submitted_at_ms=?,revision=? WHERE "
      "entry_id=? "
      "AND status='proposed' AND submitted_at_ms IS NULL AND revision=?");
  update.bind(1, request.now_ms);
  update.bind(2, static_cast<std::int64_t>(submitted_revision));
  update.bind(3, entry->entry_id);
  update.bind(4, static_cast<std::int64_t>(entry->revision));
  update.execute();
  use_token(connection, request.token_id, request.now_ms);
  auto cancel_edit = connection.prepare(
      "UPDATE interaction_token SET state='cancelled' WHERE entity_type="
      "'chronicle_entry' AND entity_id=? AND action IN "
      "('chronicle.entry.edit','chronicle.entry.retract') AND state='active'");
  cancel_edit.bind(1, entry->entry_id);
  cancel_edit.execute();

  auto proposer = connection.prepare(
      "INSERT INTO chronicle_approval (approval_id,entry_id,reviewer_user_id,"
      "approval_role,state,entry_revision,requested_at_ms,acted_at_ms,"
      "interaction_idempotency_key) VALUES "
      "(?,?,?,'proposer','approved',?,?,?,?)");
  proposer.bind(1, request.proposer_approval_id);
  proposer.bind(2, entry->entry_id);
  proposer.bind(3, request.actor_user_id.str());
  proposer.bind(4, static_cast<std::int64_t>(submitted_revision));
  proposer.bind(5, request.now_ms);
  proposer.bind(6, request.now_ms);
  proposer.bind(7, request.interaction_idempotency_key);
  proposer.execute();

  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(
          request.submit_event_id, "chronicle.proposal_submitted.v1",
          "chronicle_entry", entry->entry_id, request.actor_user_id,
          request.guild_id, request.channel_id, entry->source_message_id,
          request.now_ms, request.correlation_id,
          request.interaction_idempotency_key,
          Json{{"entry_id", entry->entry_id},
               {"status", "proposed"},
               {"visibility", chronicle_visibility_name(entry->visibility)},
               {"revision", submitted_revision},
               {"test", owner_test_entry}})));

  if (reviewers.empty()) {
    auto canon = connection.prepare(
        "UPDATE chronicle_entry SET status='canon',approved_at_ms=?,"
        "approved_by_user_id=?,revision=revision+1 WHERE entry_id=? AND "
        "status='proposed' AND revision=?");
    canon.bind(1, request.now_ms);
    canon.bind(2, request.actor_user_id.str());
    canon.bind(3, entry->entry_id);
    canon.bind(4, static_cast<std::int64_t>(submitted_revision));
    canon.execute();
    require_fresh_durable_write(detail::insert_event_uncommitted(
        connection,
        event_for(request.immediate_canon_event_id,
                  "chronicle.entry_canonized.v1", "chronicle_entry",
                  entry->entry_id, request.actor_user_id, request.guild_id,
                  request.channel_id, entry->source_message_id, request.now_ms,
                  request.correlation_id,
                  "event:chronicle:canon:" + entry->entry_id,
                  Json{{"entry_id", entry->entry_id},
                       {"status", "canon"},
                       {"visibility", "participant_only"},
                       {"revision", submitted_revision + 1},
                       {"test", owner_test_entry}})));
    entry = load_entry(connection, entry->entry_id);
    transaction.commit();
    return {.code = ChronicleResultCode::updated,
            .entry = std::move(entry),
            .became_canon = true};
  }

  bool wake_outbox = false;
  for (std::size_t index = 0; index < reviewers.size(); ++index) {
    const auto &[reviewer, role] = reviewers[index];
    const auto &dispatch = request.reviewer_dispatches[index];
    require_id(dispatch.approval_id);
    require_id(dispatch.notice_id);
    require_id(dispatch.notice_open_token_id);
    require_id(dispatch.approve_token_id);
    require_id(dispatch.decline_token_id);
    require_id(dispatch.notice_event_id);
    require_id(dispatch.notice_outbox_id);
    auto approval = connection.prepare(
        "INSERT INTO chronicle_approval (approval_id,entry_id,reviewer_user_id,"
        "approval_role,state,entry_revision,notice_id,requested_at_ms) "
        "VALUES (?,?,?,?,'pending',?,?,?)");
    approval.bind(1, dispatch.approval_id);
    approval.bind(2, entry->entry_id);
    approval.bind(3, reviewer.str());
    approval.bind(4, role);
    approval.bind(5, static_cast<std::int64_t>(submitted_revision));
    approval.bind(6, dispatch.notice_id);
    approval.bind(7, request.now_ms);
    // Notice must precede approval because approval.notice_id is a foreign key.
    const PendingNoticeContent content{
        .title = "Chronicle approval requested",
        .body = (owner_test_entry ? "**TEST DATA — OWNER SELF-APPROVAL**\n"
                                  : std::string{}) +
                "**" + entry->title + "**\n" +
                bounded_summary(entry->body, 180) + "\n" +
                render_chronicle_provenance(*entry, 1'100) +
                "\nApprove only if this should become canon.",
        .actions = {PendingNoticeContent::Action{
                        .custom_id =
                            make_chronicle_component(chronicle_component_prefix,
                                                     dispatch.approve_token_id),
                        .label = "Approve"},
                    PendingNoticeContent::Action{
                        .custom_id =
                            make_chronicle_component(chronicle_component_prefix,
                                                     dispatch.decline_token_id),
                        .label = "Decline"}},
    };
    insert_notice(connection,
                  CreatePendingNoticeRequest{
                      .notice_id = dispatch.notice_id,
                      .token_id = dispatch.notice_open_token_id,
                      .target_user_id = reviewer,
                      .guild_id = request.guild_id,
                      .channel_id = request.channel_id,
                      .notice_type = "chronicle_approval",
                      .content = content,
                      .source_aggregate_type = "chronicle_entry",
                      .source_aggregate_id = entry->entry_id,
                      .expires_at_ms = request.notice_expires_at_ms,
                      .notice_idempotency_key =
                          "notice:chronicle:" + dispatch.approval_id,
                      .token_idempotency_key =
                          "token:chronicle:notice:" + dispatch.approval_id,
                      .created_at_ms = request.now_ms,
                  });
    approval.execute();
    insert_token(
        connection, dispatch.approve_token_id, "button",
        "chronicle.entry.approve", "chronicle_approval", dispatch.approval_id,
        reviewer, request.guild_id, request.channel_id, submitted_revision,
        request.notice_expires_at_ms,
        "token:chronicle:approve:" + dispatch.approval_id, request.now_ms);
    insert_token(
        connection, dispatch.decline_token_id, "button",
        "chronicle.entry.decline", "chronicle_approval", dispatch.approval_id,
        reviewer, request.guild_id, request.channel_id, submitted_revision,
        request.notice_expires_at_ms,
        "token:chronicle:decline:" + dispatch.approval_id, request.now_ms);
    require_fresh_durable_write(detail::insert_event_uncommitted(
        connection,
        event_for(dispatch.notice_event_id, "chronicle.notice_created.v1",
                  "chronicle_entry", entry->entry_id, request.actor_user_id,
                  request.guild_id, request.channel_id,
                  entry->source_message_id, request.now_ms,
                  request.correlation_id,
                  "event:chronicle:notice:" + dispatch.approval_id,
                  Json{{"entry_id", entry->entry_id},
                       {"approval_id", dispatch.approval_id},
                       {"status", "pending"},
                       {"revision", submitted_revision},
                       {"test", owner_test_entry}})));
    if (entry->visibility == ChronicleVisibility::shared) {
      insert_public_outbox(
          connection, dispatch.notice_outbox_id, request.guild_id,
          request.channel_id, entry->entry_id,
          "outbox:chronicle:notice:" + dispatch.approval_id, request.now_ms,
          request.correlation_id, dispatch.notice_event_id,
          InteractionMessage{
              .content =
                  (owner_test_entry ? "**[TEST DATA]** " : std::string{}) +
                  "<@" + reviewer.str() + ">, a sealed notice awaits.",
              .embed =
                  EmbedPayload{
                      .color = 0x8B0000U,
                      .title = owner_test_entry ? "TEST DATA — sealed notice"
                                                : "A sealed notice awaits",
                      .description = owner_test_entry
                                         ? "A test-mode private Chronicle "
                                           "decision awaits the owner."
                                         : "A private Chronicle decision "
                                           "awaits the addressed recipient."},
              .buttons = {ButtonPayload{
                  .custom_id = make_component_id(dispatch.notice_open_token_id),
                  .label = "Open sealed notice"}},
              .allowed_user_mentions = {reviewer}});
      wake_outbox = true;
    }
  }
  entry = load_entry(connection, entry->entry_id);
  transaction.commit();
  return {.code = ChronicleResultCode::updated,
          .entry = std::move(entry),
          .wake_outbox = wake_outbox};
}

ChronicleMutationResult
SqliteChronicleRepository::apply_approval(const ApplyApprovalRequest &request) {
  require_id(request.token_id);
  require_id(request.action_event_id);
  require_id(request.canon_event_id);
  require_id(request.public_outbox_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.correlation_id, request.now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto token = load_active_token(
      connection, request.token_id, request.guild_id, request.channel_id,
      request.actor_user_id, "button", request.now_ms);
  if (!token) {
    const auto expired = load_expired_token(
        connection, request.token_id, request.guild_id, request.channel_id,
        request.actor_user_id, "button", request.now_ms);
    if (expired && ((expired->entity_type == "chronicle_entry" &&
                     expired->action == "chronicle.entry.retract") ||
                    (expired->entity_type == "chronicle_approval" &&
                     (expired->action == "chronicle.entry.approve" ||
                      expired->action == "chronicle.entry.decline")))) {
      transaction.commit();
      return {.code = ChronicleResultCode::expired};
    }
    const auto used =
        load_used_token(connection, request.token_id, request.guild_id,
                        request.channel_id, request.actor_user_id, "button");
    if (used) {
      std::optional<ChronicleEntry> entry;
      if (used->entity_type == "chronicle_entry") {
        entry = load_entry(connection, used->entity_id);
      } else if (used->entity_type == "chronicle_approval") {
        auto approval = connection.prepare(
            "SELECT entry_id FROM chronicle_approval WHERE approval_id=?");
        approval.bind(1, used->entity_id);
        if (approval.step())
          entry = load_entry(connection, approval.column_text(0));
      }
      const auto event_type = used->action == "chronicle.entry.approve"
                                  ? "chronicle.approval_recorded.v1"
                                  : (used->action == "chronicle.entry.decline"
                                         ? "chronicle.approval_declined.v1"
                                         : "chronicle.entry_retracted.v1");
      if (entry &&
          event_replayed(connection, request.interaction_idempotency_key,
                         event_type, entry->entry_id, request.actor_user_id,
                         request.guild_id, request.channel_id)) {
        const bool became_canon = used->action == "chronicle.entry.approve" &&
                                  entry->status == ChronicleEntryStatus::canon;
        transaction.commit();
        return {.code = ChronicleResultCode::unchanged,
                .entry = std::move(entry),
                .became_canon = became_canon};
      }
    }
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_token};
  }

  if (token->action == "chronicle.entry.retract" &&
      token->entity_type == "chronicle_entry") {
    auto entry = load_entry(connection, token->entity_id);
    if (!entry) {
      transaction.commit();
      return {.code = ChronicleResultCode::not_found};
    }
    if (entry->revision != token->expected_revision) {
      transaction.commit();
      return {.code = ChronicleResultCode::stale_revision, .entry = entry};
    }
    if (entry->status != ChronicleEntryStatus::proposed ||
        entry->created_by_user_id != request.actor_user_id) {
      transaction.commit();
      return {.code = ChronicleResultCode::invalid_state, .entry = entry};
    }
    auto update = connection.prepare(
        "UPDATE chronicle_entry SET status='retracted',retracted_at_ms=?,"
        "retracted_by_user_id=?,revision=revision+1 WHERE entry_id=? AND "
        "status='proposed' AND revision=?");
    update.bind(1, request.now_ms);
    update.bind(2, request.actor_user_id.str());
    update.bind(3, entry->entry_id);
    update.bind(4, static_cast<std::int64_t>(entry->revision));
    update.execute();
    use_token(connection, request.token_id, request.now_ms);
    auto cancel = connection.prepare(
        "UPDATE interaction_token SET state='cancelled' WHERE "
        "entity_type='chronicle_entry' AND entity_id=? AND state='active'");
    cancel.bind(1, entry->entry_id);
    cancel.execute();
    require_fresh_durable_write(detail::insert_event_uncommitted(
        connection,
        event_for(request.action_event_id, "chronicle.entry_retracted.v1",
                  "chronicle_entry", entry->entry_id, request.actor_user_id,
                  request.guild_id, request.channel_id,
                  entry->source_message_id, request.now_ms,
                  request.correlation_id, request.interaction_idempotency_key,
                  Json{{"entry_id", entry->entry_id},
                       {"status", "retracted"},
                       {"revision", entry->revision + 1},
                       {"reason", "creator_cancelled"}})));
    entry = load_entry(connection, entry->entry_id);
    transaction.commit();
    return {.code = ChronicleResultCode::updated, .entry = std::move(entry)};
  }

  if (token->entity_type != "chronicle_approval" ||
      (token->action != "chronicle.entry.approve" &&
       token->action != "chronicle.entry.decline")) {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_token};
  }
  auto approval = connection.prepare(
      "SELECT entry_id,reviewer_user_id,state,entry_revision,notice_id "
      "FROM chronicle_approval WHERE approval_id=?");
  approval.bind(1, token->entity_id);
  if (!approval.step()) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  const auto entry_id = approval.column_text(0);
  const auto reviewer = DiscordSnowflake::parse(approval.column_text(1));
  const auto approval_state = approval.column_text(2);
  const auto approval_revision =
      static_cast<std::size_t>(approval.column_int64(3));
  const auto notice_id = optional_text(approval, 4);
  auto entry = load_entry(connection, entry_id);
  if (!entry) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  if (entry->revision != token->expected_revision ||
      entry->revision != approval_revision) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision, .entry = entry};
  }
  if (entry->status != ChronicleEntryStatus::proposed ||
      approval_state != "pending" || reviewer != request.actor_user_id) {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_state, .entry = entry};
  }
  const bool declined = token->action == "chronicle.entry.decline";
  if (!declined) {
    bool consent_valid = opted_in(connection, request.actor_user_id) &&
                         opted_in(connection, entry->created_by_user_id) &&
                         opted_in(connection, entry->source_author_user_id);
    if (entry->visibility == ChronicleVisibility::participant_only) {
      consent_valid =
          consent_valid &&
          std::all_of(entry->participants.begin(), entry->participants.end(),
                      [&connection](const DiscordSnowflake &participant) {
                        return opted_in(connection, participant);
                      });
    }
    if (!consent_valid) {
      transaction.commit();
      return {.code = ChronicleResultCode::opted_out, .entry = entry};
    }
  }
  auto act = connection.prepare(
      "UPDATE chronicle_approval SET state=?,acted_at_ms=?,"
      "interaction_idempotency_key=? WHERE approval_id=? AND state='pending'");
  act.bind(1, declined ? "declined" : "approved");
  act.bind(2, request.now_ms);
  act.bind(3, request.interaction_idempotency_key);
  act.bind(4, token->entity_id);
  act.execute();
  use_token(connection, request.token_id, request.now_ms);
  auto cancel_sibling = connection.prepare(
      "UPDATE interaction_token SET state='cancelled' WHERE "
      "entity_type='chronicle_approval' AND entity_id=? AND state='active'");
  cancel_sibling.bind(1, token->entity_id);
  cancel_sibling.execute();
  if (notice_id) {
    auto consume = connection.prepare(
        "UPDATE pending_notice SET state='consumed',consumed_at_ms=? WHERE "
        "notice_id=? AND state='opened'");
    consume.bind(1, request.now_ms);
    consume.bind(2, *notice_id);
    consume.execute();
  }
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(request.action_event_id,
                declined ? "chronicle.approval_declined.v1"
                         : "chronicle.approval_recorded.v1",
                "chronicle_entry", entry->entry_id, request.actor_user_id,
                request.guild_id, request.channel_id, entry->source_message_id,
                request.now_ms, request.correlation_id,
                request.interaction_idempotency_key,
                Json{{"entry_id", entry->entry_id},
                     {"approval_id", token->entity_id},
                     {"status", declined ? "declined" : "approved"},
                     {"revision", entry->revision},
                     {"test", has_tag(*entry, "owner-test")}})));

  if (declined) {
    auto retract = connection.prepare(
        "UPDATE chronicle_entry SET status='retracted',retracted_at_ms=?,"
        "retracted_by_user_id=?,revision=revision+1 WHERE entry_id=? AND "
        "status='proposed' AND revision=?");
    retract.bind(1, request.now_ms);
    retract.bind(2, request.actor_user_id.str());
    retract.bind(3, entry->entry_id);
    retract.bind(4, static_cast<std::int64_t>(entry->revision));
    retract.execute();
    auto cancel_all = connection.prepare(
        "UPDATE chronicle_approval SET state='cancelled',acted_at_ms=? WHERE "
        "entry_id=? AND state='pending'");
    cancel_all.bind(1, request.now_ms);
    cancel_all.bind(2, entry->entry_id);
    cancel_all.execute();
    cancel_approval_artifacts(connection, entry->entry_id, request.now_ms);
    require_fresh_durable_write(detail::insert_event_uncommitted(
        connection,
        event_for(request.canon_event_id, "chronicle.entry_retracted.v1",
                  "chronicle_entry", entry->entry_id, request.actor_user_id,
                  request.guild_id, request.channel_id,
                  entry->source_message_id, request.now_ms,
                  request.correlation_id,
                  "event:chronicle:decline-retract:" + token->entity_id,
                  Json{{"entry_id", entry->entry_id},
                       {"status", "retracted"},
                       {"revision", entry->revision + 1},
                       {"reason", "reviewer_declined"}})));
    entry = load_entry(connection, entry->entry_id);
    transaction.commit();
    return {.code = ChronicleResultCode::updated, .entry = std::move(entry)};
  }

  auto pending = connection.prepare("SELECT count(*) FROM chronicle_approval "
                                    "WHERE entry_id=? AND state='pending'");
  pending.bind(1, entry->entry_id);
  if (!pending.step() || pending.column_int64(0) != 0) {
    transaction.commit();
    return {.code = ChronicleResultCode::updated, .entry = std::move(entry)};
  }
  auto canon = connection.prepare(
      "UPDATE chronicle_entry SET status='canon',approved_at_ms=?,"
      "approved_by_user_id=?,revision=revision+1 WHERE entry_id=? AND "
      "status='proposed' AND revision=?");
  canon.bind(1, request.now_ms);
  canon.bind(2, request.actor_user_id.str());
  canon.bind(3, entry->entry_id);
  canon.bind(4, static_cast<std::int64_t>(entry->revision));
  canon.execute();
  cancel_approval_artifacts(connection, entry->entry_id, request.now_ms);
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(
          request.canon_event_id, "chronicle.entry_canonized.v1",
          "chronicle_entry", entry->entry_id, request.actor_user_id,
          request.guild_id, request.channel_id, entry->source_message_id,
          request.now_ms, request.correlation_id,
          "event:chronicle:canon:" + entry->entry_id,
          Json{{"entry_id", entry->entry_id},
               {"status", "canon"},
               {"visibility", chronicle_visibility_name(entry->visibility)},
               {"revision", entry->revision + 1},
               {"test", has_tag(*entry, "owner-test")}})));
  bool wake_outbox = false;
  if (entry->visibility == ChronicleVisibility::shared) {
    insert_public_outbox(
        connection, request.public_outbox_id, request.guild_id,
        request.channel_id, entry->entry_id,
        "outbox:chronicle:canon:" + entry->entry_id, request.now_ms,
        request.correlation_id, request.canon_event_id,
        InteractionMessage{
            .content = (is_owner_test_entry(*entry)
                            ? "**TEST DATA — OWNER SELF-APPROVAL**\n"
                            : std::string{}) +
                       "**Entered into the Living Chronicle**\n**" +
                       entry->title + "**\n" + entry->body + "\nReference: `" +
                       entry->entry_id.substr(0, 8) + "`",
            .embed = std::nullopt,
            .buttons = {},
            .allowed_user_mentions = {}});
    wake_outbox = true;
  }
  entry = load_entry(connection, entry->entry_id);
  transaction.commit();
  return {.code = ChronicleResultCode::updated,
          .entry = std::move(entry),
          .became_canon = true,
          .wake_outbox = wake_outbox};
}

ChronicleMutationResult
SqliteChronicleRepository::confirm_memory(const ConfirmMemoryRequest &request) {
  require_id(request.memory_id);
  require_id(request.event_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.draft.guild_id, request.draft.channel_id,
                  request.draft.user_id, request.correlation_id,
                  request.now_ms);
  if (request.expiry_job_id)
    require_id(*request.expiry_job_id);
  if (!valid_chronicle_text(request.draft.text, maximum_memory_text_size) ||
      !request.draft.user_id.is_set() || !request.draft.guild_id.is_set() ||
      !request.draft.channel_id.is_set() || request.now_ms < 0 ||
      (request.draft.sensitivity != MemorySensitivity::ordinary &&
       request.draft.visibility != MemoryVisibility::self_only) ||
      request.draft.expires_at_ms.has_value() !=
          request.expiry_job_id.has_value() ||
      (request.draft.expires_at_ms &&
       *request.draft.expires_at_ms <= request.now_ms)) {
    throw std::invalid_argument{"Invalid explicit memory confirmation."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT memory_id,text,visibility,sensitivity,created_by_user_id,"
      "expires_at_ms FROM memory WHERE creation_idempotency_key=?");
  replay.bind(1, request.interaction_idempotency_key);
  if (replay.step()) {
    const bool matches =
        replay.column_text(1) == request.draft.text &&
        replay.column_text(2) ==
            memory_visibility_name(request.draft.visibility) &&
        replay.column_text(3) ==
            memory_sensitivity_name(request.draft.sensitivity) &&
        replay.column_text(4) == request.draft.user_id.str() &&
        optional_integer(replay, 5) == request.draft.expires_at_ms;
    if (!matches) {
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT, "Memory idempotency conflict."};
    }
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged};
  }
  if (!opted_in(connection, request.draft.user_id)) {
    transaction.commit();
    return {.code = ChronicleResultCode::opted_out};
  }
  const auto event = event_for(
      request.event_id, "chronicle.memory_confirmed.v1", "memory",
      request.memory_id, request.draft.user_id, request.draft.guild_id,
      request.draft.channel_id, std::nullopt, request.now_ms,
      request.correlation_id, request.interaction_idempotency_key,
      Json{{"memory_id", request.memory_id},
           {"status", "confirmed"},
           {"visibility", memory_visibility_name(request.draft.visibility)},
           {"sensitivity", memory_sensitivity_name(request.draft.sensitivity)},
           {"revision", 1},
           {"has_expiry", request.draft.expires_at_ms.has_value()}});
  require_fresh_durable_write(
      detail::insert_event_uncommitted(connection, event));
  auto insert = connection.prepare(
      "INSERT INTO memory (memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,expires_at_ms,"
      "revision,creation_idempotency_key) VALUES "
      "(?,'explicit',?,?,?,'confirmed',"
      "'user_confirmed',?,?,?,?,?,?,1,?)");
  insert.bind(1, request.memory_id);
  insert.bind(2, request.draft.text);
  insert.bind(3, memory_visibility_name(request.draft.visibility));
  insert.bind(4, memory_sensitivity_name(request.draft.sensitivity));
  insert.bind(5, request.event_id);
  insert.bind(6, request.draft.user_id.str());
  insert.bind(7, request.draft.user_id.str());
  insert.bind(8, request.now_ms);
  insert.bind(9, request.now_ms);
  if (request.draft.expires_at_ms)
    insert.bind(10, *request.draft.expires_at_ms);
  else
    insert.bind_null(10);
  insert.bind(11, request.interaction_idempotency_key);
  insert.execute();
  auto subject = connection.prepare(
      "INSERT INTO memory_subject (memory_id,subject_type,subject_id) "
      "VALUES (?,'user',?)");
  subject.bind(1, request.memory_id);
  subject.bind(2, request.draft.user_id.str());
  subject.execute();
  if (request.expiry_job_id && request.draft.expires_at_ms) {
    const ScheduledJobEnqueue job{
        .job_id = *request.expiry_job_id,
        .job_type = std::string{memory_expiry_job_type},
        .aggregate_type = "memory",
        .aggregate_id = request.memory_id,
        .due_at_ms = *request.draft.expires_at_ms,
        .max_attempts = 5,
        .idempotency_key = "job:chronicle:memory-expire:" + request.memory_id,
        .created_at_ms = request.now_ms,
    };
    require_fresh_durable_write(detail::insert_job_uncommitted(
        connection, job,
        detail::encode_memory_expiry_payload(
            MemoryExpiryJobPayload{.memory_id = request.memory_id,
                                   .expected_revision = 1},
            request.correlation_id, request.event_id)));
  }
  transaction.commit();
  return {.code = ChronicleResultCode::created,
          .wake_scheduler = request.expiry_job_id.has_value()};
}

ChronicleMutationResult
SqliteChronicleRepository::retract_memory(const RetractItemRequest &request) {
  require_id(request.entity_id);
  require_id(request.event_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.correlation_id, request.now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare("SELECT status,revision,created_by_user_id "
                                  "FROM memory WHERE memory_id=?");
  query.bind(1, request.entity_id);
  if (!query.step()) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  const auto status = query.column_text(0);
  const auto revision = static_cast<std::size_t>(query.column_int64(1));
  const auto creator = DiscordSnowflake::parse(query.column_text(2));
  if (event_replayed(connection, request.interaction_idempotency_key,
                     "chronicle.memory_retracted.v1", request.entity_id,
                     request.actor_user_id, request.guild_id,
                     request.channel_id)) {
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged};
  }
  if (revision != request.expected_revision) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision};
  }
  if (creator != request.actor_user_id &&
      request.actor_user_id != request.owner_user_id) {
    transaction.commit();
    return {.code = ChronicleResultCode::unauthorized};
  }
  if (status == "retracted") {
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged};
  }
  if (status != "confirmed") {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_state};
  }
  auto update = connection.prepare(
      "UPDATE memory SET "
      "status='retracted',retracted_at_ms=?,revision=revision+1 "
      "WHERE memory_id=? AND status='confirmed' AND revision=?");
  update.bind(1, request.now_ms);
  update.bind(2, request.entity_id);
  update.bind(3, static_cast<std::int64_t>(revision));
  update.execute();
  auto cancel = connection.prepare(
      "UPDATE scheduled_job SET "
      "state='cancelled',terminal_at_ms=?,updated_at_ms=? "
      "WHERE aggregate_type='memory' AND aggregate_id=? AND state='pending'");
  cancel.bind(1, request.now_ms);
  cancel.bind(2, request.now_ms);
  cancel.bind(3, request.entity_id);
  cancel.execute();
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(request.event_id, "chronicle.memory_retracted.v1", "memory",
                request.entity_id, request.actor_user_id, request.guild_id,
                request.channel_id, std::nullopt, request.now_ms,
                request.correlation_id, request.interaction_idempotency_key,
                Json{{"memory_id", request.entity_id},
                     {"status", "retracted"},
                     {"revision", revision + 1},
                     {"reason", "authorized_request"}})));
  transaction.commit();
  return {.code = ChronicleResultCode::updated};
}

ChronicleMutationResult
SqliteChronicleRepository::retract_entry(const RetractItemRequest &request) {
  require_id(request.entity_id);
  require_id(request.event_id);
  require_id(request.public_outbox_id);
  require_key(request.interaction_idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.correlation_id, request.now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto entry = load_entry(connection, request.entity_id);
  if (!entry) {
    transaction.commit();
    return {.code = ChronicleResultCode::not_found};
  }
  if (event_replayed(connection, request.interaction_idempotency_key,
                     "chronicle.entry_retracted.v1", entry->entry_id,
                     request.actor_user_id, request.guild_id,
                     request.channel_id)) {
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged, .entry = entry};
  }
  if (entry->revision != request.expected_revision) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision, .entry = entry};
  }
  const bool participant =
      std::find(entry->participants.begin(), entry->participants.end(),
                request.actor_user_id) != entry->participants.end();
  const bool owner_allowed = entry->visibility == ChronicleVisibility::shared &&
                             request.actor_user_id == request.owner_user_id;
  if (entry->created_by_user_id != request.actor_user_id && !participant &&
      !owner_allowed) {
    transaction.commit();
    return {.code = ChronicleResultCode::unauthorized, .entry = entry};
  }
  if (entry->status == ChronicleEntryStatus::retracted) {
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged, .entry = entry};
  }
  const bool was_public = entry->status == ChronicleEntryStatus::canon &&
                          entry->visibility == ChronicleVisibility::shared;
  auto update = connection.prepare(
      "UPDATE chronicle_entry SET status='retracted',retracted_at_ms=?,"
      "retracted_by_user_id=?,revision=revision+1 WHERE entry_id=? AND "
      "status IN ('proposed','canon') AND revision=?");
  update.bind(1, request.now_ms);
  update.bind(2, request.actor_user_id.str());
  update.bind(3, entry->entry_id);
  update.bind(4, static_cast<std::int64_t>(entry->revision));
  update.execute();
  auto cancel = connection.prepare(
      "UPDATE chronicle_approval SET state='cancelled',acted_at_ms=? WHERE "
      "entry_id=? AND state='pending'");
  cancel.bind(1, request.now_ms);
  cancel.bind(2, entry->entry_id);
  cancel.execute();
  cancel_approval_artifacts(connection, entry->entry_id, request.now_ms);
  require_fresh_durable_write(detail::insert_event_uncommitted(
      connection,
      event_for(
          request.event_id, "chronicle.entry_retracted.v1", "chronicle_entry",
          entry->entry_id, request.actor_user_id, request.guild_id,
          request.channel_id, entry->source_message_id, request.now_ms,
          request.correlation_id, request.interaction_idempotency_key,
          Json{{"entry_id", entry->entry_id},
               {"status", "retracted"},
               {"visibility", chronicle_visibility_name(entry->visibility)},
               {"revision", entry->revision + 1},
               {"reason", "authorized_request"},
               {"test", is_owner_test_entry(*entry)}})));
  if (was_public) {
    insert_public_outbox(
        connection, request.public_outbox_id, request.guild_id,
        request.channel_id, entry->entry_id,
        "outbox:chronicle:retract:" + entry->entry_id, request.now_ms,
        request.correlation_id, request.event_id,
        text_message(
            (is_owner_test_entry(*entry) ? "**[TEST DATA]** " : std::string{}) +
            "A Chronicle entry has been retracted. Reference: `" +
            entry->entry_id.substr(0, 8) + "`."));
  }
  entry = load_entry(connection, entry->entry_id);
  transaction.commit();
  return {.code = ChronicleResultCode::updated,
          .entry = std::move(entry),
          .wake_outbox = was_public};
}

ChronicleMutationResult
SqliteChronicleRepository::expire_memory(const ClaimedScheduledJob &job,
                                         std::string event_id,
                                         const std::int64_t now_ms) {
  require_id(event_id);
  if (now_ms < 0) {
    throw std::invalid_argument{"Invalid Chronicle expiry time."};
  }
  const auto *payload = std::get_if<MemoryExpiryJobPayload>(&job.payload);
  if (job.job_type != memory_expiry_job_type || payload == nullptr) {
    return {.code = ChronicleResultCode::invalid_state};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto owned = connection.prepare(
      "SELECT job_type,aggregate_type,aggregate_id,due_at_ms FROM "
      "scheduled_job WHERE job_id=? AND state='claimed' AND lease_owner=? AND "
      "lease_token=?");
  owned.bind(1, job.job_id);
  owned.bind(2, job.lease_owner);
  owned.bind(3, job.lease_token);
  if (!owned.step()) {
    transaction.commit();
    return {.code = ChronicleResultCode::stale_revision};
  }
  if (owned.column_text(0) != memory_expiry_job_type ||
      optional_text(owned, 1) != std::optional<std::string>{"memory"} ||
      optional_text(owned, 2) !=
          std::optional<std::string>{payload->memory_id} ||
      owned.column_int64(3) != job.due_at_ms) {
    transaction.commit();
    return {.code = ChronicleResultCode::invalid_state};
  }
  if (now_ms < job.due_at_ms) {
    auto release = connection.prepare(
        "UPDATE scheduled_job SET state='pending',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,updated_at_ms=max(?,updated_at_"
        "ms) "
        "WHERE job_id=? AND state='claimed' AND lease_owner=? AND "
        "lease_token=?");
    release.bind(1, now_ms);
    release.bind(2, job.job_id);
    release.bind(3, job.lease_owner);
    release.bind(4, job.lease_token);
    release.execute();
    if (connection.changes() != 1) {
      return {.code = ChronicleResultCode::stale_revision};
    }
    transaction.commit();
    return {.code = ChronicleResultCode::unchanged};
  }
  auto memory = connection.prepare(
      "SELECT status,revision,created_by_user_id,expires_at_ms FROM memory "
      "WHERE memory_id=?");
  memory.bind(1, payload->memory_id);
  bool changed = false;
  if (memory.step() && memory.column_text(0) == "confirmed" &&
      static_cast<std::size_t>(memory.column_int64(1)) ==
          payload->expected_revision) {
    if (optional_integer(memory, 3) !=
        std::optional<std::int64_t>{job.due_at_ms}) {
      transaction.commit();
      return {.code = ChronicleResultCode::invalid_state};
    }
    const auto actor = DiscordSnowflake::parse(memory.column_text(2));
    auto update = connection.prepare(
        "UPDATE memory SET "
        "status='expired',expired_at_ms=?,revision=revision+1 "
        "WHERE memory_id=? AND status='confirmed' AND revision=? AND "
        "expires_at_ms=?");
    update.bind(1, now_ms);
    update.bind(2, payload->memory_id);
    update.bind(3, static_cast<std::int64_t>(payload->expected_revision));
    update.bind(4, job.due_at_ms);
    update.execute();
    changed = connection.changes() == 1;
    if (!changed) {
      transaction.commit();
      return {.code = ChronicleResultCode::stale_revision};
    }
    auto scope = connection.prepare("SELECT guild_id,primary_channel_id FROM "
                                    "guild_config WHERE singleton=1");
    if (!scope.step())
      throw std::runtime_error{"Chronicle scope is absent."};
    const auto guild = DiscordSnowflake::parse(scope.column_text(0));
    const auto channel = DiscordSnowflake::parse(scope.column_text(1));
    require_fresh_durable_write(detail::insert_event_uncommitted(
        connection,
        event_for(std::move(event_id), "chronicle.memory_expired.v1", "memory",
                  payload->memory_id, actor, guild, channel, std::nullopt,
                  now_ms, job.correlation_id,
                  "event:chronicle:memory-expired:" + job.job_id,
                  Json{{"memory_id", payload->memory_id},
                       {"status", "expired"},
                       {"revision", payload->expected_revision + 1},
                       {"reason", "scheduled_expiry"}})));
  }
  auto complete = connection.prepare(
      "UPDATE scheduled_job SET state='completed',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,completed_at_ms=max(?,created_at_"
      "ms),"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms),"
      "last_error_code=NULL WHERE job_id=? AND state='claimed' AND "
      "lease_owner=? AND lease_token=?");
  complete.bind(1, now_ms);
  complete.bind(2, now_ms);
  complete.bind(3, now_ms);
  complete.bind(4, job.job_id);
  complete.bind(5, job.lease_owner);
  complete.bind(6, job.lease_token);
  complete.execute();
  if (connection.changes() != 1) {
    return {.code = ChronicleResultCode::stale_revision};
  }
  transaction.commit();
  return {.code = changed ? ChronicleResultCode::updated
                          : ChronicleResultCode::unchanged};
}

RecallResults SqliteChronicleRepository::recall(const DiscordSnowflake &viewer,
                                                const std::string_view query,
                                                const std::int64_t now_ms,
                                                const std::size_t limit) {
  if (!viewer.is_set() ||
      (!query.empty() &&
       !valid_chronicle_text(query, maximum_memory_text_size)) ||
      now_ms < 0 || limit > 20) {
    throw std::invalid_argument{"Invalid Chronicle recall."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  RecallResults result;
  if (!opted_in(connection, viewer))
    return result;
  std::vector<ChronicleEntry> entry_candidates;
  auto entries = connection.prepare(
      "SELECT " + std::string{entry_columns} +
      " FROM chronicle_entry e WHERE e.status='canon' AND "
      "e.occurred_at_ms<=? AND "
      "(e.visibility='shared' OR EXISTS (SELECT 1 FROM chronicle_participant p "
      "WHERE p.entry_id=e.entry_id AND p.user_id=?)) AND "
      "(?='' OR e.title LIKE '%'||?||'%' COLLATE NOCASE OR "
      "e.body LIKE '%'||?||'%' COLLATE NOCASE OR "
      "e.source_text LIKE '%'||?||'%' COLLATE NOCASE) "
      "ORDER BY e.occurred_at_ms DESC,e.entry_id DESC LIMIT ?");
  entries.bind(1, now_ms);
  entries.bind(2, viewer.str());
  entries.bind(3, query);
  entries.bind(4, query);
  entries.bind(5, query);
  entries.bind(6, query);
  entries.bind(7, static_cast<std::int64_t>(limit));
  while (entries.step()) {
    auto entry = read_entry_base(entries);
    enrich_entry(connection, entry);
    entry_candidates.push_back(std::move(entry));
  }
  std::vector<ExplicitMemory> memory_candidates;
  auto memories = connection.prepare(
      "SELECT m.memory_id,m.text,m.visibility,m.sensitivity,m.status,"
      "m.created_by_user_id,m.created_by_user_id,m.created_at_ms,m.expires_at_"
      "ms,"
      "m.revision FROM memory m WHERE m.status='confirmed' AND "
      "(m.expires_at_ms IS NULL OR m.expires_at_ms>?) AND "
      "(m.visibility='shared' OR m.created_by_user_id=?) AND "
      "(?='' OR m.text LIKE '%'||?||'%' COLLATE NOCASE) "
      "ORDER BY m.created_at_ms DESC,m.memory_id DESC LIMIT ?");
  memories.bind(1, now_ms);
  memories.bind(2, viewer.str());
  memories.bind(3, query);
  memories.bind(4, query);
  memories.bind(5, static_cast<std::int64_t>(limit));
  while (memories.step())
    memory_candidates.push_back(read_memory(memories));

  std::size_t entry_index = 0;
  std::size_t memory_index = 0;
  while (result.ordered_items.size() < limit &&
         (entry_index < entry_candidates.size() ||
          memory_index < memory_candidates.size())) {
    const bool take_entry =
        memory_index == memory_candidates.size() ||
        (entry_index < entry_candidates.size() &&
         (entry_candidates[entry_index].occurred_at_ms >
              memory_candidates[memory_index].created_at_ms ||
          (entry_candidates[entry_index].occurred_at_ms ==
               memory_candidates[memory_index].created_at_ms &&
           entry_candidates[entry_index].entry_id >
               memory_candidates[memory_index].memory_id)));
    if (take_entry) {
      const auto &entry = entry_candidates[entry_index++];
      result.ordered_items.emplace_back(entry);
      result.entries.push_back(entry);
    } else {
      const auto &memory = memory_candidates[memory_index++];
      result.ordered_items.emplace_back(memory);
      result.memories.push_back(memory);
    }
  }
  return result;
}

std::vector<ChronicleEntry>
SqliteChronicleRepository::timeline(const std::optional<std::int64_t> since_ms,
                                    const std::int64_t now_ms,
                                    const std::size_t limit) {
  if (now_ms < 0 || (since_ms && *since_ms < 0) || limit > 20) {
    throw std::invalid_argument{"Invalid Chronicle timeline."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto query = connection.prepare(
      "SELECT " + std::string{entry_columns} +
      " FROM chronicle_entry WHERE status='canon' AND visibility='shared' "
      "AND (? IS NULL OR occurred_at_ms>=?) AND occurred_at_ms<=? "
      "ORDER BY occurred_at_ms DESC,entry_id DESC LIMIT ?");
  if (since_ms)
    query.bind(1, *since_ms);
  else
    query.bind_null(1);
  if (since_ms)
    query.bind(2, *since_ms);
  else
    query.bind_null(2);
  query.bind(3, now_ms);
  query.bind(4, static_cast<std::int64_t>(limit));
  std::vector<ChronicleEntry> result;
  while (query.step()) {
    auto entry = read_entry_base(query);
    enrich_entry(connection, entry);
    result.push_back(std::move(entry));
  }
  return result;
}

std::vector<ManageableChronicleItem> SqliteChronicleRepository::manageable(
    const DiscordSnowflake &viewer, const DiscordSnowflake &owner,
    const std::string_view reference, const std::int64_t now_ms,
    const std::size_t limit) {
  const bool valid_reference =
      reference.empty() ||
      (reference.size() >= 4 && reference.size() <= 36 &&
       std::all_of(
           reference.begin(), reference.end(), [](const char character) {
             return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f') || character == '-';
           }));
  if (!viewer.is_set() || !owner.is_set() || !valid_reference || now_ms < 0 ||
      limit > 20) {
    throw std::invalid_argument{"Invalid Chronicle management query."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  std::vector<ManageableChronicleItem> result;
  const bool viewer_opted_in = opted_in(connection, viewer);
  auto memories = connection.prepare(
      "SELECT memory_id,revision,text,visibility,created_by_user_id FROM "
      "memory "
      "WHERE ((status='confirmed' AND "
      "(expires_at_ms IS NULL OR expires_at_ms>?)) OR "
      "(status='retracted' AND ?<>'' AND memory_id LIKE ?||'%')) AND "
      "(created_by_user_id=? OR ?=?) AND (?='' OR memory_id LIKE ?||'%') "
      "ORDER BY created_at_ms DESC LIMIT ?");
  memories.bind(1, now_ms);
  memories.bind(2, reference);
  memories.bind(3, reference);
  memories.bind(4, viewer.str());
  memories.bind(5, viewer.str());
  memories.bind(6, owner.str());
  memories.bind(7, reference);
  memories.bind(8, reference);
  memories.bind(9, static_cast<std::int64_t>(limit));
  while (memories.step()) {
    const auto creator = DiscordSnowflake::parse(memories.column_text(4));
    const bool may_view_text =
        creator == viewer ||
        (memories.column_text(3) == "shared" && viewer_opted_in);
    result.push_back(ManageableChronicleItem{
        .kind = ManageableKind::memory,
        .entity_id = memories.column_text(0),
        .revision = static_cast<std::size_t>(memories.column_int64(1)),
        .summary = may_view_text ? memories.column_text(2)
                                 : "Private memory (content hidden)"});
  }
  const auto remaining = limit > result.size() ? limit - result.size() : 0;
  if (remaining == 0)
    return result;
  auto entries = connection.prepare(
      "SELECT e.entry_id,e.revision,e.title FROM chronicle_entry e WHERE "
      "(e.status IN ('proposed','canon') OR "
      "(e.status='retracted' AND ?<>'' AND e.entry_id LIKE ?||'%')) AND "
      "(e.created_by_user_id=? OR "
      "EXISTS (SELECT 1 FROM chronicle_participant p WHERE "
      "p.entry_id=e.entry_id "
      "AND p.user_id=?) OR (e.visibility='shared' AND ?=?)) AND "
      "(?='' OR e.entry_id LIKE ?||'%') "
      "ORDER BY e.created_at_ms DESC LIMIT ?");
  entries.bind(1, reference);
  entries.bind(2, reference);
  entries.bind(3, viewer.str());
  entries.bind(4, viewer.str());
  entries.bind(5, viewer.str());
  entries.bind(6, owner.str());
  entries.bind(7, reference);
  entries.bind(8, reference);
  entries.bind(9, static_cast<std::int64_t>(remaining));
  while (entries.step()) {
    result.push_back(ManageableChronicleItem{
        .kind = ManageableKind::entry,
        .entity_id = entries.column_text(0),
        .revision = static_cast<std::size_t>(entries.column_int64(1)),
        .summary = entries.column_text(2)});
  }
  return result;
}

} // namespace sanguinius::persistence
