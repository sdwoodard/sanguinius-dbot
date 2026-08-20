#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"

#include "sqlite_durable_work_writes.hpp"

#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

struct DurableTrace {
  std::string correlation_id;
  std::optional<std::string> causation_event_id;
};

void require_timestamp(const std::int64_t value) {
  if (value < 0) {
    throw std::invalid_argument{"Persistent timestamps must be nonnegative."};
  }
}

void require_key(const std::string_view value) {
  if (value.empty() || value.size() > 160) {
    throw std::invalid_argument{"Idempotency keys must be 1-160 bytes."};
  }
}

void require_type(const std::string_view value, const std::size_t maximum) {
  if (value.empty() || value.size() > maximum ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        return !((character >= 'a' && character <= 'z') ||
                 (character >= '0' && character <= '9') || character == '_' ||
                 character == '.' || character == '-');
      })) {
    throw std::invalid_argument{"Durable work type is invalid."};
  }
}

void require_boot_session_id(const std::string_view value) {
  if (value.empty() || value.size() > 64 ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        return !((character >= 'a' && character <= 'z') ||
                 (character >= '0' && character <= '9') || character == '-');
      })) {
    throw std::invalid_argument{"Boot session ID is invalid."};
  }
}

void bind_optional(SqliteStatement &statement, const std::size_t index,
                   const std::optional<std::string> &value) {
  if (value.has_value()) {
    statement.bind(index, *value);
  } else {
    statement.bind_null(index);
  }
}

void bind_snowflake(SqliteStatement &statement, const std::size_t index,
                    const std::optional<DiscordSnowflake> &value) {
  if (value.has_value()) {
    statement.bind(index, value->str());
  } else {
    statement.bind_null(index);
  }
}

[[nodiscard]] std::optional<std::string>
optional_text(SqliteStatement &statement, const int index) {
  if (statement.column_is_null(index)) {
    return std::nullopt;
  }
  return statement.column_text(index);
}

[[nodiscard]] std::optional<DiscordSnowflake>
optional_snowflake(SqliteStatement &statement, const int index) {
  const auto value = optional_text(statement, index);
  return value.has_value()
             ? std::optional<DiscordSnowflake>{DiscordSnowflake::parse(*value)}
             : std::nullopt;
}

[[nodiscard]] std::vector<EventJournalEntry>
read_event_rows(SqliteStatement &statement) {
  std::vector<EventJournalEntry> result;
  while (statement.step()) {
    result.push_back(EventJournalEntry{
        .event_id = statement.column_text(0),
        .event_type = statement.column_text(1),
        .aggregate_type = statement.column_text(2),
        .aggregate_id = statement.column_text(3),
        .actor_user_id = optional_snowflake(statement, 4),
        .guild_id = DiscordSnowflake::parse(statement.column_text(5)),
        .channel_id = optional_snowflake(statement, 6),
        .source_message_id = optional_snowflake(statement, 7),
        .occurred_at_ms = statement.column_int64(8),
        .recorded_at_ms = statement.column_int64(9),
        .correlation_id = statement.column_text(10),
        .causation_id = optional_text(statement, 11),
        .idempotency_key = statement.column_text(12),
        .payload_json = statement.column_text(13),
    });
  }
  return result;
}

void add_trace(Json &value, const std::string_view correlation_id,
               const std::optional<std::string> &causation_event_id) {
  if (!correlation_id.empty()) {
    value["correlation_id"] = correlation_id;
  }
  if (causation_event_id.has_value()) {
    value["causation_event_id"] = *causation_event_id;
  }
}

[[nodiscard]] Json encode_notice(
    const NoticeOutboxPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  const auto &notice = payload.notice;
  Json actions = Json::array();
  for (const auto &action : notice.content.actions) {
    actions.push_back(
        {{"custom_id", action.custom_id}, {"label", action.label}});
  }
  Json result{
      {"payload_version", 1},
      {"notice_id", notice.notice_id},
      {"token_id", notice.token_id},
      {"target_user_id", notice.target_user_id.str()},
      {"guild_id", notice.guild_id.str()},
      {"channel_id", notice.channel_id.str()},
      {"notice_type", notice.notice_type},
      {"title", notice.content.title},
      {"body", notice.content.body},
      {"actions", std::move(actions)},
      {"expires_at_ms", notice.expires_at_ms},
      {"notice_idempotency_key", notice.notice_idempotency_key},
      {"token_idempotency_key", notice.token_idempotency_key},
      {"created_at_ms", notice.created_at_ms},
      {"announce_publicly", payload.announce_publicly},
  };
  result["source_aggregate_type"] = notice.source_aggregate_type.has_value()
                                        ? Json(*notice.source_aggregate_type)
                                        : Json{nullptr};
  result["source_aggregate_id"] = notice.source_aggregate_id.has_value()
                                      ? Json(*notice.source_aggregate_id)
                                      : Json{nullptr};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] NoticeOutboxPayload decode_notice(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1) {
    throw std::runtime_error{"Unsupported notice outbox payload."};
  }
  CreatePendingNoticeRequest notice{
      .notice_id = value.at("notice_id").get<std::string>(),
      .token_id = value.at("token_id").get<std::string>(),
      .target_user_id = DiscordSnowflake::parse(
          value.at("target_user_id").get<std::string>()),
      .guild_id =
          DiscordSnowflake::parse(value.at("guild_id").get<std::string>()),
      .channel_id =
          DiscordSnowflake::parse(value.at("channel_id").get<std::string>()),
      .notice_type = value.at("notice_type").get<std::string>(),
      .content = {value.at("title").get<std::string>(),
                  value.at("body").get<std::string>()},
      .source_aggregate_type =
          value.at("source_aggregate_type").is_null()
              ? std::nullopt
              : std::optional<std::string>{value.at("source_aggregate_type")
                                               .get<std::string>()},
      .source_aggregate_id =
          value.at("source_aggregate_id").is_null()
              ? std::nullopt
              : std::optional<std::string>{value.at("source_aggregate_id")
                                               .get<std::string>()},
      .expires_at_ms = value.at("expires_at_ms").get<std::int64_t>(),
      .notice_idempotency_key =
          value.at("notice_idempotency_key").get<std::string>(),
      .token_idempotency_key =
          value.at("token_idempotency_key").get<std::string>(),
      .created_at_ms = value.at("created_at_ms").get<std::int64_t>(),
  };
  if (value.contains("actions")) {
    for (const auto &action : value.at("actions")) {
      notice.content.actions.push_back(PendingNoticeContent::Action{
          .custom_id = action.at("custom_id").get<std::string>(),
          .label = action.at("label").get<std::string>(),
      });
    }
  }
  return NoticeOutboxPayload{
      .notice = std::move(notice),
      .announce_publicly = value.at("announce_publicly").get<bool>(),
  };
}

[[nodiscard]] Json encode_message(
    const PublicOutboxPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  Json buttons = Json::array();
  for (const auto &button : payload.request.message.buttons) {
    Json encoded{{"custom_id", button.custom_id},
                 {"label", button.label},
                 {"disabled", button.disabled}};
    if (button.style != ButtonStyle::primary)
      encoded["style"] = "secondary";
    buttons.push_back(std::move(encoded));
  }
  Json mentions = Json::array();
  for (const auto mention : payload.request.message.allowed_user_mentions) {
    mentions.push_back(mention.str());
  }
  Json embed = nullptr;
  if (payload.request.message.embed.has_value()) {
    embed = {{"color", payload.request.message.embed->color},
             {"title", payload.request.message.embed->title},
             {"url", payload.request.message.embed->url},
             {"description", payload.request.message.embed->description}};
  }
  Json result{{"payload_version", 1},
              {"guild_id", payload.request.guild_id.str()},
              {"channel_id", payload.request.channel_id.str()},
              {"content", payload.request.message.content},
              {"embed", std::move(embed)},
              {"buttons", std::move(buttons)},
              {"allowed_user_mentions", std::move(mentions)},
              {"fail_before_first_send", payload.fail_before_first_send}};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] PublicOutboxPayload decode_message(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1) {
    throw std::runtime_error{"Unsupported public outbox payload."};
  }
  InteractionMessage message;
  message.content = value.at("content").get<std::string>();
  if (!value.at("embed").is_null()) {
    const auto &embed = value.at("embed");
    message.embed = EmbedPayload{
        .color = embed.at("color").get<std::uint32_t>(),
        .title = embed.at("title").get<std::string>(),
        .url = embed.at("url").get<std::string>(),
        .description = embed.at("description").get<std::string>(),
    };
  }
  for (const auto &button : value.at("buttons")) {
    const auto style = button.value("style", std::string{"primary"});
    if (style != "primary" && style != "secondary")
      throw std::runtime_error{"Unsupported public button style."};
    message.buttons.push_back(ButtonPayload{
        .custom_id = button.at("custom_id").get<std::string>(),
        .label = button.at("label").get<std::string>(),
        .disabled = button.at("disabled").get<bool>(),
        .style = style == "secondary" ? ButtonStyle::secondary
                                        : ButtonStyle::primary,
    });
  }
  for (const auto &mention : value.at("allowed_user_mentions")) {
    message.allowed_user_mentions.push_back(
        DiscordSnowflake::parse(mention.get<std::string>()));
  }
  if (message.content.size() > 2'000 || message.buttons.size() > 5 ||
      message.allowed_user_mentions.size() > 1) {
    throw std::runtime_error{"Public outbox payload exceeds safe bounds."};
  }
  return PublicOutboxPayload{
      .request =
          PublicMessageRequest{
              .guild_id = DiscordSnowflake::parse(
                  value.at("guild_id").get<std::string>()),
              .channel_id = DiscordSnowflake::parse(
                  value.at("channel_id").get<std::string>()),
              .message = std::move(message),
          },
      .fail_before_first_send = value.at("fail_before_first_send").get<bool>(),
  };
}

