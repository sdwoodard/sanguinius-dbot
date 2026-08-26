#include "sanguinius/persistence/sqlite_vox_narration_repository.hpp"

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/tts.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t rolling_day_ms = 24 * 60 * 60 * 1'000;
constexpr std::int64_t noncritical_speech_capacity = 16;

struct SourceEvent {
  std::int64_t rowid{};
  std::string event_id;
  std::string event_type;
  std::string aggregate_id;
  std::string guild_id;
  std::string channel_id;
  std::int64_t recorded_at_ms{};
  std::string payload_json;
};

struct Projection {
  bool eligible{};
  std::string reason{"projection_missing"};
  std::string safe_input;
  std::optional<std::string> counterpart_outbox_id;
  bool is_test{};
};

void bind_optional(SqliteStatement &statement, const std::size_t index,
                   const std::optional<std::string> &value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.bind_null(index);
}

[[nodiscard]] std::optional<std::string>
optional_text(SqliteStatement &statement, const int column) {
  return statement.column_is_null(column)
             ? std::nullopt
             : std::optional<std::string>{statement.column_text(column)};
}

void validate_control_context(const VoxNarrationControlContext &context) {
  const auto invalid_fingerprint =
      context.request_fingerprint.empty() ||
      context.request_fingerprint.size() > 128 ||
      std::ranges::any_of(context.request_fingerprint, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x21U || byte > 0x7eU;
      });
  if (context.idempotency_key.empty() || context.idempotency_key.size() > 160 ||
      (context.operation != "narration_preview" &&
       context.operation != "narration_enqueue") ||
      context.actor_user_id.empty() || context.actor_user_id.size() > 20 ||
      context.guild_id.empty() || context.guild_id.size() > 20 ||
      context.channel_id.empty() || context.channel_id.size() > 20 ||
      invalid_fingerprint || context.now_ms < 0)
    throw std::invalid_argument{"Narration control receipt is invalid."};
}

void validate_enqueue_request(const VoxNarrationEnqueueRequest &request) {
  if (request.source_event_id.empty() || request.now_ms < 0 || !request.next_id)
    throw std::invalid_argument{"Narration enqueue request is invalid."};
}

[[nodiscard]] std::optional<std::string>
load_control_receipt(SqliteConnection &connection,
                     const VoxNarrationControlContext &context) {
  auto query = connection.prepare(
      "SELECT operation,actor_user_id,guild_id,channel_id,request_json,"
      "result_json FROM voice_interaction_receipt WHERE idempotency_key=?");
  query.bind(1, context.idempotency_key);
  if (!query.step())
    return std::nullopt;
  if (query.column_text(0) != context.operation ||
      query.column_text(1) != context.actor_user_id ||
      query.column_text(2) != context.guild_id ||
      query.column_text(3) != context.channel_id)
    throw std::invalid_argument{
        "Narration interaction idempotency key was reused."};
  try {
    const auto request = Json::parse(query.column_text(4));
    const auto result = Json::parse(query.column_text(5));
    if (request.value("operation", std::string{}) != context.operation ||
        request.value("request_fingerprint", std::string{}) !=
            context.request_fingerprint ||
        !result.contains("message") || !result.at("message").is_string())
      throw std::invalid_argument{
          "Narration interaction idempotency key was reused."};
    return result.at("message").get<std::string>();
  } catch (const std::invalid_argument &) {
    throw;
  } catch (const std::exception &) {
    throw std::invalid_argument{
        "Narration interaction idempotency key was reused."};
  }
}