[[nodiscard]] Json encode_memory_expiry(
    const MemoryExpiryJobPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  Json result{{"payload_version", 1},
              {"memory_id", payload.memory_id},
              {"expected_revision", payload.expected_revision}};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] MemoryExpiryJobPayload decode_memory_expiry(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1) {
    throw std::runtime_error{"Unsupported memory expiry payload."};
  }
  auto result = MemoryExpiryJobPayload{
      .memory_id = value.at("memory_id").get<std::string>(),
      .expected_revision = value.at("expected_revision").get<std::size_t>(),
  };
  if (!valid_uuid_v4(result.memory_id) || result.expected_revision == 0) {
    throw std::runtime_error{"Invalid memory expiry payload."};
  }
  return result;
}

[[nodiscard]] Json encode_session_summary(
    const SessionSummaryJobPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  Json result{{"payload_version", 1},
              {"session_id", payload.session_id},
              {"draft_id", payload.draft_id},
              {"expected_session_revision", payload.expected_session_revision},
              {"expected_draft_revision", payload.expected_draft_revision}};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] SessionSummaryJobPayload
decode_session_summary(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1)
    throw std::runtime_error{"Unsupported session-summary payload."};
  SessionSummaryJobPayload result{
      .session_id = value.at("session_id").get<std::string>(),
      .draft_id = value.at("draft_id").get<std::string>(),
      .expected_session_revision =
          value.at("expected_session_revision").get<std::size_t>(),
      .expected_draft_revision =
          value.at("expected_draft_revision").get<std::size_t>(),
  };
  if (!valid_uuid_v4(result.session_id) || !valid_uuid_v4(result.draft_id) ||
      result.expected_session_revision == 0 ||
      result.expected_draft_revision == 0)
    throw std::runtime_error{"Invalid session-summary payload."};
  return result;
}

[[nodiscard]] Json encode_session_context_purge(
    const SessionContextPurgeJobPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  Json result{{"payload_version", 1}, {"session_id", payload.session_id}};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] SessionContextPurgeJobPayload
decode_session_context_purge(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1)
    throw std::runtime_error{"Unsupported session-context purge payload."};
  SessionContextPurgeJobPayload result{
      .session_id = value.at("session_id").get<std::string>(),
  };
  if (!valid_uuid_v4(result.session_id))
    throw std::runtime_error{"Invalid session-context purge payload."};
  return result;
}

[[nodiscard]] Json encode_anniversary_scan(
    const AnniversaryScanJobPayload &payload,
    const std::string_view correlation_id = {},
    const std::optional<std::string> &causation_event_id = std::nullopt) {
  Json result{{"payload_version", 1},
              {"local_date", payload.local_date},
              {"test_run", payload.test_run}};
  add_trace(result, correlation_id, causation_event_id);
  return result;
}

[[nodiscard]] AnniversaryScanJobPayload
decode_anniversary_scan(const Json &value) {
  if (!value.is_object() || value.at("payload_version").get<int>() != 1)
    throw std::runtime_error{"Unsupported anniversary-scan payload."};
  AnniversaryScanJobPayload result{
      .local_date = value.at("local_date").get<std::string>(),
      .test_run = value.at("test_run").get<bool>(),
  };
  if (result.local_date.size() != 10)
    throw std::runtime_error{"Invalid anniversary-scan payload."};
  return result;
}

template <typename Payload>
[[nodiscard]] Payload decode_appearance_job(const Json &value) {
  if (!value.is_object() || value.size() < 2 ||
      value.at("payload_version").get<int>() != 1 ||
      !value.at("policy_version").is_string())
    throw std::runtime_error{"Unsupported appearance-job payload."};
  const auto version = value.at("policy_version").get<std::string>();
  if (version.empty() || version.size() > 80)
    throw std::runtime_error{"Invalid appearance-job payload."};
  return Payload{.policy_version = version};
}

[[nodiscard]] DurablePayload decode_payload(const std::string_view kind,
                                            const std::string &payload) {
  try {
    const auto value = Json::parse(payload);
    if (kind == pending_notice_outbox_kind ||
        kind == owner_test_notice_job_type) {
      return decode_notice(value);
    }
    if (kind == public_discord_outbox_kind ||
        kind == test_public_retry_outbox_kind) {
      return decode_message(value);
    }
    if (kind == "chronicle.memory-expire.v1") {
      return decode_memory_expiry(value);
    }
    if (kind == session_summary_job_type)
      return decode_session_summary(value);
    if (kind == session_context_purge_job_type)
      return decode_session_context_purge(value);
    if (kind == anniversary_scan_job_type)
      return decode_anniversary_scan(value);
    if (kind == "appearance.scan.v1")
      return decode_appearance_job<AppearanceScanJobPayload>(value);
    if (kind == "appearance.purge.v1")
      return decode_appearance_job<AppearancePurgeJobPayload>(value);
  } catch (const std::exception &) {
    return std::monostate{};
  }
  return std::monostate{};
}

[[nodiscard]] DurableTrace trace_from_payload(const std::string &payload,
                                              std::string fallback) {
  DurableTrace trace{.correlation_id = std::move(fallback),
                     .causation_event_id = std::nullopt};
  try {
    const auto value = Json::parse(payload);
    if (value.contains("correlation_id") &&
        value.at("correlation_id").is_string()) {
      const auto correlation = value.at("correlation_id").get<std::string>();
      if (!correlation.empty() && correlation.size() <= 160) {
        trace.correlation_id = correlation;
      }
    }
    if (value.contains("causation_event_id") &&
        value.at("causation_event_id").is_string()) {
      const auto causation = value.at("causation_event_id").get<std::string>();
      if (valid_uuid_v4(causation)) {
        trace.causation_event_id = causation;
      }
    }
  } catch (...) {
  }
  return trace;
}

[[nodiscard]] bool event_matches(SqliteConnection &connection,
                                 const EventJournalEntry &event) {
  auto query = connection.prepare(
      "SELECT event_id, event_type, aggregate_type, aggregate_id, "
      "actor_user_id, guild_id, channel_id, source_message_id, occurred_at_ms, "
      "recorded_at_ms, correlation_id, causation_id, payload_json "
      "FROM event_journal WHERE idempotency_key = ?");
  query.bind(1, event.idempotency_key);
  if (!query.step()) {
    return false;
  }
  const bool matches =
      query.column_text(0) == event.event_id &&
      query.column_text(1) == event.event_type &&
      query.column_text(2) == event.aggregate_type &&
      query.column_text(3) == event.aggregate_id &&
      optional_text(query, 4) ==
          (event.actor_user_id.has_value()
               ? std::optional<std::string>{event.actor_user_id->str()}
               : std::nullopt) &&
      query.column_text(5) == event.guild_id.str() &&
      optional_text(query, 6) ==
          (event.channel_id.has_value()
               ? std::optional<std::string>{event.channel_id->str()}
               : std::nullopt) &&
      optional_text(query, 7) ==
          (event.source_message_id.has_value()
               ? std::optional<std::string>{event.source_message_id->str()}
               : std::nullopt) &&
      query.column_int64(8) == event.occurred_at_ms &&
      query.column_int64(9) == event.recorded_at_ms &&
      query.column_text(10) == event.correlation_id &&
      optional_text(query, 11) == event.causation_id &&
      query.column_text(12) == event.payload_json;
  if (query.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Event idempotency query returned duplicate rows."};
  }
  if (!matches) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Event idempotency key conflicts with existing data."};
  }
  return true;
}

[[nodiscard]] bool insert_event(SqliteConnection &connection,
                                const EventJournalEntry &event) {
  if (!valid_uuid_v4(event.event_id) || !event.guild_id.is_set()) {
    throw std::invalid_argument{"Event identity or guild is invalid."};
  }
  require_type(event.event_type, 96);
  require_type(event.aggregate_type, 64);
  require_key(event.idempotency_key);
  require_timestamp(event.occurred_at_ms);
  require_timestamp(event.recorded_at_ms);
  if (event_matches(connection, event)) {
    return false;
  }
  auto insert = connection.prepare(
      "INSERT INTO event_journal "
      "(event_id, event_type, aggregate_type, aggregate_id, actor_user_id, "
      "guild_id, channel_id, source_message_id, occurred_at_ms, "
      "recorded_at_ms, correlation_id, causation_id, idempotency_key, "
      "payload_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, event.event_id);
  insert.bind(2, event.event_type);
  insert.bind(3, event.aggregate_type);
  insert.bind(4, event.aggregate_id);
  bind_snowflake(insert, 5, event.actor_user_id);
  insert.bind(6, event.guild_id.str());
  bind_snowflake(insert, 7, event.channel_id);
  bind_snowflake(insert, 8, event.source_message_id);
  insert.bind(9, event.occurred_at_ms);
  insert.bind(10, event.recorded_at_ms);
  insert.bind(11, event.correlation_id);
  bind_optional(insert, 12, event.causation_id);
  insert.bind(13, event.idempotency_key);
  insert.bind(14, event.payload_json);
  insert.execute();
  return true;
}

[[nodiscard]] bool compound_replay(SqliteConnection &connection,
                                   const EventJournalEntry &event) {
  auto query = connection.prepare(
      "SELECT event_type, aggregate_type, aggregate_id, actor_user_id, "
      "guild_id, channel_id, source_message_id, correlation_id, causation_id, "
      "payload_json FROM event_journal WHERE idempotency_key = ?");
  query.bind(1, event.idempotency_key);
  if (!query.step()) {
    return false;
  }
  const bool matches =
      query.column_text(0) == event.event_type &&
      query.column_text(1) == event.aggregate_type &&
      query.column_text(2) == event.aggregate_id &&
      optional_text(query, 3) ==
          (event.actor_user_id.has_value()
               ? std::optional<std::string>{event.actor_user_id->str()}
               : std::nullopt) &&
      query.column_text(4) == event.guild_id.str() &&
      optional_text(query, 5) ==
          (event.channel_id.has_value()
               ? std::optional<std::string>{event.channel_id->str()}
               : std::nullopt) &&
      optional_text(query, 6) ==
          (event.source_message_id.has_value()
               ? std::optional<std::string>{event.source_message_id->str()}
               : std::nullopt) &&
      query.column_text(7) == event.correlation_id &&
      optional_text(query, 8) == event.causation_id &&
      query.column_text(9) == event.payload_json;
  if (!matches) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Compound operation idempotency conflict."};
  }
  return true;
}

[[nodiscard]] Json normalized_replay_payload(const std::string_view kind,
                                             const std::string &payload_json) {
  auto payload = Json::parse(payload_json);
  payload.erase("causation_event_id");
  if (kind == pending_notice_outbox_kind ||
      kind == owner_test_notice_job_type) {
    const auto created_at = payload.at("created_at_ms").get<std::int64_t>();
    const auto expires_at = payload.at("expires_at_ms").get<std::int64_t>();
    payload.erase("notice_id");
    payload.erase("token_id");
    payload["created_at_ms"] = 0;
    payload["expires_at_ms"] = expires_at - created_at;
  }
  return payload;
}

[[noreturn]] void throw_compound_conflict() {
  throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                      SQLITE_CONSTRAINT,
                      "Compound operation idempotency conflict."};
}

[[nodiscard]] bool compound_job_replay(SqliteConnection &connection,
                                       const EventJournalEntry &event,
                                       const ScheduledJobEnqueue &job,
                                       const std::string &payload_json) {
  if (!compound_replay(connection, event)) {
    return false;
  }
  auto query = connection.prepare(
      "SELECT job_type, aggregate_type, aggregate_id, payload_json, "
      "due_at_ms, max_attempts, created_at_ms FROM scheduled_job "
      "WHERE idempotency_key = ?");
  query.bind(1, job.idempotency_key);
  if (!query.step()) {
    throw_compound_conflict();
  }
  try {
    const bool matches =
        query.column_text(0) == job.job_type &&
        optional_text(query, 1) == job.aggregate_type &&
        optional_text(query, 2) == job.aggregate_id &&
        normalized_replay_payload(job.job_type, query.column_text(3)) ==
            normalized_replay_payload(job.job_type, payload_json) &&
        query.column_int64(4) - query.column_int64(6) ==
            job.due_at_ms - job.created_at_ms &&
        query.column_int64(5) == static_cast<std::int64_t>(job.max_attempts);
    if (!matches || query.step()) {
      throw_compound_conflict();
    }
  } catch (const DatabaseError &) {
    throw;
  } catch (...) {
    throw_compound_conflict();
  }
  return true;
}

[[nodiscard]] bool compound_outbox_replay(SqliteConnection &connection,
                                          const EventJournalEntry &event,
                                          const OutboxEnqueue &outbox,
                                          const std::string &payload_json) {
  if (!compound_replay(connection, event)) {
    return false;
  }
  auto query = connection.prepare(
      "SELECT kind, aggregate_type, aggregate_id, target_guild_id, "
      "target_channel_id, target_user_id, payload_json, available_at_ms, "
      "max_attempts, created_at_ms FROM outbox_message "
      "WHERE idempotency_key = ?");
  query.bind(1, outbox.idempotency_key);
  if (!query.step()) {
    throw_compound_conflict();
  }
  try {
    const bool matches =
        query.column_text(0) == outbox.kind &&
        optional_text(query, 1) == outbox.aggregate_type &&
        optional_text(query, 2) == outbox.aggregate_id &&
        query.column_text(3) == outbox.target_guild_id.str() &&
        query.column_text(4) == outbox.target_channel_id.str() &&
        optional_text(query, 5) ==
            (outbox.target_user_id.has_value()
                 ? std::optional<std::string>{outbox.target_user_id->str()}
                 : std::nullopt) &&
        normalized_replay_payload(outbox.kind, query.column_text(6)) ==
            normalized_replay_payload(outbox.kind, payload_json) &&
        query.column_int64(7) - query.column_int64(9) ==
            outbox.available_at_ms - outbox.created_at_ms &&
        query.column_int64(8) == static_cast<std::int64_t>(outbox.max_attempts);
    if (!matches || query.step()) {
      throw_compound_conflict();
    }
  } catch (const DatabaseError &) {
    throw;
  } catch (...) {
    throw_compound_conflict();
  }
  return true;
}

[[nodiscard]] bool job_matches(SqliteConnection &connection,
                               const ScheduledJobEnqueue &job,
                               const std::string &payload_json) {
  auto query = connection.prepare(
      "SELECT job_id, job_type, aggregate_type, aggregate_id, payload_json, "
      "due_at_ms, max_attempts, created_at_ms FROM scheduled_job "
      "WHERE idempotency_key = ?");
  query.bind(1, job.idempotency_key);
  if (!query.step()) {
    return false;
  }
  const bool matches =
      query.column_text(0) == job.job_id &&
      query.column_text(1) == job.job_type &&
      optional_text(query, 2) == job.aggregate_type &&
      optional_text(query, 3) == job.aggregate_id &&
      query.column_text(4) == payload_json &&
      query.column_int64(5) == job.due_at_ms &&
      query.column_int64(6) == static_cast<std::int64_t>(job.max_attempts) &&
      query.column_int64(7) == job.created_at_ms;
  if (!matches) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Job idempotency key conflicts with existing data."};
  }
  return true;
}

[[nodiscard]] bool insert_job(SqliteConnection &connection,
                              const ScheduledJobEnqueue &job,
                              const std::string &payload_json) {
  if (!valid_uuid_v4(job.job_id) || job.max_attempts == 0 ||
      job.max_attempts > 20) {
    throw std::invalid_argument{"Scheduled job metadata is invalid."};
  }
  require_type(job.job_type, 96);
  require_key(job.idempotency_key);
  require_timestamp(job.created_at_ms);
  require_timestamp(job.due_at_ms);
  if (job_matches(connection, job, payload_json)) {
    return false;
  }
  auto insert = connection.prepare(
      "INSERT INTO scheduled_job "
      "(job_id, job_type, aggregate_type, aggregate_id, payload_json, "
      "due_at_ms, "
      "state, attempt_count, max_attempts, idempotency_key, created_at_ms, "
      "updated_at_ms) VALUES (?, ?, ?, ?, ?, ?, 'pending', 0, ?, ?, ?, ?)");
  insert.bind(1, job.job_id);
  insert.bind(2, job.job_type);
  bind_optional(insert, 3, job.aggregate_type);
  bind_optional(insert, 4, job.aggregate_id);
  insert.bind(5, payload_json);
  insert.bind(6, job.due_at_ms);
  insert.bind(7, static_cast<std::int64_t>(job.max_attempts));
  insert.bind(8, job.idempotency_key);
  insert.bind(9, job.created_at_ms);
  insert.bind(10, job.created_at_ms);
  insert.execute();
  return true;
}

[[nodiscard]] bool outbox_matches(SqliteConnection &connection,
                                  const OutboxEnqueue &outbox,
                                  const std::string &payload_json) {
  auto query = connection.prepare(
      "SELECT outbox_id, kind, aggregate_type, aggregate_id, target_guild_id, "
      "target_channel_id, target_user_id, payload_json, available_at_ms, "
      "max_attempts, provider_nonce, created_at_ms FROM outbox_message "
      "WHERE idempotency_key = ?");
  query.bind(1, outbox.idempotency_key);
  if (!query.step()) {
    return false;
  }
  const bool matches =
      query.column_text(0) == outbox.outbox_id &&
      query.column_text(1) == outbox.kind &&
      optional_text(query, 2) == outbox.aggregate_type &&
      optional_text(query, 3) == outbox.aggregate_id &&
      query.column_text(4) == outbox.target_guild_id.str() &&
      query.column_text(5) == outbox.target_channel_id.str() &&
      optional_text(query, 6) ==
          (outbox.target_user_id.has_value()
               ? std::optional<std::string>{outbox.target_user_id->str()}
               : std::nullopt) &&
      query.column_text(7) == payload_json &&
      query.column_int64(8) == outbox.available_at_ms &&
      query.column_int64(9) == static_cast<std::int64_t>(outbox.max_attempts) &&
      query.column_text(10) == outbox.provider_nonce &&
      query.column_int64(11) == outbox.created_at_ms;
  if (!matches) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Outbox idempotency key conflicts with existing data."};
  }
  return true;
}