[[nodiscard]] std::string
store_control_receipt_uncommitted(SqliteConnection &connection,
                                  const VoxNarrationControlContext &context,
                                  std::string message) {
  if (message.empty() || message.size() > 1'900)
    throw std::invalid_argument{
        "Narration control receipt response is invalid."};
  if (const auto existing = load_control_receipt(connection, context))
    return *existing;
  const Json request{{"operation", context.operation},
                     {"request_fingerprint", context.request_fingerprint}};
  const Json result{{"message", message}};
  auto insert = connection.prepare(
      "INSERT INTO voice_interaction_receipt(idempotency_key,operation,"
      "actor_user_id,guild_id,channel_id,request_json,result_json,session_id,"
      "created_at_ms) VALUES(?,?,?,?,?,?,?,NULL,?)");
  insert.bind(1, context.idempotency_key);
  insert.bind(2, context.operation);
  insert.bind(3, context.actor_user_id);
  insert.bind(4, context.guild_id);
  insert.bind(5, context.channel_id);
  insert.bind(6, request.dump());
  insert.bind(7, result.dump());
  insert.bind(8, context.now_ms);
  insert.execute();
  return message;
}

[[nodiscard]] VoxNarrationFeature feature_from(const std::string_view value) {
  if (value == "chronicle")
    return VoxNarrationFeature::chronicle;
  if (value == "tarot")
    return VoxNarrationFeature::tarot;
  if (value == "appearance")
    return VoxNarrationFeature::appearance;
  if (value == "session")
    return VoxNarrationFeature::session;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored narration feature is invalid."};
}

[[nodiscard]] SourceEvent event_from(SqliteStatement &query) {
  return {.rowid = query.column_int64(0),
          .event_id = query.column_text(1),
          .event_type = query.column_text(2),
          .aggregate_id = query.column_text(3),
          .guild_id = query.column_text(4),
          .channel_id =
              query.column_is_null(5) ? std::string{} : query.column_text(5),
          .recorded_at_ms = query.column_int64(6),
          .payload_json = query.column_text(7)};
}

[[nodiscard]] std::optional<SourceEvent>
find_event(SqliteConnection &connection, const std::string_view event_id) {
  auto query = connection.prepare(
      "SELECT rowid,event_id,event_type,aggregate_id,guild_id,channel_id,"
      "recorded_at_ms,payload_json FROM event_journal WHERE event_id=?");
  query.bind(1, event_id);
  if (!query.step())
    return std::nullopt;
  return event_from(query);
}

[[nodiscard]] bool primary_scope(SqliteConnection &connection,
                                 const SourceEvent &event) {
  auto query = connection.prepare(
      "SELECT 1 FROM guild_config WHERE guild_id=? AND primary_channel_id=?");
  query.bind(1, event.guild_id);
  query.bind(2, event.channel_id);
  return query.step();
}

struct PublicOutboxLookup {
  std::optional<std::string> outbox_id;
  bool ambiguous{};
};

[[nodiscard]] PublicOutboxLookup
generic_public_outbox(SqliteConnection &connection, const SourceEvent &event) {
  auto query = connection.prepare(
      "SELECT outbox_id FROM outbox_message WHERE target_guild_id=? AND "
      "target_channel_id=? AND target_user_id IS NULL AND "
      "aggregate_id=? AND json_extract(payload_json,'$.causation_event_id')=? "
      "ORDER BY created_at_ms DESC,outbox_id DESC LIMIT 2");
  query.bind(1, event.guild_id);
  query.bind(2, event.channel_id);
  query.bind(3, event.aggregate_id);
  query.bind(4, event.event_id);
  if (!query.step())
    return {};
  const auto outbox_id = query.column_text(0);
  if (query.step())
    return {.outbox_id = std::nullopt, .ambiguous = true};
  return {.outbox_id = outbox_id, .ambiguous = false};
}

[[nodiscard]] Projection project(SqliteConnection &connection,
                                 const SourceEvent &event) {
  Projection result;
  if (!primary_scope(connection, event)) {
    result.reason = "scope_rejected";
    return result;
  }
  if (event.event_type == "chronicle.title_awarded.v1") {
    auto query = connection.prepare(
        "SELECT definition.title,COALESCE(user.display_name_cache,"
        "user.username_cache,'a member of the company'),grant.state "
        "FROM chronicle_title_grant grant JOIN chronicle_title_definition "
        "definition ON definition.definition_id=grant.definition_id JOIN "
        "discord_user user ON user.user_id=grant.recipient_user_id WHERE "
        "grant.grant_id=?");
    query.bind(1, event.aggregate_id);
    if (!query.step() || query.column_text(2) != "active") {
      result.reason = "title_not_public_active";
      return result;
    }
    result.safe_input = "Public active title: " + query.column_text(0) +
                        ". Recipient display name: " + query.column_text(1) +
                        ".";
    const auto counterpart = generic_public_outbox(connection, event);
    if (counterpart.ambiguous) {
      result.reason = "counterpart_ambiguous";
      return result;
    }
    result.counterpart_outbox_id = counterpart.outbox_id;
  } else if (event.event_type == "tarot.draw_created.v1") {
    auto query = connection.prepare(
        "SELECT card.name,card.theme_tag,draw.visibility,draw.is_test "
        "FROM tarot_card_draw draw JOIN tarot_card_definition card ON "
        "card.catalog_version=draw.catalog_version AND "
        "card.ordinal=draw.card_ordinal WHERE draw.draw_id=?");
    query.bind(1, event.aggregate_id);
    if (!query.step() || query.column_text(2) != "public") {
      result.reason = "draw_not_public";
      return result;
    }
    result.is_test = query.column_int64(3) != 0;
    result.safe_input = "Public card: " + query.column_text(0) +
                        ". Public theme: " + query.column_text(1) + ".";
    auto delivery = connection.prepare(
        "SELECT outbox_id FROM tarot_draw_public_delivery WHERE draw_id=?");
    delivery.bind(1, event.aggregate_id);
    if (delivery.step())
      result.counterpart_outbox_id = delivery.column_text(0);
  } else if (event.event_type == "appearance.live_queued.v1") {
    auto query = connection.prepare(
        "SELECT "
        "json_extract(outbox.payload_json,'$.content'),reservation.outbox_id,"
        "reservation.is_test,decision.action FROM appearance_decision decision "
        "JOIN appearance_budget_reservation reservation ON "
        "reservation.decision_id=decision.decision_id JOIN outbox_message "
        "outbox "
        "ON outbox.outbox_id=reservation.outbox_id WHERE "
        "decision.decision_id=?");
    query.bind(1, event.aggregate_id);
    if (!query.step() || query.column_text(3) != "live_queued" ||
        query.column_is_null(0)) {
      result.reason = "appearance_not_public";
      return result;
    }
    result.safe_input = query.column_text(0);
    result.counterpart_outbox_id = query.column_text(1);
    result.is_test = query.column_int64(2) != 0;
  } else if (event.event_type == "tarot.wager_funded.v1" ||
             event.event_type == "tarot.wager_resolved.v1" ||
             event.event_type == "tarot.wager_voided.v1") {
    auto query = connection.prepare(
        "SELECT visibility,is_test FROM tarot_wager WHERE wager_id=?");
    query.bind(1, event.aggregate_id);
    if (!query.step() || query.column_text(0) != "public") {
      result.reason = "wager_not_public";
      return result;
    }
    result.is_test = query.column_int64(1) != 0;
    result.safe_input = event.event_type == "tarot.wager_funded.v1"
                            ? "A public challenge was accepted."
                            : "A public challenge reached its result.";
    const auto counterpart = generic_public_outbox(connection, event);
    if (counterpart.ambiguous) {
      result.reason = "counterpart_ambiguous";
      return result;
    }
    result.counterpart_outbox_id = counterpart.outbox_id;
  } else if (event.event_type == "tarot.house_funded.v1" ||
             event.event_type == "tarot.house_resolved.v1" ||
             event.event_type == "tarot.house_voided.v1") {
    auto query = connection.prepare(
        "SELECT visibility,recovery,is_test FROM tarot_house_wager WHERE "
        "wager_id=?");
    query.bind(1, event.aggregate_id);
    if (!query.step() || query.column_text(0) != "public" ||
        query.column_int64(1) != 0) {
      result.reason = "house_wager_not_public";
      return result;
    }
    result.is_test = query.column_int64(2) != 0;
    result.safe_input = event.event_type == "tarot.house_funded.v1"
                            ? "A public House challenge was accepted."
                            : "A public House challenge reached its result.";
    const auto counterpart = generic_public_outbox(connection, event);
    if (counterpart.ambiguous) {
      result.reason = "counterpart_ambiguous";
      return result;
    }
    result.counterpart_outbox_id = counterpart.outbox_id;
  } else if (event.event_type == "chronicle.session_started.v1" ||
             event.event_type == "chronicle.session_completed.v1") {
    auto query = connection.prepare(
        "SELECT state FROM chronicle_session WHERE session_id=? AND guild_id=? "
        "AND channel_id=?");
    query.bind(1, event.aggregate_id);
    query.bind(2, event.guild_id);
    query.bind(3, event.channel_id);
    if (!query.step() ||
        (event.event_type == "chronicle.session_started.v1" &&
         query.column_text(0) != "open") ||
        (event.event_type == "chronicle.session_completed.v1" &&
         query.column_text(0) != "closed")) {
      result.reason = "chronicle_session_not_public";
      return result;
    }
    result.safe_input = event.event_type == "chronicle.session_started.v1"
                            ? "The shared Chronicle session is open."
                            : "The shared Chronicle session is closed.";
  } else {
    result.reason = "event_not_mapped";
    return result;
  }
  if (result.safe_input.empty()) {
    result.reason = "safe_projection_missing";
    return result;
  }
  result.eligible = true;
  result.reason = "eligible";
  return result;
}

[[nodiscard]] std::optional<std::string>
active_session(SqliteConnection &connection, const SourceEvent &event,
               std::string &state) {
  auto query = connection.prepare(
      "SELECT session_id,state FROM voice_session WHERE guild_id=? AND "
      "text_channel_id=? AND narration_event_rowid_floor<? AND state IN "
      "('connecting','ready','muted','reconnecting') ORDER BY started_at_ms "
      "DESC LIMIT 1");
  query.bind(1, event.guild_id);
  query.bind(2, event.channel_id);
  query.bind(3, event.rowid);
  if (!query.step())
    return std::nullopt;
  state = query.column_text(1);
  return query.column_text(0);
}

[[nodiscard]] bool quiet(SqliteConnection &connection,
                         const std::int64_t now_ms) {
  auto query = connection.prepare(
      "SELECT quiet_until_ms FROM appearance_control_state WHERE singleton=1");
  return query.step() && !query.column_is_null(0) &&
         query.column_int64(0) > now_ms;
}

struct NarrationUsage {
  std::int64_t rolling_day_micro_usd{};
  std::int64_t calendar_month_micro_usd{};
  std::size_t rolling_day_attempts{};
};

[[nodiscard]] std::int64_t month_start_utc_ms(const std::int64_t now_ms) {
  using namespace std::chrono;
  const auto now = sys_time<milliseconds>{milliseconds{now_ms}};
  const year_month_day date{floor<days>(now)};
  return duration_cast<milliseconds>(
             sys_days{date.year() / date.month() / day{1}}.time_since_epoch())
      .count();
}

[[nodiscard]] NarrationUsage narration_usage(SqliteConnection &connection,
                                             const std::int64_t now_ms) {
  const auto day_start = std::max<std::int64_t>(0, now_ms - rolling_day_ms);
  auto query = connection.prepare(
      "SELECT COALESCE(SUM(CASE WHEN submitted_at_ms>? THEN "
      "estimated_micro_usd ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>=? THEN estimated_micro_usd "
      "ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>? THEN 1 ELSE 0 END),0) "
      "FROM tts_usage_attempt");
  query.bind(1, day_start);
  query.bind(2, month_start_utc_ms(now_ms));
  query.bind(3, day_start);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Unable to summarize narration TTS usage."};
  return {.rolling_day_micro_usd = query.column_int64(0),
          .calendar_month_micro_usd = query.column_int64(1),
          .rolling_day_attempts =
              static_cast<std::size_t>(query.column_int64(2))};
}

[[nodiscard]] bool add_within(const std::int64_t current,
                              const std::int64_t addition,
                              const std::int64_t limit) noexcept {
  return current >= 0 && addition >= 0 && current < limit &&
         addition <= limit - current;
}

[[nodiscard]] bool narration_budget_available(
    SqliteConnection &connection, const std::int64_t now_ms,
    const TtsUsagePolicy &policy, const std::int64_t estimated_micro_usd = 0) {
  const auto usage = narration_usage(connection, now_ms);
  return usage.rolling_day_attempts < policy.rolling_day_attempts &&
         add_within(usage.rolling_day_micro_usd, estimated_micro_usd,
                    policy.rolling_day_micro_usd) &&
         add_within(usage.calendar_month_micro_usd, estimated_micro_usd,
                    policy.calendar_month_micro_usd);
}

[[nodiscard]] bool narration_queue_full(SqliteConnection &connection,
                                        const std::string_view session_id,
                                        const std::int64_t now_ms) {
  auto count = connection.prepare(
      "SELECT count(*) FROM speech_item WHERE voice_session_id=? AND state IN "
      "('pending','synthesizing','ready','playing') AND priority<>400 AND "
      "(expires_at_ms IS NULL OR expires_at_ms>?)");
  count.bind(1, session_id);
  count.bind(2, now_ms);
  static_cast<void>(count.step());
  return count.column_int64(0) >= noncritical_speech_capacity;
}

void insert_narration_transition(
    SqliteConnection &connection, const std::string_view transition_id,
    const std::string_view intent_id, const std::string_view from,
    const std::string_view to, const std::size_t from_version,
    const std::string_view reason, const std::int64_t occurred_at_ms) {
  auto insert = connection.prepare(
      "INSERT INTO voice_narration_transition(transition_id,intent_id,"
      "from_state,to_state,from_version,to_version,reason,idempotency_key,"
      "occurred_at_ms) VALUES(?,?,?,?,?,?,?,'narration:'||?||':'||?,?)");
  insert.bind(1, transition_id);
  insert.bind(2, intent_id);
  insert.bind(3, from);
  insert.bind(4, to);
  insert.bind(5, static_cast<std::int64_t>(from_version));
  insert.bind(6, static_cast<std::int64_t>(from_version + 1));
  insert.bind(7, reason);
  insert.bind(8, intent_id);
  insert.bind(9, static_cast<std::int64_t>(from_version + 1));
  insert.bind(10, occurred_at_ms);
  insert.execute();
}

[[nodiscard]] std::string session_card_payload(const SourceEvent &event,
                                               const std::string_view content) {
  return nlohmann::json{{"payload_version", 1},
                        {"guild_id", event.guild_id},
                        {"channel_id", event.channel_id},
                        {"content", content},
                        {"embed", nullptr},
                        {"buttons", nlohmann::json::array()},
                        {"allowed_user_mentions", nlohmann::json::array()},
                        {"fail_before_first_send", false},
                        {"correlation_id", "vox-narration:" + event.event_id},
                        {"causation_event_id", event.event_id}}
      .dump();
}

[[nodiscard]] std::string
create_session_counterpart(SqliteConnection &connection,
                           const SourceEvent &event,
                           const std::function<std::string()> &next_id) {
  const auto outbox_id = next_id();
  const auto content = event.event_type == "chronicle.session_started.v1"
                           ? "**The Chronicle session is open.**"
                           : "**The Chronicle session is closed.**";
  auto insert = connection.prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,target_user_id,payload_json,state,"
      "attempt_count,max_attempts,lease_owner,lease_token,lease_until_ms,"
      "idempotency_key,provider_nonce,created_at_ms,available_at_ms,"
      "updated_at_ms) VALUES("
      "?,'discord.public.v1','voice_narration',?,?,?,NULL,?,'pending',0,5,"
      "NULL,NULL,NULL,?,?,?,?,?)");
  insert.bind(1, outbox_id);
  insert.bind(2, event.aggregate_id);
  insert.bind(3, event.guild_id);
  insert.bind(4, event.channel_id);
  insert.bind(5, session_card_payload(event, content));
  insert.bind(6, "outbox:vox-narration:" + event.event_id);
  insert.bind(7, discord_nonce_from_uuid(outbox_id));
  insert.bind(8, event.recorded_at_ms);
  insert.bind(9, event.recorded_at_ms);
  insert.bind(10, event.recorded_at_ms);
  insert.execute();
  return outbox_id;
}