[[nodiscard]] bool insert_outbox(SqliteConnection &connection,
                                 const OutboxEnqueue &outbox,
                                 const std::string &payload_json) {
  if (!valid_uuid_v4(outbox.outbox_id) || !outbox.target_guild_id.is_set() ||
      !outbox.target_channel_id.is_set() || outbox.max_attempts == 0 ||
      outbox.max_attempts > 20 || outbox.provider_nonce.size() != 25) {
    throw std::invalid_argument{"Outbox metadata is invalid."};
  }
  require_type(outbox.kind, 96);
  require_key(outbox.idempotency_key);
  require_timestamp(outbox.created_at_ms);
  require_timestamp(outbox.available_at_ms);
  if (outbox_matches(connection, outbox, payload_json)) {
    return false;
  }
  auto insert = connection.prepare(
      "INSERT INTO outbox_message "
      "(outbox_id, kind, aggregate_type, aggregate_id, target_guild_id, "
      "target_channel_id, target_user_id, payload_json, state, attempt_count, "
      "max_attempts, idempotency_key, provider_nonce, created_at_ms, "
      "available_at_ms, updated_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending', 0, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, outbox.outbox_id);
  insert.bind(2, outbox.kind);
  bind_optional(insert, 3, outbox.aggregate_type);
  bind_optional(insert, 4, outbox.aggregate_id);
  insert.bind(5, outbox.target_guild_id.str());
  insert.bind(6, outbox.target_channel_id.str());
  bind_snowflake(insert, 7, outbox.target_user_id);
  insert.bind(8, payload_json);
  insert.bind(9, static_cast<std::int64_t>(outbox.max_attempts));
  insert.bind(10, outbox.idempotency_key);
  insert.bind(11, outbox.provider_nonce);
  insert.bind(12, outbox.created_at_ms);
  insert.bind(13, outbox.available_at_ms);
  insert.bind(14, outbox.created_at_ms);
  insert.execute();
  return true;
}

void create_notice(SqliteConnection &connection,
                   const NoticeOutboxPayload &payload) {
  const auto &request = payload.notice;
  Json actions = Json::array();
  for (const auto &action : request.content.actions) {
    actions.push_back(
        {{"custom_id", action.custom_id}, {"label", action.label}});
  }
  const auto payload_json = Json{
      {"title", request.content.title},
      {"body", request.content.body},
      {"actions",
       std::move(actions)}}.dump();

  auto notice = connection.prepare(
      "INSERT INTO pending_notice "
      "(notice_id, target_user_id, notice_type, payload_json, "
      "source_aggregate_type, source_aggregate_id, state, expires_at_ms, "
      "idempotency_key, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, 'pending', ?, ?, ?) "
      "ON CONFLICT(idempotency_key) DO NOTHING");
  notice.bind(1, request.notice_id);
  notice.bind(2, request.target_user_id.str());
  notice.bind(3, request.notice_type);
  notice.bind(4, payload_json);
  bind_optional(notice, 5, request.source_aggregate_type);
  bind_optional(notice, 6, request.source_aggregate_id);
  notice.bind(7, request.expires_at_ms);
  notice.bind(8, request.notice_idempotency_key);
  notice.bind(9, request.created_at_ms);
  notice.execute();

  auto verify_notice = connection.prepare(
      "SELECT notice_id, target_user_id, notice_type, payload_json, "
      "source_aggregate_type, source_aggregate_id, expires_at_ms, "
      "created_at_ms "
      "FROM pending_notice WHERE idempotency_key = ?");
  verify_notice.bind(1, request.notice_idempotency_key);
  if (!verify_notice.step() ||
      verify_notice.column_text(0) != request.notice_id ||
      verify_notice.column_text(1) != request.target_user_id.str() ||
      verify_notice.column_text(2) != request.notice_type ||
      verify_notice.column_text(3) != payload_json ||
      optional_text(verify_notice, 4) != request.source_aggregate_type ||
      optional_text(verify_notice, 5) != request.source_aggregate_id ||
      verify_notice.column_int64(6) != request.expires_at_ms ||
      verify_notice.column_int64(7) != request.created_at_ms) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Pending notice idempotency conflict."};
  }

  auto token = connection.prepare(
      "INSERT INTO interaction_token "
      "(token_id, token_version, interaction_kind, action, entity_type, "
      "entity_id, expected_user_id, guild_id, channel_id, state, "
      "expires_at_ms, idempotency_key, created_at_ms) "
      "VALUES (?, 1, 'button', 'notice.open', 'pending_notice', ?, ?, ?, ?, "
      "'active', ?, ?, ?) ON CONFLICT(idempotency_key) DO NOTHING");
  token.bind(1, request.token_id);
  token.bind(2, request.notice_id);
  token.bind(3, request.target_user_id.str());
  token.bind(4, request.guild_id.str());
  token.bind(5, request.channel_id.str());
  token.bind(6, request.expires_at_ms);
  token.bind(7, request.token_idempotency_key);
  token.bind(8, request.created_at_ms);
  token.execute();

  auto verify_token = connection.prepare(
      "SELECT token_id, token_version, interaction_kind, action, entity_type, "
      "entity_id, expected_user_id, guild_id, channel_id, expires_at_ms, "
      "created_at_ms FROM interaction_token "
      "WHERE idempotency_key = ?");
  verify_token.bind(1, request.token_idempotency_key);
  if (!verify_token.step() || verify_token.column_text(0) != request.token_id ||
      verify_token.column_int64(1) != 1 ||
      verify_token.column_text(2) != "button" ||
      verify_token.column_text(3) != "notice.open" ||
      verify_token.column_text(4) != "pending_notice" ||
      verify_token.column_text(5) != request.notice_id ||
      verify_token.column_text(6) != request.target_user_id.str() ||
      verify_token.column_text(7) != request.guild_id.str() ||
      verify_token.column_text(8) != request.channel_id.str() ||
      verify_token.column_int64(9) != request.expires_at_ms ||
      verify_token.column_int64(10) != request.created_at_ms) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Pending notice token idempotency conflict."};
  }
}

[[nodiscard]] WorkMutationStatus stale_job_status(SqliteConnection &connection,
                                                  const std::string_view id) {
  auto query =
      connection.prepare("SELECT state FROM scheduled_job WHERE job_id = ?");
  query.bind(1, id);
  if (!query.step()) {
    return WorkMutationStatus::not_found;
  }
  return query.column_text(0) == "completed" ? WorkMutationStatus::unchanged
                                             : WorkMutationStatus::stale_claim;
}

[[nodiscard]] WorkMutationStatus
stale_outbox_status(SqliteConnection &connection, const std::string_view id) {
  auto query = connection.prepare(
      "SELECT state FROM outbox_message WHERE outbox_id = ?");
  query.bind(1, id);
  if (!query.step()) {
    return WorkMutationStatus::not_found;
  }
  return query.column_text(0) == "delivered" ? WorkMutationStatus::unchanged
                                             : WorkMutationStatus::stale_claim;
}

[[nodiscard]] std::int64_t scalar(SqliteConnection &connection,
                                  const std::string_view sql) {
  auto query = connection.prepare(sql);
  if (!query.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Durable health query failed."};
  }
  return query.column_int64(0);
}

[[nodiscard]] std::optional<std::string>
latest_error(SqliteConnection &connection, const std::string_view table) {
  auto query =
      connection.prepare("SELECT last_error_code FROM " + std::string{table} +
                         " WHERE last_error_code IS NOT NULL "
                         "ORDER BY updated_at_ms DESC LIMIT 1");
  if (!query.step()) {
    return std::nullopt;
  }
  return query.column_text(0);
}

} // namespace

namespace detail {

bool insert_event_uncommitted(SqliteConnection &connection,
                              const EventJournalEntry &event) {
  return insert_event(connection, event);
}

bool insert_job_uncommitted(SqliteConnection &connection,
                            const ScheduledJobEnqueue &job,
                            const std::string &payload_json) {
  return insert_job(connection, job, payload_json);
}

bool insert_outbox_uncommitted(SqliteConnection &connection,
                               const OutboxEnqueue &outbox,
                               const std::string &payload_json) {
  return insert_outbox(connection, outbox, payload_json);
}

std::string
encode_public_payload(const PublicOutboxPayload &payload,
                      const std::string_view correlation_id,
                      const std::optional<std::string> &causation_event_id) {
  return encode_message(payload, correlation_id, causation_event_id).dump();
}

std::string
encode_notice_payload(const NoticeOutboxPayload &payload,
                      const std::string_view correlation_id,
                      const std::optional<std::string> &causation_event_id) {
  return encode_notice(payload, correlation_id, causation_event_id).dump();
}

std::string encode_memory_expiry_payload(
    const MemoryExpiryJobPayload &payload,
    const std::string_view correlation_id,
    const std::optional<std::string> &causation_event_id) {
  return encode_memory_expiry(payload, correlation_id, causation_event_id)
      .dump();
}

std::string encode_session_summary_payload(
    const SessionSummaryJobPayload &payload,
    const std::string_view correlation_id,
    const std::optional<std::string> &causation_event_id) {
  return encode_session_summary(payload, correlation_id, causation_event_id)
      .dump();
}

std::string encode_session_context_purge_payload(
    const SessionContextPurgeJobPayload &payload,
    const std::string_view correlation_id,
    const std::optional<std::string> &causation_event_id) {
  return encode_session_context_purge(payload, correlation_id,
                                      causation_event_id)
      .dump();
}

std::string encode_anniversary_scan_payload(
    const AnniversaryScanJobPayload &payload,
    const std::string_view correlation_id,
    const std::optional<std::string> &causation_event_id) {
  return encode_anniversary_scan(payload, correlation_id, causation_event_id)
      .dump();
}

} // namespace detail