[[nodiscard]] bool insert_observation(
    SqliteConnection &connection, const SourceEvent &event,
    const VoxNarrationPolicy &policy, const Projection &projection,
    const std::optional<std::string> &session_id,
    const std::optional<std::string> &counterpart, const bool terminal,
    const std::string_view reason, const VoxNarrationObserveRequest &request) {
  const auto intent_id = request.next_id();
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO voice_narration_intent(intent_id,source_event_id,"
      "slot,feature,event_type,guild_id,channel_id,safe_input,fallback_line,"
      "narration_rank,created_at_ms,expires_at_ms,session_id,"
      "counterpart_outbox_id,counterpart_required,model_status,speech_id,"
      "is_test,state,state_version,terminal_reason,content_hash) VALUES("
      "?,?,'feature',?,?,?,?,?,?,?,?,?,?,?,?,'not_requested',NULL,?,?,1,?,"
      "NULL)");
  insert.bind(1, intent_id);
  insert.bind(2, event.event_id);
  insert.bind(3, vox_narration_feature_name(policy.feature));
  insert.bind(4, event.event_type);
  insert.bind(5, event.guild_id);
  insert.bind(6, event.channel_id);
  if (projection.safe_input.empty())
    insert.bind_null(7);
  else
    insert.bind(7, projection.safe_input);
  bind_optional(insert, 8, policy.fallback_line);
  insert.bind(9, static_cast<std::int64_t>(policy.rank));
  insert.bind(10, event.recorded_at_ms);
  insert.bind(11, event.recorded_at_ms + policy.ttl_ms);
  bind_optional(insert, 12, session_id);
  bind_optional(insert, 13, counterpart);
  insert.bind(14, policy.counterpart_required ? 1LL : 0LL);
  insert.bind(15, projection.is_test ? 1LL : 0LL);
  insert.bind(16, terminal ? "suppressed" : "pending");
  if (terminal)
    insert.bind(17, reason);
  else
    insert.bind_null(17);
  insert.execute();
  if (connection.changes() == 0)
    return false;
  insert_narration_transition(connection, request.next_id(), intent_id,
                              "pending", terminal ? "suppressed" : "pending", 0,
                              reason, request.now_ms);
  return true;
}

[[nodiscard]] VoxNarrationCandidate candidate_from(SqliteStatement &query) {
  return {.intent_id = query.column_text(0),
          .revision = static_cast<std::size_t>(query.column_int64(1)),
          .source_event_id = query.column_text(2),
          .event_type = query.column_text(3),
          .feature = feature_from(query.column_text(4)),
          .guild_id = query.column_text(5),
          .channel_id = query.column_text(6),
          .safe_input = query.column_text(7),
          .fallback_line = optional_text(query, 8),
          .rank = static_cast<std::uint8_t>(query.column_int64(9)),
          .created_at_ms = query.column_int64(10),
          .expires_at_ms = query.column_int64(11),
          .session_id =
              query.column_is_null(12) ? std::string{} : query.column_text(12),
          .counterpart_outbox_id = optional_text(query, 13),
          .counterpart_required = query.column_int64(14) != 0,
          .is_test = query.column_int64(15) != 0};
}

constexpr std::string_view candidate_columns{
    "intent.intent_id,intent.state_version,intent.source_event_id,"
    "intent.event_type,intent.feature,intent.guild_id,intent.channel_id,"
    "intent.safe_input,intent.fallback_line,intent.narration_rank,"
    "intent.created_at_ms,intent.expires_at_ms,intent.session_id,"
    "intent.counterpart_outbox_id,intent.counterpart_required,intent.is_test"};

[[nodiscard]] bool source_projection_is_current(
    SqliteConnection &connection, const VoxNarrationCandidate &candidate,
    std::string &reason, const bool revalidate_all_features = false) {
  if (!revalidate_all_features &&
      candidate.event_type != "chronicle.title_awarded.v1" &&
      candidate.event_type != "chronicle.session_started.v1" &&
      candidate.event_type != "chronicle.session_completed.v1")
    return true;
  const auto event = find_event(connection, candidate.source_event_id);
  if (!event) {
    reason = "source_ineligible";
    return false;
  }
  const auto projection = project(connection, *event);
  if (!projection.eligible) {
    reason = "source_ineligible";
    return false;
  }
  if (projection.safe_input != candidate.safe_input ||
      projection.is_test != candidate.is_test) {
    reason = "source_changed";
    return false;
  }
  if (candidate.event_type != "chronicle.session_started.v1" &&
      candidate.event_type != "chronicle.session_completed.v1" &&
      projection.counterpart_outbox_id != candidate.counterpart_outbox_id) {
    reason = "source_changed";
    return false;
  }
  return true;
}

void terminalize(SqliteConnection &connection, const std::string_view intent_id,
                 const std::size_t revision, const std::string_view from,
                 const std::string_view target, const std::string_view reason,
                 const std::int64_t now_ms, const std::string &transition_id) {
  auto update = connection.prepare(
      "UPDATE voice_narration_intent SET state=?,state_version=state_version+1,"
      "lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,terminal_reason=? "
      "WHERE intent_id=? AND state_version=?");
  update.bind(1, target);
  update.bind(2, reason);
  update.bind(3, intent_id);
  update.bind(4, static_cast<std::int64_t>(revision));
  update.execute();
  if (connection.changes() == 1)
    insert_narration_transition(connection, transition_id, intent_id, from,
                                target, revision, reason, now_ms);
}

[[nodiscard]] std::string uuid_sql() {
  return "lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||"
         "substr(lower(hex(randomblob(2))),2)||'-a'||"
         "substr(lower(hex(randomblob(2))),2)||'-'||lower(hex(randomblob(6)))";
}