SqliteDurableWorkRepository::SqliteDurableWorkRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_) {
    throw std::invalid_argument{"SQLite repository context is required."};
  }
}

bool SqliteDurableWorkRepository::append_event(const EventJournalEntry &event) {
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  const bool created =
      detail::insert_event_uncommitted(context_->connection(), event);
  transaction.commit();
  return created;
}

std::vector<EventJournalEntry>
SqliteDurableWorkRepository::recent_events(const std::size_t limit) {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT event_id, event_type, aggregate_type, aggregate_id, "
      "actor_user_id, guild_id, channel_id, source_message_id, "
      "occurred_at_ms, recorded_at_ms, correlation_id, causation_id, "
      "idempotency_key, payload_json FROM event_journal "
      "ORDER BY recorded_at_ms DESC, event_id DESC LIMIT ?");
  query.bind(1, static_cast<std::int64_t>(std::min<std::size_t>(limit, 100)));
  return read_event_rows(query);
}

std::vector<EventJournalEntry>
SqliteDurableWorkRepository::events_by_type(const std::string_view event_type,
                                            const std::size_t limit) {
  require_type(event_type, 96);
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT event_id, event_type, aggregate_type, aggregate_id, "
      "actor_user_id, guild_id, channel_id, source_message_id, "
      "occurred_at_ms, recorded_at_ms, correlation_id, causation_id, "
      "idempotency_key, payload_json FROM event_journal WHERE event_type = ? "
      "ORDER BY occurred_at_ms DESC, event_id DESC LIMIT ?");
  query.bind(1, event_type);
  query.bind(2, static_cast<std::int64_t>(std::min<std::size_t>(limit, 100)));
  return read_event_rows(query);
}

std::vector<EventJournalEntry> SqliteDurableWorkRepository::aggregate_history(
    const std::string_view aggregate_type, const std::string_view aggregate_id,
    const std::size_t limit) {
  require_type(aggregate_type, 64);
  if (aggregate_id.empty() || aggregate_id.size() > 128) {
    throw std::invalid_argument{"Aggregate ID is invalid."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT event_id, event_type, aggregate_type, aggregate_id, "
      "actor_user_id, guild_id, channel_id, source_message_id, "
      "occurred_at_ms, recorded_at_ms, correlation_id, causation_id, "
      "idempotency_key, payload_json FROM event_journal "
      "WHERE aggregate_type = ? AND aggregate_id = ? "
      "ORDER BY occurred_at_ms DESC, event_id DESC LIMIT ?");
  query.bind(1, aggregate_type);
  query.bind(2, aggregate_id);
  query.bind(3, static_cast<std::int64_t>(std::min<std::size_t>(limit, 100)));
  return read_event_rows(query);
}

bool SqliteDurableWorkRepository::enqueue_notice(
    const EventJournalEntry &event, const OutboxEnqueue &outbox,
    const NoticeOutboxPayload &payload) {
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  const auto payload_json =
      encode_notice(payload, event.correlation_id, event.event_id).dump();
  if (compound_outbox_replay(context_->connection(), event, outbox,
                             payload_json)) {
    transaction.commit();
    return false;
  }
  const bool event_created =
      detail::insert_event_uncommitted(context_->connection(), event);
  const bool outbox_created = detail::insert_outbox_uncommitted(
      context_->connection(), outbox, payload_json);
  transaction.commit();
  return event_created || outbox_created;
}

bool SqliteDurableWorkRepository::schedule_notice(
    const EventJournalEntry &event, const ScheduledJobEnqueue &job,
    const NoticeOutboxPayload &payload) {
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  const auto payload_json =
      encode_notice(payload, event.correlation_id, event.event_id).dump();
  if (compound_job_replay(context_->connection(), event, job, payload_json)) {
    transaction.commit();
    return false;
  }
  const bool event_created =
      detail::insert_event_uncommitted(context_->connection(), event);
  const bool job_created =
      detail::insert_job_uncommitted(context_->connection(), job, payload_json);
  transaction.commit();
  return event_created || job_created;
}

bool SqliteDurableWorkRepository::enqueue_public(
    const EventJournalEntry &event, const OutboxEnqueue &outbox,
    const PublicOutboxPayload &payload) {
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  const auto payload_json =
      encode_message(payload, event.correlation_id, event.event_id).dump();
  if (compound_outbox_replay(context_->connection(), event, outbox,
                             payload_json)) {
    transaction.commit();
    return false;
  }
  const bool event_created =
      detail::insert_event_uncommitted(context_->connection(), event);
  const bool outbox_created = detail::insert_outbox_uncommitted(
      context_->connection(), outbox, payload_json);
  transaction.commit();
  return event_created || outbox_created;
}

std::optional<ClaimedScheduledJob> SqliteDurableWorkRepository::claim_due_job(
    const std::int64_t now_ms, const std::int64_t lease_until_ms,
    std::string lease_owner, std::string lease_token) {
  require_timestamp(now_ms);
  require_timestamp(lease_until_ms);
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto exhaust = context_->connection().prepare(
      "UPDATE scheduled_job SET state = 'dead', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "last_error_code = 'attempts_exhausted' "
      "WHERE ((state = 'pending' AND due_at_ms <= ?) OR "
      "(state = 'claimed' AND lease_until_ms <= ?)) "
      "AND attempt_count >= max_attempts");
  exhaust.bind(1, now_ms);
  exhaust.bind(2, now_ms);
  exhaust.bind(3, now_ms);
  exhaust.bind(4, now_ms);
  exhaust.execute();

  auto select = context_->connection().prepare(
      "SELECT job_id, job_type, payload_json, due_at_ms, attempt_count, "
      "max_attempts FROM scheduled_job WHERE "
      "((state = 'pending' AND due_at_ms <= ?) OR "
      "(state = 'claimed' AND lease_until_ms <= ?)) "
      "AND attempt_count < max_attempts ORDER BY due_at_ms, job_id LIMIT 1");
  select.bind(1, now_ms);
  select.bind(2, now_ms);
  if (!select.step()) {
    transaction.commit();
    return std::nullopt;
  }
  const auto job_id = select.column_text(0);
  const auto job_type = select.column_text(1);
  const auto payload_json = select.column_text(2);
  const auto due_at_ms = select.column_int64(3);
  const auto attempts = select.column_int64(4) + 1;
  const auto maximum = select.column_int64(5);

  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET state = 'claimed', attempt_count = ?, "
      "lease_owner = ?, lease_token = ?, lease_until_ms = ?, "
      "updated_at_ms = max(?, updated_at_ms) "
      "WHERE job_id = ?");
  update.bind(1, attempts);
  update.bind(2, lease_owner);
  update.bind(3, lease_token);
  update.bind(4, lease_until_ms);
  update.bind(5, now_ms);
  update.bind(6, job_id);
  update.execute();
  transaction.commit();

  auto trace = trace_from_payload(payload_json, job_id);
  return ClaimedScheduledJob{
      .job_id = job_id,
      .job_type = job_type,
      .lease_owner = std::move(lease_owner),
      .lease_token = std::move(lease_token),
      .attempt_count = static_cast<std::size_t>(attempts),
      .max_attempts = static_cast<std::size_t>(maximum),
      .due_at_ms = due_at_ms,
      .payload = decode_payload(job_type, payload_json),
      .correlation_id = std::move(trace.correlation_id),
      .causation_event_id = std::move(trace.causation_event_id),
  };
}

WorkMutationStatus SqliteDurableWorkRepository::complete_notice_job(
    const ClaimedScheduledJob &job, const EventJournalEntry &event,
    const OutboxEnqueue &outbox, const std::int64_t now_ms) {
  const auto *payload = std::get_if<NoticeOutboxPayload>(&job.payload);
  if (payload == nullptr || event.correlation_id != job.correlation_id ||
      event.causation_id != job.causation_event_id) {
    return WorkMutationStatus::invalid_state;
  }
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto owned = context_->connection().prepare(
      "SELECT 1 FROM scheduled_job WHERE job_id = ? AND state = 'claimed' "
      "AND lease_owner = ? AND lease_token = ?");
  owned.bind(1, job.job_id);
  owned.bind(2, job.lease_owner);
  owned.bind(3, job.lease_token);
  if (!owned.step()) {
    const auto status = stale_job_status(context_->connection(), job.job_id);
    transaction.commit();
    return status;
  }
  static_cast<void>(
      detail::insert_event_uncommitted(context_->connection(), event));
  static_cast<void>(detail::insert_outbox_uncommitted(
      context_->connection(), outbox,
      encode_notice(*payload, job.correlation_id, event.event_id).dump()));
  auto complete = context_->connection().prepare(
      "UPDATE scheduled_job SET state = 'completed', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "completed_at_ms = max(?, created_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), last_error_code = NULL "
      "WHERE job_id = ? "
      "AND state = 'claimed' AND lease_token = ?");
  complete.bind(1, now_ms);
  complete.bind(2, now_ms);
  complete.bind(3, now_ms);
  complete.bind(4, job.job_id);
  complete.bind(5, job.lease_token);
  complete.execute();
  transaction.commit();
  return WorkMutationStatus::applied;
}

WorkMutationStatus
SqliteDurableWorkRepository::release_job(const ClaimedScheduledJob &job,
                                         const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET state = 'pending', "
      "attempt_count = max(attempt_count - 1, 0), lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = NULL WHERE job_id = ? AND state = 'claimed' "
      "AND lease_owner = ? AND lease_token = ?");
  update.bind(1, now_ms);
  update.bind(2, job.job_id);
  update.bind(3, job.lease_owner);
  update.bind(4, job.lease_token);
  update.execute();
  if (context_->connection().changes() == 0) {
    return stale_job_status(context_->connection(), job.job_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::defer_job(
    const ClaimedScheduledJob &job, const std::int64_t now_ms,
    const std::int64_t retry_at_ms, std::string error_code) {
  require_timestamp(now_ms);
  require_timestamp(retry_at_ms);
  require_type(error_code, 96);
  if (retry_at_ms <= now_ms)
    throw std::invalid_argument{"A deferred job must move into the future."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET state='pending',due_at_ms=?,"
      "attempt_count=max(attempt_count-1,0),lease_owner=NULL,lease_token=NULL,"
      "lease_until_ms=NULL,updated_at_ms=max(?,updated_at_ms),terminal_at_ms="
      "NULL,"
      "last_error_code=? WHERE job_id=? AND state='claimed' AND lease_owner=? "
      "AND lease_token=?");
  update.bind(1, retry_at_ms);
  update.bind(2, now_ms);
  update.bind(3, error_code);
  update.bind(4, job.job_id);
  update.bind(5, job.lease_owner);
  update.bind(6, job.lease_token);
  update.execute();
  if (context_->connection().changes() == 0)
    return stale_job_status(context_->connection(), job.job_id);
  return WorkMutationStatus::applied;
}

WorkMutationStatus
SqliteDurableWorkRepository::reschedule_job(const ClaimedScheduledJob &job,
                                            const std::int64_t now_ms,
                                            const std::int64_t due_at_ms) {
  require_timestamp(now_ms);
  require_timestamp(due_at_ms);
  if (due_at_ms <= now_ms)
    throw std::invalid_argument{"A rescheduled job must move into the future."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET state='pending',due_at_ms=?,"
      "attempt_count=max(attempt_count-1,0),lease_owner=NULL,lease_token=NULL,"
      "lease_until_ms=NULL,updated_at_ms=max(?,updated_at_ms),terminal_at_ms="
      "NULL,last_error_code=NULL WHERE job_id=? AND state='claimed' AND "
      "lease_owner=? AND lease_token=?");
  update.bind(1, due_at_ms);
  update.bind(2, now_ms);
  update.bind(3, job.job_id);
  update.bind(4, job.lease_owner);
  update.bind(5, job.lease_token);
  update.execute();
  if (context_->connection().changes() == 0)
    return stale_job_status(context_->connection(), job.job_id);
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::extend_job_lease(
    const ClaimedScheduledJob &job, const std::int64_t now_ms,
    const std::int64_t lease_until_ms) {
  require_timestamp(now_ms);
  require_timestamp(lease_until_ms);
  if (lease_until_ms <= now_ms)
    throw std::invalid_argument{
        "A renewed job lease must remain in the future."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET lease_until_ms=max(lease_until_ms,?),"
      "updated_at_ms=max(?,updated_at_ms) WHERE job_id=? AND state='claimed' "
      "AND lease_owner=? AND lease_token=?");
  update.bind(1, lease_until_ms);
  update.bind(2, now_ms);
  update.bind(3, job.job_id);
  update.bind(4, job.lease_owner);
  update.bind(5, job.lease_token);
  update.execute();
  if (context_->connection().changes() == 0)
    return stale_job_status(context_->connection(), job.job_id);
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::fail_job(
    const ClaimedScheduledJob &job, const std::int64_t now_ms,
    const std::int64_t retry_at_ms, std::string error_code,
    const bool retryable) {
  require_type(error_code, 96);
  const bool retry = retryable && job.attempt_count < job.max_attempts;
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      retry ? "UPDATE scheduled_job SET state = 'pending', due_at_ms = ?, "
              "lease_owner = NULL, lease_token = NULL, lease_until_ms = NULL, "
              "updated_at_ms = max(?, updated_at_ms), terminal_at_ms = NULL, "
              "last_error_code = ? "
              "WHERE job_id = ? "
              "AND state = 'claimed' AND lease_owner = ? AND lease_token = ?"
            : "UPDATE scheduled_job SET state = 'dead', lease_owner = NULL, "
              "lease_token = NULL, lease_until_ms = NULL, "
              "updated_at_ms = max(?, updated_at_ms), "
              "terminal_at_ms = max(?, created_at_ms), last_error_code = ? "
              "WHERE job_id = ? "
              "AND state = 'claimed' "
              "AND lease_owner = ? AND lease_token = ?");
  std::size_t index = 1;
  if (retry) {
    update.bind(index++, retry_at_ms);
  }
  update.bind(index++, now_ms);
  if (!retry) {
    update.bind(index++, now_ms);
  }
  update.bind(index++, error_code);
  update.bind(index++, job.job_id);
  update.bind(index++, job.lease_owner);
  update.bind(index, job.lease_token);
  update.execute();
  if (context_->connection().changes() == 0) {
    return stale_job_status(context_->connection(), job.job_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus
SqliteDurableWorkRepository::cancel_job(const std::string_view job_id,
                                        const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE scheduled_job SET state = 'cancelled', "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "lease_owner = NULL, lease_token = NULL, lease_until_ms = NULL "
      "WHERE job_id = ? AND state = 'pending'");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, job_id);
  update.execute();
  if (context_->connection().changes() != 0) {
    return WorkMutationStatus::applied;
  }
  auto query = context_->connection().prepare(
      "SELECT state FROM scheduled_job WHERE job_id = ?");
  query.bind(1, job_id);
  if (!query.step()) {
    return WorkMutationStatus::not_found;
  }
  return query.column_text(0) == "cancelled"
             ? WorkMutationStatus::unchanged
             : WorkMutationStatus::invalid_state;
}

std::optional<ClaimedOutboxMessage>
SqliteDurableWorkRepository::claim_due_outbox(
    const std::int64_t now_ms, const std::int64_t lease_until_ms,
    std::string lease_owner, std::string lease_token,
    const bool public_delivery_ready) {
  require_timestamp(now_ms);
  require_timestamp(lease_until_ms);
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto quarantine_submitted = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'failed', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "last_error_code = 'discord_unknown_outcome_stale' WHERE "
      "state = 'claimed' AND lease_until_ms <= ? "
      "AND attempt_count >= max_attempts "
      "AND submission_started_at_ms IS NOT NULL AND kind IN (?, ?)");
  quarantine_submitted.bind(1, now_ms);
  quarantine_submitted.bind(2, now_ms);
  quarantine_submitted.bind(3, now_ms);
  quarantine_submitted.bind(4, std::string{public_discord_outbox_kind});
  quarantine_submitted.bind(5, std::string{test_public_retry_outbox_kind});
  quarantine_submitted.execute();

  auto exhaust = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'dead', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "last_error_code = 'attempts_exhausted' WHERE "
      "((state = 'pending' AND available_at_ms <= ?) OR "
      "(state = 'claimed' AND lease_until_ms <= ?)) "
      "AND attempt_count >= max_attempts AND (? OR kind NOT IN (?, ?))");
  exhaust.bind(1, now_ms);
  exhaust.bind(2, now_ms);
  exhaust.bind(3, now_ms);
  exhaust.bind(4, public_delivery_ready ? 1LL : 0LL);
  exhaust.bind(5, std::string{public_discord_outbox_kind});
  exhaust.bind(6, std::string{test_public_retry_outbox_kind});
  exhaust.execute();

  auto select = context_->connection().prepare(
      "SELECT candidate.outbox_id, candidate.kind, candidate.payload_json, "
      "candidate.available_at_ms, candidate.attempt_count, "
      "candidate.max_attempts, candidate.first_attempt_at_ms, "
      "candidate.first_attempt_elapsed_ms, candidate.first_attempt_boot_id, "
      "candidate.submission_started_at_ms, candidate.provider_nonce, "
      "candidate.last_error_code "
      "FROM outbox_message AS candidate "
      "WHERE ((candidate.state = 'pending' AND "
      "candidate.available_at_ms <= ?) OR (candidate.state = 'claimed' AND "
      "candidate.lease_until_ms <= ?)) "
      "AND candidate.attempt_count < candidate.max_attempts "
      "AND (? OR candidate.kind NOT IN (?, ?)) "
      "AND (candidate.aggregate_type IS NULL OR "
      "candidate.aggregate_type <> 'chronicle_entry' OR NOT EXISTS ("
      "SELECT 1 FROM outbox_message AS predecessor WHERE "
      "predecessor.aggregate_type = 'chronicle_entry' AND "
      "predecessor.aggregate_id = candidate.aggregate_id AND "
      "predecessor.created_at_ms < candidate.created_at_ms AND "
      "predecessor.state IN ('pending','claimed'))) "
      "ORDER BY candidate.available_at_ms, candidate.outbox_id "
      "LIMIT 1");
  select.bind(1, now_ms);
  select.bind(2, now_ms);
  select.bind(3, public_delivery_ready ? 1LL : 0LL);
  select.bind(4, std::string{public_discord_outbox_kind});
  select.bind(5, std::string{test_public_retry_outbox_kind});
  if (!select.step()) {
    transaction.commit();
    return std::nullopt;
  }
  const auto outbox_id = select.column_text(0);
  const auto kind = select.column_text(1);
  const auto payload_json = select.column_text(2);
  const auto available_at_ms = select.column_int64(3);
  const auto attempts = select.column_int64(4) + 1;
  const auto maximum = select.column_int64(5);
  const auto first_attempt =
      select.column_is_null(6)
          ? std::nullopt
          : std::optional<std::int64_t>{select.column_int64(6)};
  const auto first_attempt_elapsed =
      select.column_is_null(7)
          ? std::nullopt
          : std::optional<std::int64_t>{select.column_int64(7)};
  const auto first_attempt_boot = optional_text(select, 8);
  const auto submission_started =
      select.column_is_null(9)
          ? std::nullopt
          : std::optional<std::int64_t>{select.column_int64(9)};
  const auto nonce = select.column_text(10);
  const auto last_error = optional_text(select, 11);

  auto update = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'claimed', attempt_count = ?, "
      "lease_owner = ?, lease_token = ?, lease_until_ms = ?, "
      "updated_at_ms = max(?, updated_at_ms) "
      "WHERE outbox_id = ?");
  update.bind(1, attempts);
  update.bind(2, lease_owner);
  update.bind(3, lease_token);
  update.bind(4, lease_until_ms);
  update.bind(5, now_ms);
  update.bind(6, outbox_id);
  update.execute();
  transaction.commit();

  auto trace = trace_from_payload(payload_json, outbox_id);
  return ClaimedOutboxMessage{
      .outbox_id = outbox_id,
      .kind = kind,
      .lease_owner = std::move(lease_owner),
      .lease_token = std::move(lease_token),
      .attempt_count = static_cast<std::size_t>(attempts),
      .max_attempts = static_cast<std::size_t>(maximum),
      .available_at_ms = available_at_ms,
      .first_attempt_at_ms = first_attempt,
      .first_attempt_elapsed_ms = first_attempt_elapsed,
      .first_attempt_boot_id = first_attempt_boot,
      .submission_started_at_ms = submission_started,
      .last_error_code = last_error,
      .provider_nonce = nonce,
      .payload = decode_payload(kind, payload_json),
      .correlation_id = std::move(trace.correlation_id),
      .causation_event_id = std::move(trace.causation_event_id),
  };
}

WorkMutationStatus SqliteDurableWorkRepository::complete_notice_outbox(
    const ClaimedOutboxMessage &outbox, const EventJournalEntry &event,
    std::optional<OutboxEnqueue> public_outbox,
    std::optional<PublicOutboxPayload> public_payload,
    const std::int64_t now_ms) {
  const auto *notice = std::get_if<NoticeOutboxPayload>(&outbox.payload);
  if (notice == nullptr ||
      public_outbox.has_value() != public_payload.has_value() ||
      event.correlation_id != outbox.correlation_id ||
      event.causation_id != outbox.causation_event_id) {
    return WorkMutationStatus::invalid_state;
  }
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto owned = context_->connection().prepare(
      "SELECT 1 FROM outbox_message WHERE outbox_id = ? AND state = 'claimed' "
      "AND lease_owner = ? AND lease_token = ?");
  owned.bind(1, outbox.outbox_id);
  owned.bind(2, outbox.lease_owner);
  owned.bind(3, outbox.lease_token);
  if (!owned.step()) {
    const auto status =
        stale_outbox_status(context_->connection(), outbox.outbox_id);
    transaction.commit();
    return status;
  }
  create_notice(context_->connection(), *notice);
  static_cast<void>(
      detail::insert_event_uncommitted(context_->connection(), event));
  if (public_outbox.has_value()) {
    static_cast<void>(detail::insert_outbox_uncommitted(
        context_->connection(), *public_outbox,
        encode_message(*public_payload, outbox.correlation_id, event.event_id)
            .dump()));
  }
  auto complete = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'delivered', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, "
      "delivered_at_ms = max(?, created_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "updated_at_ms = max(?, updated_at_ms), last_error_code = NULL "
      "WHERE outbox_id = ? "
      "AND state = 'claimed' AND lease_token = ?");
  complete.bind(1, now_ms);
  complete.bind(2, now_ms);
  complete.bind(3, now_ms);
  complete.bind(4, outbox.outbox_id);
  complete.bind(5, outbox.lease_token);
  complete.execute();
  transaction.commit();
  return WorkMutationStatus::applied;
}