void cancel_pending_speech(SqliteConnection &connection,
                           const std::string_view intent_id,
                           const std::string_view speech_id,
                           const std::int64_t now_ms,
                           const std::string_view speech_reason,
                           const std::string_view intent_reason) {
  auto speech = connection.prepare(
      "SELECT state,state_version FROM speech_item WHERE speech_id=? AND "
      "state IN ('pending','synthesizing','ready')");
  speech.bind(1, speech_id);
  if (!speech.step())
    return;
  const auto state = speech.column_text(0);
  const auto revision = speech.column_int64(1);
  auto update_speech = connection.prepare(
      "UPDATE speech_item SET state='cancelled',state_version=state_version+1,"
      "text=NULL,terminal_at_ms=?,last_error_code=? WHERE speech_id=? AND "
      "state_version=?");
  update_speech.bind(1, now_ms);
  update_speech.bind(2, speech_reason);
  update_speech.bind(3, speech_id);
  update_speech.bind(4, revision);
  update_speech.execute();
  auto audit = connection.prepare(
      "INSERT INTO speech_item_transition(transition_id,speech_id,from_state,"
      "to_state,from_version,to_version,reason,idempotency_key,occurred_at_ms) "
      "VALUES(" +
      uuid_sql() + ",?,?,'cancelled',?,?,?,?,?)");
  audit.bind(1, speech_id);
  audit.bind(2, state);
  audit.bind(3, revision);
  audit.bind(4, revision + 1);
  audit.bind(5, speech_reason);
  audit.bind(6, "speech:" + std::string{speech_reason} + ":" +
                    std::string{speech_id});
  audit.bind(7, now_ms);
  audit.execute();
  auto intent = connection.prepare("SELECT state,state_version FROM "
                                   "voice_narration_intent WHERE intent_id=?");
  intent.bind(1, intent_id);
  if (!intent.step())
    return;
  const auto intent_state = intent.column_text(0);
  const auto intent_revision = static_cast<std::size_t>(intent.column_int64(1));
  terminalize(connection, intent_id, intent_revision, intent_state, "cancelled",
              intent_reason, now_ms,
              std::string{"00000000-0000-4000-a000-"} +
                  std::string{intent_id}.substr(24));
}

[[nodiscard]] bool supersede_unfinished_intent(
    SqliteConnection &connection, const std::string_view intent_id,
    const std::string_view state, const std::size_t revision,
    const std::optional<std::string> &speech_id,
    const std::string_view speech_state, const std::int64_t now_ms) {
  if ((state == "generating" || state == "prepared") && !speech_id) {
    terminalize(connection, intent_id, revision, state, "cancelled",
                "superseded", now_ms,
                std::string{"10000000-0000-4000-a000-"} +
                    std::string{intent_id}.substr(24));
    return true;
  }
  if (state == "queued" && speech_id &&
      (speech_state == "pending" || speech_state == "synthesizing" ||
       speech_state == "ready")) {
    cancel_pending_speech(connection, intent_id, *speech_id, now_ms,
                          "narration_superseded", "superseded");
    return true;
  }
  return false;
}

std::size_t suppress_active_intents(
    SqliteConnection &connection, const std::string_view reason,
    const std::int64_t now_ms, const std::function<std::string()> &next_id) {
  auto query = connection.prepare(
      "SELECT intent.intent_id,intent.state,intent.state_version,"
      "intent.speech_id,COALESCE(speech.state,'') FROM "
      "voice_narration_intent intent LEFT JOIN speech_item speech ON "
      "speech.speech_id=intent.speech_id WHERE intent.state IN "
      "('pending','generating','prepared','queued') ORDER BY "
      "intent.created_at_ms LIMIT 32");
  struct ActiveIntent {
    std::string id;
    std::string state;
    std::size_t revision{};
    std::optional<std::string> speech_id;
    std::string speech_state;
  };
  std::vector<ActiveIntent> intents;
  while (query.step())
    intents.push_back(
        {.id = query.column_text(0),
         .state = query.column_text(1),
         .revision = static_cast<std::size_t>(query.column_int64(2)),
         .speech_id = optional_text(query, 3),
         .speech_state = query.column_text(4)});
  std::size_t changed{};
  for (const auto &intent : intents) {
    if (intent.state == "queued" && intent.speech_id &&
        (intent.speech_state == "pending" ||
         intent.speech_state == "synthesizing" ||
         intent.speech_state == "ready")) {
      cancel_pending_speech(connection, intent.id, *intent.speech_id, now_ms,
                            "narration_" + std::string{reason}, reason);
      ++changed;
    } else if (intent.state == "pending" || intent.state == "generating" ||
               intent.state == "prepared") {
      terminalize(connection, intent.id, intent.revision, intent.state,
                  "suppressed", reason, now_ms, next_id());
      ++changed;
    }
  }
  return changed;
}

[[nodiscard]] VoxNarrationEnqueueResult
enqueue_reference_uncommitted(SqliteConnection &connection,
                              const VoxNarrationEnqueueRequest &request) {
  const auto existing_result =
      [&]() -> std::optional<VoxNarrationEnqueueResult> {
    auto existing = connection.prepare(
        "SELECT state,terminal_reason FROM voice_narration_intent WHERE "
        "source_event_id=? AND slot='feature'");
    existing.bind(1, request.source_event_id);
    if (!existing.step())
      return std::nullopt;
    return VoxNarrationEnqueueResult{
        .status = VoxNarrationEnqueueStatus::replay,
        .reason = existing.column_is_null(1) ? "already_observed"
                                             : existing.column_text(1)};
  };
  if (const auto existing = existing_result())
    return *existing;
  const auto event = find_event(connection, request.source_event_id);
  if (!event)
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = "event_not_found"};
  const auto policy = vox_narration_policy(event->event_type);
  if (!policy)
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = "event_not_mapped"};
  auto projection = project(connection, *event);
  if (!projection.eligible)
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = projection.reason};
  if (!projection.is_test)
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = "test_event_required"};
  if (!request.test_mode)
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = "test_mode_disabled"};

  std::string session_state;
  const auto session_id = active_session(connection, *event, session_state);
  auto counterpart = projection.counterpart_outbox_id;
  std::string reason{"eligible"};
  bool terminal{};
  if (!request.enabled) {
    terminal = true;
    reason = "feature_disabled";
  } else if (event->recorded_at_ms + policy->ttl_ms <= request.now_ms) {
    terminal = true;
    reason = "stale";
  } else if (!session_id) {
    terminal = true;
    reason = "disconnected";
  } else if (session_state == "muted") {
    terminal = true;
    reason = "muted";
  } else if (quiet(connection, request.now_ms)) {
    terminal = true;
    reason = "quiet";
  } else if (policy->counterpart_required && !counterpart) {
    terminal = true;
    reason = "counterpart_missing";
  }
  const VoxNarrationObserveRequest observation{.now_ms = request.now_ms,
                                               .enabled = request.enabled,
                                               .test_mode = request.test_mode,
                                               .limit = 1,
                                               .next_id = request.next_id};
  const auto inserted =
      insert_observation(connection, *event, *policy, projection, session_id,
                         counterpart, terminal, reason, observation);
  if (!inserted)
    return existing_result().value_or(
        VoxNarrationEnqueueResult{.status = VoxNarrationEnqueueStatus::rejected,
                                  .reason = "deduplication_failed"});
  return {.status = terminal ? VoxNarrationEnqueueStatus::rejected
                             : VoxNarrationEnqueueStatus::accepted,
          .reason = reason};
}

} // namespace

SqliteVoxNarrationRepository::SqliteVoxNarrationRepository(
    std::shared_ptr<SqliteRepositoryContext> context,
    const TtsUsagePolicy usage_policy)
    : context_{std::move(context)}, usage_policy_{usage_policy} {
  if (!context_ || usage_policy_.rolling_day_attempts == 0 ||
      usage_policy_.rolling_day_micro_usd <= 0 ||
      usage_policy_.calendar_month_micro_usd <= 0)
    throw std::invalid_argument{
        "Narration repository context and TTS budget are required."};
}