WorkMutationStatus
SqliteDurableWorkRepository::release_outbox(const ClaimedOutboxMessage &outbox,
                                            const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'pending', "
      "attempt_count = max(attempt_count - 1, 0), lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = NULL WHERE outbox_id = ? AND state = 'claimed' "
      "AND lease_owner = ? AND lease_token = ? "
      "AND submission_started_at_ms IS NULL");
  update.bind(1, now_ms);
  update.bind(2, outbox.outbox_id);
  update.bind(3, outbox.lease_owner);
  update.bind(4, outbox.lease_token);
  update.execute();
  if (context_->connection().changes() == 0) {
    return stale_outbox_status(context_->connection(), outbox.outbox_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::mark_public_outbox_submitted(
    const ClaimedOutboxMessage &outbox, const DeliveryAttemptStamp &attempt,
    const std::int64_t lease_until_ms) {
  require_timestamp(attempt.wall_time_ms);
  require_timestamp(attempt.elapsed_realtime_ms);
  require_timestamp(lease_until_ms);
  if (lease_until_ms <= attempt.wall_time_ms) {
    throw std::invalid_argument{
        "A submitted outbox lease must extend beyond its attempt time."};
  }
  require_boot_session_id(attempt.boot_session_id);
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE outbox_message SET "
      "first_attempt_at_ms = COALESCE(first_attempt_at_ms, "
      "max(?, created_at_ms)), "
      "first_attempt_elapsed_ms = COALESCE(first_attempt_elapsed_ms, ?), "
      "first_attempt_boot_id = COALESCE(first_attempt_boot_id, ?), "
      "submission_started_at_ms = max(?, created_at_ms), "
      "lease_until_ms = max(lease_until_ms, ?), "
      "updated_at_ms = max(?, updated_at_ms) "
      "WHERE outbox_id = ? AND state = 'claimed' AND lease_owner = ? "
      "AND lease_token = ? AND kind IN (?, ?)");
  update.bind(1, attempt.wall_time_ms);
  update.bind(2, attempt.elapsed_realtime_ms);
  update.bind(3, attempt.boot_session_id);
  update.bind(4, attempt.wall_time_ms);
  update.bind(5, lease_until_ms);
  update.bind(6, attempt.wall_time_ms);
  update.bind(7, outbox.outbox_id);
  update.bind(8, outbox.lease_owner);
  update.bind(9, outbox.lease_token);
  update.bind(10, std::string{public_discord_outbox_kind});
  update.bind(11, std::string{test_public_retry_outbox_kind});
  update.execute();
  if (context_->connection().changes() == 0) {
    return stale_outbox_status(context_->connection(), outbox.outbox_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::complete_public_outbox(
    const ClaimedOutboxMessage &outbox,
    const DiscordSnowflake provider_message_id, const std::int64_t now_ms) {
  if (!provider_message_id.is_set()) {
    throw std::invalid_argument{"Provider message ID must be set."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto complete = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'delivered', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, provider_message_id = ?, "
      "delivered_at_ms = max(?, created_at_ms), "
      "terminal_at_ms = max(?, created_at_ms), "
      "updated_at_ms = max(?, updated_at_ms), "
      "last_error_code = NULL "
      "WHERE outbox_id = ? AND state = 'claimed' AND lease_owner = ? "
      "AND lease_token = ? AND submission_started_at_ms IS NOT NULL");
  complete.bind(1, provider_message_id.str());
  complete.bind(2, now_ms);
  complete.bind(3, now_ms);
  complete.bind(4, now_ms);
  complete.bind(5, outbox.outbox_id);
  complete.bind(6, outbox.lease_owner);
  complete.bind(7, outbox.lease_token);
  complete.execute();
  if (context_->connection().changes() == 0) {
    return stale_outbox_status(context_->connection(), outbox.outbox_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteDurableWorkRepository::fail_outbox(
    const ClaimedOutboxMessage &outbox, const std::int64_t now_ms,
    const std::int64_t retry_at_ms, std::string error_code,
    const OutboxFailureMode mode) {
  require_type(error_code, 96);
  const bool retry = mode == OutboxFailureMode::retryable &&
                     outbox.attempt_count < outbox.max_attempts;
  const std::string state = retry                               ? "pending"
                            : mode == OutboxFailureMode::failed ? "failed"
                                                                : "dead";
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE outbox_message SET state = ?, "
      "available_at_ms = max(?, created_at_ms), "
      "lease_owner = NULL, lease_token = NULL, lease_until_ms = NULL, "
      "submission_started_at_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = CASE WHEN ? IS NULL THEN NULL "
      "ELSE max(?, created_at_ms) END, last_error_code = ? "
      "WHERE outbox_id = ? "
      "AND state = 'claimed' AND lease_owner = ? AND lease_token = ?");
  update.bind(1, state);
  update.bind(2, retry ? retry_at_ms : outbox.available_at_ms);
  update.bind(3, now_ms);
  if (retry) {
    update.bind_null(4);
    update.bind_null(5);
  } else {
    update.bind(4, now_ms);
    update.bind(5, now_ms);
  }
  update.bind(6, error_code);
  update.bind(7, outbox.outbox_id);
  update.bind(8, outbox.lease_owner);
  update.bind(9, outbox.lease_token);
  update.execute();
  if (context_->connection().changes() == 0) {
    return stale_outbox_status(context_->connection(), outbox.outbox_id);
  }
  return WorkMutationStatus::applied;
}

WorkMutationStatus
SqliteDurableWorkRepository::cancel_outbox(const std::string_view outbox_id,
                                           const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE outbox_message SET state = 'cancelled', "
      "lease_owner = NULL, lease_token = NULL, lease_until_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms) "
      "WHERE outbox_id = ? AND state = 'pending'");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, outbox_id);
  update.execute();
  if (context_->connection().changes() != 0) {
    return WorkMutationStatus::applied;
  }
  auto query = context_->connection().prepare(
      "SELECT state FROM outbox_message WHERE outbox_id = ?");
  query.bind(1, outbox_id);
  if (!query.step()) {
    return WorkMutationStatus::not_found;
  }
  return query.column_text(0) == "cancelled"
             ? WorkMutationStatus::unchanged
             : WorkMutationStatus::invalid_state;
}

DurableWorkHealth
SqliteDurableWorkRepository::health(const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  DurableWorkHealth result{
      .pending_jobs = static_cast<std::size_t>(
          scalar(context_->connection(),
                 "SELECT count(*) FROM scheduled_job WHERE state = 'pending'")),
      .claimed_jobs = static_cast<std::size_t>(
          scalar(context_->connection(),
                 "SELECT count(*) FROM scheduled_job WHERE state = 'claimed'")),
      .dead_jobs = static_cast<std::size_t>(
          scalar(context_->connection(),
                 "SELECT count(*) FROM scheduled_job WHERE state = 'dead'")),
      .pending_outbox = static_cast<std::size_t>(scalar(
          context_->connection(),
          "SELECT count(*) FROM outbox_message WHERE state = 'pending'")),
      .claimed_outbox = static_cast<std::size_t>(scalar(
          context_->connection(),
          "SELECT count(*) FROM outbox_message WHERE state = 'claimed'")),
      .failed_outbox = static_cast<std::size_t>(
          scalar(context_->connection(),
                 "SELECT count(*) FROM outbox_message WHERE state = 'failed'")),
      .dead_outbox = static_cast<std::size_t>(
          scalar(context_->connection(),
                 "SELECT count(*) FROM outbox_message WHERE state = 'dead'")),
      .job_retries = static_cast<std::size_t>(scalar(
          context_->connection(), "SELECT COALESCE(sum(max(attempt_count - 1, "
                                  "0)), 0) FROM scheduled_job")),
      .outbox_retries = static_cast<std::size_t>(scalar(
          context_->connection(), "SELECT COALESCE(sum(max(attempt_count - 1, "
                                  "0)), 0) FROM outbox_message")),
      .scheduler_lag_ms = 0,
      .outbox_lag_ms = 0,
      .last_job_error = latest_error(context_->connection(), "scheduled_job"),
      .last_outbox_error =
          latest_error(context_->connection(), "outbox_message"),
  };
  auto job_due = context_->connection().prepare(
      "SELECT min(due_at_ms) FROM scheduled_job WHERE "
      "(state = 'pending' AND due_at_ms <= ?) OR "
      "(state = 'claimed' AND lease_until_ms <= ?)");
  job_due.bind(1, now_ms);
  job_due.bind(2, now_ms);
  if (job_due.step() && !job_due.column_is_null(0)) {
    result.scheduler_lag_ms =
        std::max<std::int64_t>(0, now_ms - job_due.column_int64(0));
  }
  auto outbox_due = context_->connection().prepare(
      "SELECT min(available_at_ms) FROM outbox_message WHERE state = 'pending' "
      "AND available_at_ms <= ?");
  outbox_due.bind(1, now_ms);
  if (outbox_due.step() && !outbox_due.column_is_null(0)) {
    result.outbox_lag_ms =
        std::max<std::int64_t>(0, now_ms - outbox_due.column_int64(0));
  }
  return result;
}

std::vector<WorkInspectionEntry>
SqliteDurableWorkRepository::recent(const std::size_t limit) {
  const std::scoped_lock lock{context_->mutex()};
  std::vector<WorkInspectionEntry> entries;
  const auto bounded = std::min<std::size_t>(limit, 50);
  auto events = context_->connection().prepare(
      "SELECT event_type, event_id, recorded_at_ms FROM event_journal "
      "ORDER BY recorded_at_ms DESC, event_id DESC LIMIT ?");
  events.bind(1, static_cast<std::int64_t>(bounded));
  while (events.step()) {
    entries.push_back({"event", events.column_text(0), "recorded",
                       shortened_persistent_id(events.column_text(1)), 0,
                       events.column_int64(2), std::nullopt});
  }
  auto jobs = context_->connection().prepare(
      "SELECT job_type, state, job_id, attempt_count, updated_at_ms, "
      "last_error_code FROM scheduled_job ORDER BY updated_at_ms DESC, "
      "job_id DESC LIMIT ?");
  jobs.bind(1, static_cast<std::int64_t>(bounded));
  while (jobs.step()) {
    entries.push_back({"job", jobs.column_text(0), jobs.column_text(1),
                       shortened_persistent_id(jobs.column_text(2)),
                       static_cast<std::size_t>(jobs.column_int64(3)),
                       jobs.column_int64(4), optional_text(jobs, 5)});
  }
  auto outbox = context_->connection().prepare(
      "SELECT kind, state, outbox_id, attempt_count, updated_at_ms, "
      "last_error_code FROM outbox_message ORDER BY updated_at_ms DESC, "
      "outbox_id DESC LIMIT ?");
  outbox.bind(1, static_cast<std::int64_t>(bounded));
  while (outbox.step()) {
    entries.push_back({"outbox", outbox.column_text(0), outbox.column_text(1),
                       shortened_persistent_id(outbox.column_text(2)),
                       static_cast<std::size_t>(outbox.column_int64(3)),
                       outbox.column_int64(4), optional_text(outbox, 5)});
  }
  std::ranges::sort(entries, std::greater{}, &WorkInspectionEntry::at_ms);
  if (entries.size() > bounded) {
    entries.resize(bounded);
  }
  return entries;
}

std::vector<WorkInspectionEntry>
SqliteDurableWorkRepository::dead(const std::size_t limit) {
  const std::scoped_lock lock{context_->mutex()};
  std::vector<WorkInspectionEntry> entries;
  const auto bounded = std::min<std::size_t>(limit, 50);
  auto jobs = context_->connection().prepare(
      "SELECT job_type, state, job_id, attempt_count, updated_at_ms, "
      "last_error_code FROM scheduled_job WHERE state = 'dead' "
      "ORDER BY updated_at_ms DESC, job_id DESC LIMIT ?");
  jobs.bind(1, static_cast<std::int64_t>(bounded));
  while (jobs.step()) {
    entries.push_back({"job", jobs.column_text(0), jobs.column_text(1),
                       shortened_persistent_id(jobs.column_text(2)),
                       static_cast<std::size_t>(jobs.column_int64(3)),
                       jobs.column_int64(4), optional_text(jobs, 5)});
  }
  auto outbox = context_->connection().prepare(
      "SELECT kind, state, outbox_id, attempt_count, updated_at_ms, "
      "last_error_code FROM outbox_message WHERE state IN ('failed', 'dead') "
      "ORDER BY updated_at_ms DESC, outbox_id DESC LIMIT ?");
  outbox.bind(1, static_cast<std::int64_t>(bounded));
  while (outbox.step()) {
    entries.push_back({"outbox", outbox.column_text(0), outbox.column_text(1),
                       shortened_persistent_id(outbox.column_text(2)),
                       static_cast<std::size_t>(outbox.column_int64(3)),
                       outbox.column_int64(4), optional_text(outbox, 5)});
  }
  std::ranges::sort(entries, std::greater{}, &WorkInspectionEntry::at_ms);
  if (entries.size() > bounded) {
    entries.resize(bounded);
  }
  return entries;
}

} // namespace sanguinius::persistence