std::size_t SqliteVoxNarrationRepository::observe_batch(
    const VoxNarrationObserveRequest &request) {
  if (request.now_ms < 0 || request.limit == 0 || request.limit > 32 ||
      !request.next_id)
    throw std::invalid_argument{"Narration observation request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (!request.enabled)
    static_cast<void>(suppress_active_intents(connection, "feature_disabled",
                                              request.now_ms, request.next_id));
  auto cursor = connection.prepare(
      "SELECT CAST(last_event_rowid AS INTEGER) FROM voice_narration_cursor "
      "WHERE singleton=1");
  if (!cursor.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Narration cursor is missing."};
  const auto last = cursor.column_int64(0);
  auto query = connection.prepare(
      "SELECT CAST(rowid AS "
      "INTEGER),event_id,event_type,aggregate_id,guild_id,channel_id,"
      "recorded_at_ms,payload_json FROM event_journal WHERE rowid>? ORDER BY "
      "rowid LIMIT ?");
  query.bind(1, last);
  query.bind(2, static_cast<std::int64_t>(request.limit));
  std::vector<SourceEvent> events;
  while (query.step())
    events.push_back(event_from(query));
  for (const auto &event : events) {
    const auto policy = vox_narration_policy(event.event_type);
    if (!policy)
      continue;
    auto projection = project(connection, event);
    std::string session_state;
    const auto session_id = active_session(connection, event, session_state);
    auto counterpart = projection.counterpart_outbox_id;
    if (request.enabled && projection.eligible &&
        event.recorded_at_ms + policy->ttl_ms > request.now_ms &&
        (event.event_type == "chronicle.session_started.v1" ||
         event.event_type == "chronicle.session_completed.v1"))
      counterpart =
          create_session_counterpart(connection, event, request.next_id);
    std::string reason = projection.reason;
    bool terminal = !projection.eligible;
    if (!request.enabled) {
      terminal = true;
      reason = "feature_disabled";
    } else if (!terminal && projection.is_test && !request.test_mode) {
      terminal = true;
      reason = "test_mode_disabled";
    } else if (!terminal &&
               event.recorded_at_ms + policy->ttl_ms <= request.now_ms) {
      terminal = true;
      reason = "stale";
    } else if (!terminal && !session_id) {
      terminal = true;
      reason = "disconnected";
    } else if (!terminal && session_state == "muted") {
      terminal = true;
      reason = "muted";
    } else if (!terminal && quiet(connection, request.now_ms)) {
      terminal = true;
      reason = "quiet";
    } else if (!terminal && policy->counterpart_required && !counterpart) {
      terminal = true;
      reason = "counterpart_missing";
    }
    static_cast<void>(insert_observation(connection, event, *policy, projection,
                                         session_id, counterpart, terminal,
                                         reason, request));
  }
  if (!events.empty()) {
    auto update = connection.prepare(
        "UPDATE voice_narration_cursor SET last_event_rowid=?,updated_at_ms=? "
        "WHERE singleton=1");
    update.bind(1, events.back().rowid);
    update.bind(2, request.now_ms);
    update.execute();
  }
  transaction.commit();
  return events.size();
}

VoxNarrationEnqueueResult SqliteVoxNarrationRepository::enqueue_reference(
    const VoxNarrationEnqueueRequest &request) {
  validate_enqueue_request(request);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto result = enqueue_reference_uncommitted(connection, request);
  transaction.commit();
  return result;
}

std::string SqliteVoxNarrationRepository::enqueue_reference_with_receipt(
    const VoxNarrationEnqueueRequest &request,
    const VoxNarrationControlContext &context) {
  validate_enqueue_request(request);
  validate_control_context(context);
  if (context.operation != "narration_enqueue" ||
      context.request_fingerprint != request.source_event_id)
    throw std::invalid_argument{
        "Narration enqueue receipt does not match its request."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto existing = load_control_receipt(connection, context)) {
    transaction.commit();
    return *existing;
  }
  const auto result = enqueue_reference_uncommitted(connection, request);
  auto message = store_control_receipt_uncommitted(
      connection, context, vox_narration_enqueue_response(result));
  transaction.commit();
  return message;
}

std::optional<VoxNarrationCandidate> SqliteVoxNarrationRepository::claim_next(
    const VoxNarrationClaimRequest &request) {
  if (request.now_ms < 0 || request.instance_id.empty() ||
      request.lease_token.empty() || request.transition_id.empty() ||
      request.lease_until_ms <= request.now_ms)
    throw std::invalid_argument{"Narration claim request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto expired = connection.prepare(
      "SELECT intent_id,state_version FROM voice_narration_intent WHERE "
      "state='pending' AND expires_at_ms<=?");
  expired.bind(1, request.now_ms);
  std::vector<std::pair<std::string, std::size_t>> stale;
  while (expired.step())
    stale.emplace_back(expired.column_text(0),
                       static_cast<std::size_t>(expired.column_int64(1)));
  for (const auto &[id, revision] : stale)
    terminalize(connection, id, revision, "pending", "expired", "stale",
                request.now_ms, id);

  auto query = connection.prepare(
      "SELECT " + std::string{candidate_columns} +
      ",COALESCE((SELECT MAX(mute_transition.to_version) FROM "
      "voice_session_transition mute_transition WHERE "
      "mute_transition.session_id=intent.session_id AND "
      "mute_transition.to_state='muted'),0)" +
      " FROM voice_narration_intent intent JOIN voice_session session ON "
      "session.session_id=intent.session_id LEFT JOIN outbox_message outbox ON "
      "outbox.outbox_id=intent.counterpart_outbox_id WHERE "
      "intent.state='pending' "
      "AND intent.expires_at_ms>? AND session.state='ready' AND "
      "(? OR intent.is_test=0) AND (intent.counterpart_required=0 OR "
      "(outbox.state='delivered' AND "
      "outbox.delivered_at_ms<=intent.expires_at_ms)) "
      "AND NOT EXISTS(SELECT 1 FROM appearance_control_state ctl WHERE "
      "ctl.singleton=1 AND ctl.quiet_until_ms>?) ORDER BY "
      "intent.narration_rank DESC,intent.created_at_ms,intent.intent_id LIMIT "
      "1");
  query.bind(1, request.now_ms);
  query.bind(2, request.test_mode ? 1LL : 0LL);
  query.bind(3, request.now_ms);
  if (!query.step()) {
    transaction.commit();
    return std::nullopt;
  }
  auto candidate = candidate_from(query);
  candidate.mute_epoch = static_cast<std::size_t>(query.column_int64(16));

  if (!narration_budget_available(connection, request.now_ms, usage_policy_)) {
    terminalize(connection, candidate.intent_id, candidate.revision, "pending",
                "suppressed", "provider_budget", request.now_ms,
                request.transition_id);
    transaction.commit();
    return std::nullopt;
  }

  auto same_feature = connection.prepare(
      "SELECT intent.intent_id,intent.state,intent.state_version,"
      "intent.narration_rank,intent.speech_id,COALESCE(speech.state,'') FROM "
      "voice_narration_intent intent LEFT JOIN speech_item speech ON "
      "speech.speech_id=intent.speech_id WHERE intent.session_id=? AND "
      "intent.feature=? AND intent.intent_id<>? AND (intent.state IN "
      "('generating','prepared','queued','played') OR (intent.state IN "
      "('failed','expired','cancelled') AND EXISTS(SELECT 1 FROM "
      "speech_item_transition history WHERE history.speech_id="
      "intent.speech_id AND history.to_state='playing'))) ORDER BY "
      "intent.narration_rank "
      "DESC LIMIT 1");
  same_feature.bind(1, candidate.session_id);
  same_feature.bind(2, vox_narration_feature_name(candidate.feature));
  same_feature.bind(3, candidate.intent_id);
  if (same_feature.step()) {
    const auto existing_rank = same_feature.column_int64(3);
    const auto existing_speech = optional_text(same_feature, 4);
    const auto speech_state = same_feature.column_text(5);
    if (existing_rank >= candidate.rank ||
        !supersede_unfinished_intent(
            connection, same_feature.column_text(0),
            same_feature.column_text(1),
            static_cast<std::size_t>(same_feature.column_int64(2)),
            existing_speech, speech_state, request.now_ms)) {
      terminalize(connection, candidate.intent_id, candidate.revision,
                  "pending", "suppressed", "feature_budget", request.now_ms,
                  request.transition_id);
      transaction.commit();
      return std::nullopt;
    }
  }
  auto total = connection.prepare(
      "SELECT count(*) FROM voice_narration_intent WHERE session_id=? AND "
      "(state IN ('generating','prepared','queued','played') OR (state IN "
      "('failed','expired','cancelled') AND EXISTS(SELECT 1 FROM "
      "speech_item_transition history WHERE history.speech_id="
      "voice_narration_intent.speech_id AND history.to_state='playing'))) AND "
      "slot='feature'");
  total.bind(1, candidate.session_id);
  static_cast<void>(total.step());
  if (total.column_int64(0) >= 2) {
    auto lower = connection.prepare(
        "SELECT intent.intent_id,intent.state,intent.state_version,"
        "intent.speech_id,COALESCE(speech.state,''),intent.narration_rank "
        "FROM voice_narration_intent intent LEFT JOIN "
        "speech_item speech ON speech.speech_id=intent.speech_id WHERE "
        "intent.session_id=? AND intent.slot='feature' AND "
        "(intent.state IN ('generating','prepared') OR "
        "(intent.state='queued' AND speech.state IN "
        "('pending','synthesizing','ready'))) "
        "ORDER BY intent.narration_rank,intent.created_at_ms LIMIT 1");
    lower.bind(1, candidate.session_id);
    const auto has_lower = lower.step();
    const auto lower_speech =
        has_lower ? optional_text(lower, 3) : std::optional<std::string>{};
    if (!has_lower || lower.column_int64(5) >= candidate.rank ||
        !supersede_unfinished_intent(
            connection, lower.column_text(0), lower.column_text(1),
            static_cast<std::size_t>(lower.column_int64(2)), lower_speech,
            lower.column_text(4), request.now_ms)) {
      terminalize(connection, candidate.intent_id, candidate.revision,
                  "pending", "suppressed", "session_budget", request.now_ms,
                  request.transition_id);
      transaction.commit();
      return std::nullopt;
    }
  }
  if (narration_queue_full(connection, candidate.session_id, request.now_ms)) {
    terminalize(connection, candidate.intent_id, candidate.revision, "pending",
                "suppressed", "queue_full", request.now_ms,
                request.transition_id);
    transaction.commit();
    return std::nullopt;
  }
  auto update = connection.prepare(
      "UPDATE voice_narration_intent SET state='generating',"
      "state_version=state_version+1,lease_owner=?,lease_token=?,lease_until_"
      "ms=?,"
      "model_status='generating' WHERE intent_id=? AND state='pending' AND "
      "state_version=?");
  update.bind(1, request.instance_id);
  update.bind(2, request.lease_token);
  update.bind(3, request.lease_until_ms);
  update.bind(4, candidate.intent_id);
  update.bind(5, static_cast<std::int64_t>(candidate.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::busy, SQLITE_BUSY, SQLITE_BUSY,
                        "Narration claim lost its revision fence."};
  insert_narration_transition(
      connection, request.transition_id, candidate.intent_id, "pending",
      "generating", candidate.revision, "generation_claimed", request.now_ms);
  ++candidate.revision;
  transaction.commit();
  return candidate;
}

std::optional<VoxNarrationCandidate>
SqliteVoxNarrationRepository::begin_generation(
    const VoxNarrationGenerationStartRequest &request) {
  if (request.intent_id.empty() || request.expected_revision == 0 ||
      request.instance_id.empty() || request.expected_lease_token.empty() ||
      request.lease_token.empty() || request.transition_id.empty() ||
      request.now_ms < 0 || request.lease_until_ms <= request.now_ms)
    throw std::invalid_argument{
        "Narration generation-start request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT " + std::string{candidate_columns} +
      ",intent.lease_owner,intent.lease_token,intent.lease_until_ms FROM "
      "voice_narration_intent intent WHERE intent.intent_id=? AND "
      "intent.state='generating'");
  query.bind(1, request.intent_id);
  if (!query.step() ||
      static_cast<std::size_t>(query.column_int64(1)) !=
          request.expected_revision ||
      query.column_text(16) != request.instance_id ||
      query.column_text(17) != request.expected_lease_token) {
    transaction.commit();
    return std::nullopt;
  }
  auto candidate = candidate_from(query);
  const auto claimed_lease_until_ms = query.column_int64(18);
  std::string reason;
  std::string session_state;
  std::size_t current_mute_epoch{};
  if (candidate.expires_at_ms <= request.now_ms) {
    reason = "stale";
  } else if (candidate.is_test && !request.test_mode) {
    reason = "test_mode_disabled";
  } else if (source_projection_is_current(connection, candidate, reason,
                                          true)) {
    auto session = connection.prepare(
        "SELECT session.state,COALESCE((SELECT MAX(mute_transition.to_version) "
        "FROM voice_session_transition mute_transition WHERE "
        "mute_transition.session_id=session.session_id AND "
        "mute_transition.to_state='muted'),0) FROM voice_session session WHERE "
        "session.session_id=?");
    session.bind(1, candidate.session_id);
    if (!session.step()) {
      reason = "session_not_ready";
    } else {
      session_state = session.column_text(0);
      current_mute_epoch = static_cast<std::size_t>(session.column_int64(1));
      if (current_mute_epoch != request.expected_mute_epoch)
        reason = "muted";
      else if (session_state != "ready" && session_state != "reconnecting")
        reason = session_state == "muted" ? "muted" : "session_not_ready";
      else if (session_state == "ready" && quiet(connection, request.now_ms))
        reason = "quiet";
      else if (session_state == "ready" && candidate.counterpart_required) {
        auto counterpart = connection.prepare(
            "SELECT state,delivered_at_ms FROM outbox_message WHERE "
            "outbox_id=?");
        if (candidate.counterpart_outbox_id)
          counterpart.bind(1, *candidate.counterpart_outbox_id);
        else
          counterpart.bind_null(1);
        if (!counterpart.step() || counterpart.column_text(0) != "delivered" ||
            counterpart.column_is_null(1) ||
            counterpart.column_int64(1) > candidate.expires_at_ms)
          reason = "counterpart_not_delivered";
      }
    }
  }

  if (reason.empty() && session_state == "ready" &&
      !narration_budget_available(connection, request.now_ms, usage_policy_))
    reason = "provider_budget";
  if (reason.empty() && session_state == "ready" &&
      narration_queue_full(connection, candidate.session_id, request.now_ms))
    reason = "queue_full";

  if (reason.empty() && session_state == "reconnecting") {
    auto release = connection.prepare(
        "UPDATE voice_narration_intent SET state='pending',"
        "state_version=state_version+1,lease_owner=NULL,lease_token=NULL,"
        "lease_until_ms=NULL,model_status='not_requested' WHERE intent_id=? "
        "AND state='generating' AND state_version=? AND lease_owner=? AND "
        "lease_token=?");
    release.bind(1, candidate.intent_id);
    release.bind(2, static_cast<std::int64_t>(candidate.revision));
    release.bind(3, request.instance_id);
    release.bind(4, request.expected_lease_token);
    release.execute();
    if (connection.changes() == 1)
      insert_narration_transition(
          connection, request.transition_id, candidate.intent_id, "generating",
          "pending", candidate.revision, "generation_deferred_reconnecting",
          request.now_ms);
    transaction.commit();
    return std::nullopt;
  }
  if (!reason.empty()) {
    terminalize(connection, candidate.intent_id, candidate.revision,
                "generating", reason == "stale" ? "expired" : "suppressed",
                reason, request.now_ms, request.transition_id);
    transaction.commit();
    return std::nullopt;
  }

  auto start = connection.prepare(
      "UPDATE voice_narration_intent SET state_version=state_version+1,"
      "lease_owner=?,lease_token=?,lease_until_ms=? WHERE intent_id=? AND "
      "state='generating' AND state_version=? AND lease_owner=? AND "
      "lease_token=?");
  start.bind(1, request.instance_id);
  start.bind(2, request.lease_token);
  start.bind(3, std::max(request.lease_until_ms, claimed_lease_until_ms));
  start.bind(4, candidate.intent_id);
  start.bind(5, static_cast<std::int64_t>(candidate.revision));
  start.bind(6, request.instance_id);
  start.bind(7, request.expected_lease_token);
  start.execute();
  if (connection.changes() != 1) {
    transaction.commit();
    return std::nullopt;
  }
  insert_narration_transition(
      connection, request.transition_id, candidate.intent_id, "generating",
      "generating", candidate.revision, "generation_started", request.now_ms);
  ++candidate.revision;
  candidate.mute_epoch = current_mute_epoch;
  transaction.commit();
  return candidate;
}

void SqliteVoxNarrationRepository::complete_generation(
    const VoxNarrationCompletion &request) {
  if (request.intent_id.empty() || request.expected_revision == 0 ||
      request.now_ms < 0 || request.transition_id.empty())
    throw std::invalid_argument{"Narration completion is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT " + std::string{candidate_columns} +
      " FROM voice_narration_intent intent WHERE intent.intent_id=? AND "
      "intent.state='generating'");
  query.bind(1, request.intent_id);
  if (!query.step() || static_cast<std::size_t>(query.column_int64(1)) !=
                           request.expected_revision) {
    transaction.commit();
    return;
  }
  const auto candidate = candidate_from(query);
  std::string reason;
  auto generation_started = connection.prepare(
      "SELECT 1 FROM voice_narration_transition WHERE intent_id=? AND "
      "reason='generation_started' LIMIT 1");
  generation_started.bind(1, candidate.intent_id);
  const auto revalidate_all_features = generation_started.step();
  auto session = connection.prepare(
      "SELECT session.state,COALESCE((SELECT MAX(mute_transition.to_version) "
      "FROM voice_session_transition mute_transition WHERE "
      "mute_transition.session_id=session.session_id AND "
      "mute_transition.to_state='muted'),0) FROM voice_session session WHERE "
      "session.session_id=?");
  session.bind(1, candidate.session_id);
  if (source_projection_is_current(connection, candidate, reason,
                                   revalidate_all_features)) {
    if (candidate.expires_at_ms <= request.now_ms)
      reason = "stale";
    else if (!session.step())
      reason = "session_not_ready";
    else if (static_cast<std::size_t>(session.column_int64(1)) !=
             request.expected_mute_epoch)
      reason = "muted";
    else if (session.column_text(0) != "ready" &&
             session.column_text(0) != "reconnecting")
      reason = "session_not_ready";
    else if (quiet(connection, request.now_ms))
      reason = "quiet";
    else if (candidate.counterpart_required) {
      auto counterpart = connection.prepare(
          "SELECT state,delivered_at_ms FROM outbox_message WHERE outbox_id=?");
      counterpart.bind(1, *candidate.counterpart_outbox_id);
      if (!counterpart.step() || counterpart.column_text(0) != "delivered" ||
          counterpart.column_is_null(1) ||
          counterpart.column_int64(1) > candidate.expires_at_ms)
        reason = "counterpart_not_delivered";
    }
  }
  if (reason.empty() && !request.line)
    reason = request.model_status == VoxNarrationModelStatus::duplicate
                 ? "appearance_duplicate"
                 : "generation_failed";
  if (!reason.empty()) {
    auto status = connection.prepare(
        "UPDATE voice_narration_intent SET model_status=?,"
        "state='suppressed',state_version=state_version+1,lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_reason=? WHERE "
        "intent_id=? AND state_version=?");
    status.bind(1, vox_narration_model_status_name(request.model_status));
    status.bind(2, reason);
    status.bind(3, candidate.intent_id);
    status.bind(4, static_cast<std::int64_t>(candidate.revision));
    status.execute();
    insert_narration_transition(connection, request.transition_id,
                                candidate.intent_id, "generating", "suppressed",
                                candidate.revision, reason, request.now_ms);
    transaction.commit();
    return;
  }
  const auto normalized = normalize_tts_text(*request.line);
  const auto normalized_bytes =
      std::as_bytes(std::span{normalized.text.data(), normalized.text.size()});
  if (normalized.scalar_count > 160 ||
      request.content_hash != sha256_hex(normalized_bytes) ||
      request.speech_id.empty())
    throw std::invalid_argument{"Generated narration line is invalid."};
  if (!narration_budget_available(
          connection, request.now_ms, usage_policy_,
          estimated_tts_cost_micro_usd(normalized.scalar_count)))
    reason = "provider_budget";
  else if (narration_queue_full(connection, candidate.session_id,
                                request.now_ms))
    reason = "queue_full";
  if (!reason.empty()) {
    auto status = connection.prepare(
        "UPDATE voice_narration_intent SET model_status=?,"
        "state='suppressed',state_version=state_version+1,lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_reason=? WHERE "
        "intent_id=? AND state_version=?");
    status.bind(1, vox_narration_model_status_name(request.model_status));
    status.bind(2, reason);
    status.bind(3, candidate.intent_id);
    status.bind(4, static_cast<std::int64_t>(candidate.revision));
    status.execute();
    insert_narration_transition(connection, request.transition_id,
                                candidate.intent_id, "generating", "suppressed",
                                candidate.revision, reason, request.now_ms);
    transaction.commit();
    return;
  }
  auto speech = connection.prepare(
      "INSERT INTO speech_item(speech_id,voice_session_id,source_event_id,"
      "source_kind,text,text_hash,scalar_count,provider,model,voice_id,"
      "priority,"
      "narration_rank,state,state_version,earliest_at_ms,expires_at_ms,"
      "interruptible,deduplication_key,created_at_ms) VALUES(?,?,?,"
      "'vox_feature_narration',?,?,?,'openai','tts-1','onyx',200,?,"
      "'pending',1,?,?,1,?,?)");
  speech.bind(1, request.speech_id);
  speech.bind(2, candidate.session_id);
  speech.bind(3, candidate.source_event_id);
  speech.bind(4, normalized.text);
  speech.bind(5, request.content_hash);
  speech.bind(6, static_cast<std::int64_t>(normalized.scalar_count));
  speech.bind(7, static_cast<std::int64_t>(candidate.rank));
  speech.bind(8, request.now_ms);
  speech.bind(9, candidate.expires_at_ms);
  speech.bind(10, "speech:narration:" + candidate.source_event_id + ":feature");
  speech.bind(11, request.now_ms);
  speech.execute();
  auto update = connection.prepare(
      "UPDATE voice_narration_intent SET model_status=?,state='queued',"
      "state_version=state_version+1,lease_owner=NULL,lease_token=NULL,"
      "lease_until_ms=NULL,speech_id=?,content_hash=? WHERE intent_id=? AND "
      "state_version=?");
  update.bind(1, vox_narration_model_status_name(request.model_status));
  update.bind(2, request.speech_id);
  update.bind(3, request.content_hash);
  update.bind(4, candidate.intent_id);
  update.bind(5, static_cast<std::int64_t>(candidate.revision));
  update.execute();
  insert_narration_transition(
      connection, request.transition_id, candidate.intent_id, "generating",
      "queued", candidate.revision, "speech_admitted", request.now_ms);
  transaction.commit();
}

std::size_t SqliteVoxNarrationRepository::reconcile(
    const std::int64_t now_ms, const std::function<std::string()> &next_id,
    const std::function<bool(std::string_view)> &generation_is_live) {
  if (now_ms < 0 || !next_id)
    throw std::invalid_argument{"Narration reconciliation is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  std::size_t changed{};
  if (quiet(connection, now_ms))
    changed += suppress_active_intents(connection, "quiet", now_ms, next_id);
  auto failed_counterparts = connection.prepare(
      "SELECT intent.intent_id,intent.state_version FROM "
      "voice_narration_intent intent JOIN outbox_message outbox ON "
      "outbox.outbox_id=intent.counterpart_outbox_id WHERE "
      "intent.state='pending' AND intent.counterpart_required=1 AND "
      "outbox.state IN ('failed','dead','cancelled') LIMIT 32");
  struct FailedCounterpart {
    std::string id;
    std::size_t revision{};
  };
  std::vector<FailedCounterpart> failed_counterpart_items;
  while (failed_counterparts.step())
    failed_counterpart_items.push_back(
        {.id = failed_counterparts.column_text(0),
         .revision =
             static_cast<std::size_t>(failed_counterparts.column_int64(1))});
  for (const auto &item : failed_counterpart_items) {
    terminalize(connection, item.id, item.revision, "pending", "suppressed",
                "counterpart_failed", now_ms, next_id());
    ++changed;
  }
  auto abandoned = connection.prepare(
      "SELECT intent.intent_id,intent.state_version,session.state FROM "
      "voice_narration_intent intent JOIN voice_session session ON "
      "session.session_id=intent.session_id WHERE intent.state='pending' AND "
      "session.state IN ('muted','leaving','inactive','failed') LIMIT 32");
  struct Abandoned {
    std::string id;
    std::size_t revision{};
    std::string session_state;
  };
  std::vector<Abandoned> abandoned_items;
  while (abandoned.step())
    abandoned_items.push_back(
        {.id = abandoned.column_text(0),
         .revision = static_cast<std::size_t>(abandoned.column_int64(1)),
         .session_state = abandoned.column_text(2)});
  for (const auto &item : abandoned_items) {
    terminalize(connection, item.id, item.revision, "pending", "suppressed",
                item.session_state == "muted" ? "muted" : "disconnected",
                now_ms, next_id());
    ++changed;
  }
  auto expired_leases = connection.prepare(
      "SELECT intent_id,state_version,expires_at_ms FROM "
      "voice_narration_intent WHERE state='generating' AND lease_until_ms<=? "
      "LIMIT 96");
  expired_leases.bind(1, now_ms);
  struct ExpiredLease {
    std::string id;
    std::size_t revision{};
    std::int64_t expires_at_ms{};
  };
  std::vector<ExpiredLease> leases;
  while (expired_leases.step())
    leases.push_back(
        {.id = expired_leases.column_text(0),
         .revision = static_cast<std::size_t>(expired_leases.column_int64(1)),
         .expires_at_ms = expired_leases.column_int64(2)});
  for (const auto &lease : leases) {
    if (generation_is_live && generation_is_live(lease.id))
      continue;
    if (changed >= 32)
      break;
    if (lease.expires_at_ms <= now_ms) {
      terminalize(connection, lease.id, lease.revision, "generating", "expired",
                  "stale", now_ms, next_id());
    } else {
      auto release = connection.prepare(
          "UPDATE voice_narration_intent SET state='pending',"
          "state_version=state_version+1,lease_owner=NULL,lease_token=NULL,"
          "lease_until_ms=NULL,model_status='not_requested' WHERE intent_id=? "
          "AND state='generating' AND state_version=?");
      release.bind(1, lease.id);
      release.bind(2, static_cast<std::int64_t>(lease.revision));
      release.execute();
      if (connection.changes() == 1)
        insert_narration_transition(connection, next_id(), lease.id,
                                    "generating", "pending", lease.revision,
                                    "lease_recovered", now_ms);
    }
    ++changed;
  }
  auto query = connection.prepare(
      "SELECT intent.intent_id,intent.state_version,speech.state,"
      "COALESCE(speech.last_error_code,'speech_terminal') FROM "
      "voice_narration_intent intent JOIN speech_item speech ON "
      "speech.speech_id=intent.speech_id WHERE intent.state='queued' AND "
      "speech.state IN ('played','failed','expired','cancelled') LIMIT 32");
  struct Item {
    std::string id;
    std::size_t revision;
    std::string state;
    std::string reason;
  };
  std::vector<Item> items;
  while (query.step())
    items.push_back({query.column_text(0),
                     static_cast<std::size_t>(query.column_int64(1)),
                     query.column_text(2), query.column_text(3)});
  for (const auto &item : items) {
    const auto target = item.state == "played" ? "played" : item.state;
    terminalize(connection, item.id, item.revision, "queued", target,
                item.state == "played" ? "marker_completed" : item.reason,
                now_ms, next_id());
    ++changed;
  }
  transaction.commit();
  return changed;
}

std::optional<VoxNarrationCandidate>
SqliteVoxNarrationRepository::preview(const std::string_view source_event_id,
                                      const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  const auto event = find_event(connection, source_event_id);
  if (!event)
    return std::nullopt;
  const auto policy = vox_narration_policy(event->event_type);
  if (!policy || event->recorded_at_ms + policy->ttl_ms <= now_ms)
    return std::nullopt;
  const auto projection = project(connection, *event);
  if (!projection.eligible)
    return std::nullopt;
  return VoxNarrationCandidate{
      .intent_id = {},
      .revision = 0,
      .source_event_id = event->event_id,
      .event_type = event->event_type,
      .feature = policy->feature,
      .guild_id = event->guild_id,
      .channel_id = event->channel_id,
      .safe_input = projection.safe_input,
      .fallback_line = policy->fallback_line,
      .rank = policy->rank,
      .created_at_ms = event->recorded_at_ms,
      .expires_at_ms = event->recorded_at_ms + policy->ttl_ms,
      .session_id = {},
      .counterpart_outbox_id = projection.counterpart_outbox_id,
      .counterpart_required = policy->counterpart_required,
      .is_test = projection.is_test};
}

std::vector<VoxNarrationRecent>
SqliteVoxNarrationRepository::recent(const std::size_t limit) {
  if (limit > 10)
    throw std::invalid_argument{"Narration recent limit is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT intent_id,event_type,feature,state,terminal_reason,created_at_ms "
      "FROM voice_narration_intent ORDER BY created_at_ms DESC,intent_id DESC "
      "LIMIT ?");
  query.bind(1, static_cast<std::int64_t>(limit));
  std::vector<VoxNarrationRecent> result;
  while (query.step())
    result.push_back({.intent_id = query.column_text(0),
                      .event_type = query.column_text(1),
                      .feature = query.column_text(2),
                      .state = query.column_text(3),
                      .reason = optional_text(query, 4),
                      .created_at_ms = query.column_int64(5)});
  return result;
}

VoxNarrationHealth SqliteVoxNarrationRepository::health() {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT COALESCE(SUM(state='pending'),0),"
      "COALESCE(SUM(state='generating'),0),COALESCE(SUM(state='queued'),0),"
      "COALESCE(SUM(slot='feature' AND session_id=(SELECT "
      "session_id FROM voice_session WHERE state IN "
      "('connecting','ready','muted','reconnecting','leaving') ORDER BY "
      "started_at_ms DESC,session_id DESC LIMIT 1) AND (state IN "
      "('generating','prepared','queued','played') OR (state IN "
      "('failed','expired','cancelled') AND EXISTS(SELECT 1 FROM "
      "speech_item_transition history WHERE history.speech_id="
      "voice_narration_intent.speech_id AND history.to_state='playing')))),0) "
      "FROM voice_narration_intent");
  static_cast<void>(query.step());
  auto cursor = context_->connection().prepare(
      "SELECT last_event_rowid,(SELECT COALESCE(max(rowid),0) FROM "
      "event_journal) "
      "FROM voice_narration_cursor WHERE singleton=1");
  if (!cursor.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Narration cursor is missing."};
  return {.pending = static_cast<std::size_t>(query.column_int64(0)),
          .generating = static_cast<std::size_t>(query.column_int64(1)),
          .queued = static_cast<std::size_t>(query.column_int64(2)),
          .session_feature_count =
              static_cast<std::size_t>(query.column_int64(3)),
          .cursor_rowid = cursor.column_int64(0),
          .journal_head_rowid = cursor.column_int64(1)};
}

std::optional<std::string> SqliteVoxNarrationRepository::control_receipt(
    const VoxNarrationControlContext &context) {
  validate_control_context(context);
  const std::scoped_lock lock{context_->mutex()};
  return load_control_receipt(context_->connection(), context);
}

std::string SqliteVoxNarrationRepository::record_control_receipt(
    const VoxNarrationControlContext &context, std::string message) {
  validate_control_context(context);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto result = store_control_receipt_uncommitted(connection, context,
                                                  std::move(message));
  transaction.commit();
  return result;
}

bool SqliteVoxNarrationRepository::automatic_speech_suppressed(
    const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  return now_ms < 0 || quiet(context_->connection(), now_ms);
}

bool SqliteVoxNarrationRepository::automatic_speech_admission_suppressed(
    const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  return now_ms < 0 || quiet(context_->connection(), now_ms) ||
         !narration_budget_available(context_->connection(), now_ms,
                                     usage_policy_);
}

std::optional<std::string> SqliteVoxNarrationRepository::session_flavor_context(
    const std::string_view session_id, const std::string_view guild_id,
    const std::string_view summoner_user_id) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto active = connection.prepare(
      "SELECT 1 FROM voice_session WHERE session_id=? AND guild_id=? AND "
      "summoner_user_id=? AND state IN ('connecting','ready','reconnecting')");
  active.bind(1, session_id);
  active.bind(2, guild_id);
  active.bind(3, summoner_user_id);
  if (!active.step())
    return std::nullopt;
  auto summoner = connection.prepare(
      "SELECT COALESCE(display_name_cache,username_cache,'a member of the "
      "company') FROM discord_user WHERE user_id=? AND is_bot=0");
  summoner.bind(1, summoner_user_id);
  if (!summoner.step())
    return std::nullopt;
  std::string context =
      "Summoner display name: " + summoner.column_text(0) + ".";
  auto title = connection.prepare(
      "SELECT definition.title FROM chronicle_title_grant grant JOIN "
      "chronicle_title_definition definition ON definition.definition_id="
      "grant.definition_id WHERE grant.recipient_user_id=? AND "
      "grant.state='active' AND grant.featured=1 ORDER BY grant.decided_at_ms "
      "DESC,grant.grant_id DESC LIMIT 1");
  title.bind(1, summoner_user_id);
  if (title.step())
    context += " Public featured title: " + title.column_text(0) + ".";
  auto chapter = connection.prepare(
      "SELECT title FROM chronicle_entry WHERE source_guild_id=? AND "
      "visibility='shared' AND status='canon' AND "
      "source_kind='session_summary' ORDER BY occurred_at_ms DESC,entry_id "
      "DESC LIMIT 1");
  chapter.bind(1, guild_id);
  if (chapter.step())
    context +=
        " Approved public chapter heading: " + chapter.column_text(0) + ".";
  auto continuity = connection.prepare(
      "SELECT EXISTS(SELECT 1 FROM voice_session WHERE guild_id=? AND "
      "ended_at_ms IS NOT NULL)");
  continuity.bind(1, guild_id);
  if (continuity.step() && continuity.column_int64(0) != 0)
    context += " Prior Vox-session continuity: returning company.";
  return context;
}

} // namespace sanguinius::persistence
