#include "sanguinius/persistence/sqlite_voice_input_repository.hpp"

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/speech_service.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] VoiceListeningState state(const std::string_view value) {
  if (value == "proposed")
    return VoiceListeningState::proposed;
  if (value == "arming_transport")
    return VoiceListeningState::arming_transport;
  if (value == "arming_indicator")
    return VoiceListeningState::arming_indicator;
  if (value == "active")
    return VoiceListeningState::active;
  if (value == "transcribing")
    return VoiceListeningState::transcribing;
  if (value == "completed")
    return VoiceListeningState::completed;
  if (value == "stopped")
    return VoiceListeningState::stopped;
  if (value == "failed")
    return VoiceListeningState::failed;
  if (value == "abandoned")
    return VoiceListeningState::abandoned;
  throw std::runtime_error{"Unknown voice listening state."};
}

[[nodiscard]] std::optional<std::int64_t> optional_integer(SqliteStatement &row,
                                                           const int column) {
  return row.column_is_null(column)
             ? std::nullopt
             : std::optional<std::int64_t>{row.column_int64(column)};
}

[[nodiscard]] std::optional<std::string> optional_text(SqliteStatement &row,
                                                       const int column) {
  return row.column_is_null(column)
             ? std::nullopt
             : std::optional<std::string>{row.column_text(column)};
}

constexpr std::string_view window_columns =
    "window_id,vox_session_id,guild_id,text_channel_id,voice_channel_id,"
    "requester_user_id,state,state_version,connection_generation,"
    "requested_seconds,initial_human_count,reserved_micro_usd,"
    "provider_attempt_started,provider_attempt_started_at_ms,created_at_ms,"
    "active_at_ms,ended_at_ms,public_message_id,terminal_reason";

[[nodiscard]] VoiceListeningWindow read_window(SqliteStatement &row) {
  return {.window_id = row.column_text(0),
          .vox_session_id = row.column_text(1),
          .guild_id = DiscordSnowflake::parse(row.column_text(2)),
          .text_channel_id = DiscordSnowflake::parse(row.column_text(3)),
          .voice_channel_id = DiscordSnowflake::parse(row.column_text(4)),
          .requester_user_id = DiscordSnowflake::parse(row.column_text(5)),
          .state = state(row.column_text(6)),
          .revision = static_cast<std::size_t>(row.column_int64(7)),
          .connection_generation =
              static_cast<std::uint64_t>(row.column_int64(8)),
          .requested_seconds = static_cast<std::size_t>(row.column_int64(9)),
          .initial_human_count = static_cast<std::size_t>(row.column_int64(10)),
          .reserved_micro_usd = row.column_int64(11),
          .provider_attempt_started = row.column_int64(12) != 0,
          .provider_attempt_started_at_ms = optional_integer(row, 13),
          .created_at_ms = row.column_int64(14),
          .active_at_ms = optional_integer(row, 15),
          .ended_at_ms = optional_integer(row, 16),
          .public_message_id =
              row.column_is_null(17)
                  ? std::nullopt
                  : std::optional{DiscordSnowflake::parse(row.column_text(17))},
          .terminal_reason = optional_text(row, 18)};
}

[[nodiscard]] std::optional<VoiceListeningWindow>
load_window(SqliteConnection &connection, const std::string_view window_id) {
  auto query = connection.prepare("SELECT " + std::string{window_columns} +
                                  " FROM voice_listening_window WHERE "
                                  "window_id=?");
  query.bind(1, window_id);
  if (!query.step())
    return std::nullopt;
  auto result = read_window(query);
  if (query.step())
    throw std::runtime_error{"Duplicate voice listening window."};
  return result;
}

void ensure_transition_actor(SqliteConnection &connection,
                             const DiscordSnowflake actor_user_id,
                             const std::int64_t observed_at_ms) {
  auto user = connection.prepare(
      "INSERT INTO discord_user(user_id,display_name_cache,username_cache,"
      "is_bot,first_seen_at_ms,last_seen_at_ms,created_at_ms,updated_at_ms) "
      "VALUES(?,NULL,NULL,0,?,?,?,?) ON CONFLICT(user_id) DO NOTHING");
  user.bind(1, actor_user_id.str());
  user.bind(2, observed_at_ms);
  user.bind(3, observed_at_ms);
  user.bind(4, observed_at_ms);
  user.bind(5, observed_at_ms);
  user.execute();

  auto preference = connection.prepare(
      "INSERT INTO user_preference(user_id,updated_at_ms) VALUES(?,?) "
      "ON CONFLICT(user_id) DO NOTHING");
  preference.bind(1, actor_user_id.str());
  preference.bind(2, observed_at_ms);
  preference.execute();
}

void require_window(const VoiceListeningWindow &window) {
  if (!valid_uuid_v4(window.window_id) ||
      !valid_uuid_v4(window.vox_session_id) || !window.guild_id.is_set() ||
      !window.text_channel_id.is_set() || !window.voice_channel_id.is_set() ||
      !window.requester_user_id.is_set() ||
      window.state != VoiceListeningState::proposed || window.revision != 1 ||
      window.connection_generation == 0 ||
      (window.requested_seconds != 5 && window.requested_seconds != 10 &&
       window.requested_seconds != 15) ||
      window.initial_human_count == 0 ||
      window.reserved_micro_usd !=
          estimated_transcription_cost_micro_usd(
              static_cast<std::int64_t>(window.requested_seconds)) ||
      window.provider_attempt_started ||
      window.provider_attempt_started_at_ms.has_value() ||
      window.created_at_ms < 0)
    throw std::invalid_argument{"Invalid voice listening window."};
}

void require_usage(const VoiceTranscriptionUsage &usage) {
  if (!valid_uuid_v4(usage.window_id) || usage.provider != "openai" ||
      usage.model != transcription_model ||
      usage.captured_bytes > maximum_voice_pcm_bytes ||
      usage.captured_duration_ms < 0 || usage.captured_duration_ms > 15'000 ||
      usage.estimated_micro_usd < 0 || usage.latency_ms < 0 ||
      usage.result_code.empty() || usage.result_code.size() > 64 ||
      usage.recorded_at_ms < 0 ||
      (usage.provider_request_id &&
       sanitize_transcription_request_id(*usage.provider_request_id) !=
           *usage.provider_request_id))
    throw std::invalid_argument{"Invalid voice transcription usage."};
}

void insert_usage(SqliteConnection &connection,
                  const VoiceTranscriptionUsage &usage,
                  const bool ignore_conflict) {
  auto insert = connection.prepare(
      "INSERT INTO voice_transcription_usage(window_id,provider,model,"
      "provider_request_id,captured_bytes,captured_duration_ms,"
      "estimated_micro_usd,latency_ms,result_code,provider_sent,recorded_at_ms)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?)" +
      std::string{ignore_conflict ? " ON CONFLICT(window_id) DO NOTHING" : ""});
  insert.bind(1, usage.window_id);
  insert.bind(2, usage.provider);
  insert.bind(3, usage.model);
  if (usage.provider_request_id)
    insert.bind(4, *usage.provider_request_id);
  else
    insert.bind_null(4);
  insert.bind(5, static_cast<std::int64_t>(usage.captured_bytes));
  insert.bind(6, usage.captured_duration_ms);
  insert.bind(7, usage.estimated_micro_usd);
  insert.bind(8, usage.latency_ms);
  insert.bind(9, usage.result_code);
  insert.bind(10, usage.provider_sent ? 1 : 0);
  insert.bind(11, usage.recorded_at_ms);
  insert.execute();
}

[[nodiscard]] bool stored_usage_matches(SqliteConnection &connection,
                                        const VoiceTranscriptionUsage &usage) {
  auto query = connection.prepare(
      "SELECT provider,model,provider_request_id,captured_bytes,"
      "captured_duration_ms,estimated_micro_usd,latency_ms,result_code,"
      "provider_sent,recorded_at_ms FROM voice_transcription_usage WHERE "
      "window_id=?");
  query.bind(1, usage.window_id);
  if (!query.step())
    return false;
  const auto provider_request_id = optional_text(query, 2);
  const auto matches = query.column_text(0) == usage.provider &&
                       query.column_text(1) == usage.model &&
                       provider_request_id == usage.provider_request_id &&
                       query.column_int64(3) ==
                           static_cast<std::int64_t>(usage.captured_bytes) &&
                       query.column_int64(4) == usage.captured_duration_ms &&
                       query.column_int64(5) == usage.estimated_micro_usd &&
                       query.column_int64(6) == usage.latency_ms &&
                       query.column_text(7) == usage.result_code &&
                       (query.column_int64(8) != 0) == usage.provider_sent &&
                       query.column_int64(9) == usage.recorded_at_ms;
  return matches && !query.step();
}

[[nodiscard]] std::int64_t scalar(SqliteConnection &connection,
                                  const std::string_view sql,
                                  const std::int64_t parameter) {
  auto query = connection.prepare(sql);
  query.bind(1, parameter);
  if (!query.step())
    throw std::runtime_error{"Voice input aggregate query failed."};
  return query.column_int64(0);
}

} // namespace

SqliteVoiceListeningRepository::SqliteVoiceListeningRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite voice input context is required."};
}

void SqliteVoiceListeningRepository::record_consent_attestation(
    const bool attested, const DiscordSnowflake owner_user_id,
    std::string attestation_id, const std::int64_t now_ms) {
  if (!owner_user_id.is_set() || !valid_uuid_v4(attestation_id) || now_ms < 0)
    throw std::invalid_argument{"Invalid voice consent attestation."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto current = connection.prepare(
      "SELECT attested FROM voice_input_consent_attestation ORDER BY "
      "rowid DESC LIMIT 1");
  if (current.step() && (current.column_int64(0) != 0) == attested) {
    transaction.commit();
    return;
  }
  auto insert = connection.prepare(
      "INSERT INTO voice_input_consent_attestation(attestation_id,attested,"
      "owner_user_id,recorded_at_ms) VALUES(?,?,?,?)");
  insert.bind(1, attestation_id);
  insert.bind(2, attested ? 1 : 0);
  insert.bind(3, owner_user_id.str());
  insert.bind(4, now_ms);
  insert.execute();
  transaction.commit();
}

VoiceWindowBeginResult
SqliteVoiceListeningRepository::begin(const VoiceWindowBeginRequest &request,
                                      const TranscriptionUsagePolicy &policy) {
  require_window(request.window);
  if (request.interaction_idempotency_key.empty() ||
      request.interaction_idempotency_key.size() > 160 ||
      request.request_fingerprint.empty() ||
      request.request_fingerprint.size() > 64 ||
      !valid_uuid_v4(request.transition_id) ||
      policy.rolling_day_windows == 0 || policy.rolling_day_micro_usd < 1 ||
      policy.calendar_month_micro_usd < 1)
    throw std::invalid_argument{"Invalid voice listening begin request."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT window_id,request_fingerprint FROM voice_listening_window WHERE "
      "interaction_idempotency_key=?");
  replay.bind(1, request.interaction_idempotency_key);
  if (replay.step()) {
    if (replay.column_text(1) != request.request_fingerprint)
      throw std::runtime_error{
          "Voice listening idempotency key conflicts with existing data."};
    auto existing = load_window(connection, replay.column_text(0));
    transaction.commit();
    return {.code = VoiceWindowBeginCode::replay,
            .window = std::move(existing)};
  }
  auto kill = connection.prepare(
      "SELECT disabled FROM voice_input_control WHERE singleton=1");
  if (!kill.step())
    throw std::runtime_error{"Voice input control is missing."};
  if (kill.column_int64(0) != 0) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::kill_switch, .window = std::nullopt};
  }
  auto consent = connection.prepare(
      "SELECT attested FROM voice_input_consent_attestation ORDER BY "
      "rowid DESC LIMIT 1");
  if (!consent.step() || consent.column_int64(0) == 0) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::consent_missing,
            .window = std::nullopt};
  }
  auto active_query = connection.prepare(
      "SELECT count(*) FROM voice_listening_window WHERE state IN "
      "('proposed','arming_transport','arming_indicator','active',"
      "'transcribing')");
  if (!active_query.step())
    throw std::runtime_error{"Voice input active-window query failed."};
  if (active_query.column_int64(0) != 0) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::active_window,
            .window = std::nullopt};
  }
  const auto day_start = request.window.created_at_ms - 86'400'000;
  const auto month_start =
      calendar_month_start_utc_ms(request.window.created_at_ms);
  const auto day_windows = scalar(
      connection,
      "SELECT count(*) FROM voice_listening_window WHERE created_at_ms>=?",
      day_start);
  if (day_windows >= static_cast<std::int64_t>(policy.rolling_day_windows)) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::window_limit, .window = std::nullopt};
  }
  const auto cost_query = [](const std::string_view predicate) {
    return "SELECT COALESCE(sum(CASE WHEN u.provider_sent=1 THEN "
           "u.estimated_micro_usd WHEN w.reservation_released=0 THEN "
           "w.reserved_micro_usd ELSE 0 END),0) FROM voice_listening_window w "
           "LEFT JOIN voice_transcription_usage u ON u.window_id=w.window_id " +
           std::string{predicate};
  };
  const auto day_cost =
      scalar(connection, cost_query("WHERE w.created_at_ms>=?"), day_start);
  if (day_cost >
      policy.rolling_day_micro_usd - request.window.reserved_micro_usd) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::daily_budget, .window = std::nullopt};
  }
  const auto month_cost =
      scalar(connection, cost_query("WHERE w.created_at_ms>=?"), month_start);
  if (month_cost >
      policy.calendar_month_micro_usd - request.window.reserved_micro_usd) {
    transaction.commit();
    return {.code = VoiceWindowBeginCode::monthly_budget,
            .window = std::nullopt};
  }

  auto insert = connection.prepare(
      "INSERT INTO voice_listening_window(window_id,vox_session_id,guild_id,"
      "text_channel_id,voice_channel_id,requester_user_id,state,state_version,"
      "connection_generation,requested_seconds,initial_human_count,"
      "reserved_micro_usd,created_at_ms,interaction_idempotency_key,"
      "request_fingerprint) VALUES(?,?,?,?,?,?,'proposed',1,?,?,?,?,?,?,?)");
  insert.bind(1, request.window.window_id);
  insert.bind(2, request.window.vox_session_id);
  insert.bind(3, request.window.guild_id.str());
  insert.bind(4, request.window.text_channel_id.str());
  insert.bind(5, request.window.voice_channel_id.str());
  insert.bind(6, request.window.requester_user_id.str());
  insert.bind(7,
              static_cast<std::int64_t>(request.window.connection_generation));
  insert.bind(8, static_cast<std::int64_t>(request.window.requested_seconds));
  insert.bind(9, static_cast<std::int64_t>(request.window.initial_human_count));
  insert.bind(10, request.window.reserved_micro_usd);
  insert.bind(11, request.window.created_at_ms);
  insert.bind(12, request.interaction_idempotency_key);
  insert.bind(13, request.request_fingerprint);
  insert.execute();
  auto transition = connection.prepare(
      "INSERT INTO voice_listening_transition(transition_id,window_id,"
      "from_state,to_state,from_version,to_version,reason,idempotency_key,"
      "occurred_at_ms) "
      "VALUES(?,?,'none','proposed',0,1,'command_accepted',?,?)");
  transition.bind(1, request.transition_id);
  transition.bind(2, request.window.window_id);
  transition.bind(3, "voice:begin:" + request.window.window_id);
  transition.bind(4, request.window.created_at_ms);
  transition.execute();
  auto result = load_window(connection, request.window.window_id);
  transaction.commit();
  return {.code = VoiceWindowBeginCode::created, .window = std::move(result)};
}

std::optional<VoiceListeningWindow> SqliteVoiceListeningRepository::active() {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT " + std::string{window_columns} +
      " FROM voice_listening_window WHERE state IN "
      "('proposed','arming_transport','arming_indicator','active',"
      "'transcribing') ORDER BY created_at_ms,window_id LIMIT 2");
  if (!query.step())
    return std::nullopt;
  auto result = read_window(query);
  if (query.step())
    throw std::runtime_error{"Multiple active voice listening windows."};
  return result;
}

std::optional<VoiceListeningWindow> SqliteVoiceListeningRepository::transition(
    const VoiceWindowTransitionRequest &request) {
  if (!valid_uuid_v4(request.window_id) || request.expected_revision == 0 ||
      request.reason.empty() || request.reason.size() > 64 ||
      !valid_uuid_v4(request.transition_id) ||
      request.idempotency_key.empty() || request.idempotency_key.size() > 160 ||
      request.now_ms < 0)
    throw std::invalid_argument{"Invalid voice listening transition."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto current = load_window(connection, request.window_id);
  if (!current) {
    transaction.commit();
    return std::nullopt;
  }
  if (current->revision != request.expected_revision) {
    auto replay = connection.prepare(
        "SELECT 1 FROM voice_listening_transition WHERE idempotency_key=? AND "
        "window_id=? AND to_state=?");
    replay.bind(1, request.idempotency_key);
    replay.bind(2, request.window_id);
    replay.bind(3, voice_listening_state_name(request.target));
    if (replay.step()) {
      transaction.commit();
      return current;
    }
    transaction.commit();
    return std::nullopt;
  }
  if (!voice_listening_transition_allowed(current->state, request.target)) {
    transaction.commit();
    return std::nullopt;
  }
  auto latest_transition = connection.prepare(
      "SELECT occurred_at_ms FROM voice_listening_transition WHERE "
      "window_id=? AND to_version=?");
  latest_transition.bind(1, request.window_id);
  latest_transition.bind(2, static_cast<std::int64_t>(current->revision));
  if (!latest_transition.step())
    throw std::runtime_error{"Voice listening transition history is missing."};
  const auto effective_now_ms =
      std::max(request.now_ms, latest_transition.column_int64(0));
  if (request.actor_user_id)
    ensure_transition_actor(connection, *request.actor_user_id,
                            effective_now_ms);
  auto update = connection.prepare(
      "UPDATE voice_listening_window SET state=?,state_version=state_version+1,"
      "active_at_ms=CASE WHEN ?='active' THEN ? ELSE active_at_ms END,"
      "ended_at_ms=CASE WHEN ? IN ('completed','stopped','failed','abandoned') "
      "THEN ? ELSE ended_at_ms END,terminal_reason=CASE WHEN ? IN "
      "('completed','stopped','failed','abandoned') THEN ? ELSE "
      "terminal_reason "
      "END WHERE window_id=? AND state_version=?");
  const std::string target{voice_listening_state_name(request.target)};
  update.bind(1, target);
  update.bind(2, target);
  update.bind(3, effective_now_ms);
  update.bind(4, target);
  update.bind(5, effective_now_ms);
  update.bind(6, target);
  update.bind(7, request.reason);
  update.bind(8, request.window_id);
  update.bind(9, static_cast<std::int64_t>(request.expected_revision));
  update.execute();
  if (connection.changes() != 1) {
    transaction.commit();
    return std::nullopt;
  }
  auto insert = connection.prepare(
      "INSERT INTO voice_listening_transition(transition_id,window_id,"
      "from_state,to_state,from_version,to_version,reason,actor_user_id,"
      "idempotency_key,occurred_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?)");
  insert.bind(1, request.transition_id);
  insert.bind(2, request.window_id);
  insert.bind(3, voice_listening_state_name(current->state));
  insert.bind(4, target);
  insert.bind(5, static_cast<std::int64_t>(current->revision));
  insert.bind(6, static_cast<std::int64_t>(current->revision + 1));
  insert.bind(7, request.reason);
  if (request.actor_user_id)
    insert.bind(8, request.actor_user_id->str());
  else
    insert.bind_null(8);
  insert.bind(9, request.idempotency_key);
  insert.bind(10, effective_now_ms);
  insert.execute();
  auto result = load_window(connection, request.window_id);
  transaction.commit();
  return result;
}

std::optional<VoiceListeningWindow>
SqliteVoiceListeningRepository::complete_transcription(
    const VoiceWindowTransitionRequest &request,
    const VoiceTranscriptionUsage &usage) {
  require_usage(usage);
  if (!valid_uuid_v4(request.window_id) || request.expected_revision == 0 ||
      request.target != VoiceListeningState::completed ||
      request.reason.empty() || request.reason.size() > 64 ||
      request.actor_user_id.has_value() ||
      !valid_uuid_v4(request.transition_id) ||
      request.idempotency_key.empty() || request.idempotency_key.size() > 160 ||
      request.now_ms < 0 || usage.window_id != request.window_id ||
      usage.result_code != "completed" || !usage.provider_sent)
    throw std::invalid_argument{"Invalid completed voice transcription."};

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto current = load_window(connection, request.window_id);
  if (!current) {
    transaction.commit();
    return std::nullopt;
  }
  if (current->revision != request.expected_revision) {
    auto replay = connection.prepare(
        "SELECT 1 FROM voice_listening_transition WHERE idempotency_key=? AND "
        "window_id=? AND to_state='completed'");
    replay.bind(1, request.idempotency_key);
    replay.bind(2, request.window_id);
    const auto replayed = current->state == VoiceListeningState::completed &&
                          replay.step() &&
                          stored_usage_matches(connection, usage);
    transaction.commit();
    return replayed ? current : std::nullopt;
  }
  if (current->state != VoiceListeningState::transcribing ||
      !current->provider_attempt_started ||
      usage.estimated_micro_usd != current->reserved_micro_usd) {
    transaction.commit();
    return std::nullopt;
  }
  auto latest_transition = connection.prepare(
      "SELECT occurred_at_ms FROM voice_listening_transition WHERE "
      "window_id=? AND to_version=?");
  latest_transition.bind(1, request.window_id);
  latest_transition.bind(2, static_cast<std::int64_t>(current->revision));
  if (!latest_transition.step())
    throw std::runtime_error{"Voice listening transition history is missing."};
  const auto effective_now_ms =
      std::max(request.now_ms, latest_transition.column_int64(0));

  insert_usage(connection, usage, false);
  auto update = connection.prepare(
      "UPDATE voice_listening_window SET state='completed',"
      "state_version=state_version+1,ended_at_ms=?,terminal_reason=? WHERE "
      "window_id=? AND state='transcribing' AND state_version=?");
  update.bind(1, effective_now_ms);
  update.bind(2, request.reason);
  update.bind(3, request.window_id);
  update.bind(4, static_cast<std::int64_t>(request.expected_revision));
  update.execute();
  if (connection.changes() != 1)
    throw std::runtime_error{"Unable to complete voice transcription."};

  auto insert_transition = connection.prepare(
      "INSERT INTO voice_listening_transition(transition_id,window_id,"
      "from_state,to_state,from_version,to_version,reason,actor_user_id,"
      "idempotency_key,occurred_at_ms) VALUES(?,?,'transcribing','completed',"
      "?,?,?,NULL,?,?)");
  insert_transition.bind(1, request.transition_id);
  insert_transition.bind(2, request.window_id);
  insert_transition.bind(3, static_cast<std::int64_t>(current->revision));
  insert_transition.bind(4, static_cast<std::int64_t>(current->revision + 1));
  insert_transition.bind(5, request.reason);
  insert_transition.bind(6, request.idempotency_key);
  insert_transition.bind(7, effective_now_ms);
  insert_transition.execute();
  auto result = load_window(connection, request.window_id);
  transaction.commit();
  return result;
}

void SqliteVoiceListeningRepository::record_public_message(
    const std::string_view window_id, const DiscordSnowflake message_id,
    const std::int64_t now_ms) {
  if (!valid_uuid_v4(std::string{window_id}) || !message_id.is_set() ||
      now_ms < 0)
    throw std::invalid_argument{"Invalid voice listening public message."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE voice_listening_window SET public_message_id=? WHERE window_id=? "
      "AND public_message_id IS NULL AND state IN "
      "('proposed','arming_transport','arming_indicator')");
  update.bind(1, message_id.str());
  update.bind(2, window_id);
  update.execute();
  if (context_->connection().changes() != 1)
    throw std::runtime_error{"Unable to record voice public message."};
}

void SqliteVoiceListeningRepository::record_usage(
    const VoiceTranscriptionUsage &usage) {
  require_usage(usage);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  insert_usage(connection, usage, true);
  if (!usage.provider_sent) {
    auto release = connection.prepare(
        "UPDATE voice_listening_window SET reservation_released=1,"
        "reservation_released_at_ms=max(?,created_at_ms) WHERE window_id=? "
        "AND reservation_released=0 AND EXISTS(SELECT 1 FROM "
        "voice_transcription_usage WHERE window_id=? AND provider_sent=0) "
        "AND NOT EXISTS(SELECT 1 FROM voice_transcription_usage WHERE "
        "window_id=? AND provider_sent=1)");
    release.bind(1, usage.recorded_at_ms);
    release.bind(2, usage.window_id);
    release.bind(3, usage.window_id);
    release.bind(4, usage.window_id);
    release.execute();
  }
  transaction.commit();
}

void SqliteVoiceListeningRepository::record_provider_attempt(
    const std::string_view window_id, const std::int64_t now_ms) {
  if (!valid_uuid_v4(std::string{window_id}) || now_ms < 0)
    throw std::invalid_argument{"Invalid voice provider attempt."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE voice_listening_window SET provider_attempt_started=1,"
      "provider_attempt_started_at_ms=max(?,created_at_ms) WHERE window_id=? "
      "AND state='transcribing' AND provider_attempt_started=0");
  update.bind(1, now_ms);
  update.bind(2, window_id);
  update.execute();
  if (connection.changes() == 1)
    return;
  auto replay = connection.prepare(
      "SELECT provider_attempt_started FROM voice_listening_window WHERE "
      "window_id=? AND state='transcribing'");
  replay.bind(1, window_id);
  if (!replay.step() || replay.column_int64(0) == 0)
    throw std::runtime_error{"Unable to record voice provider attempt."};
}

void SqliteVoiceListeningRepository::release_reservation(
    const std::string_view window_id, const std::int64_t now_ms) {
  if (!valid_uuid_v4(std::string{window_id}) || now_ms < 0)
    throw std::invalid_argument{"Invalid voice reservation release."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE voice_listening_window SET reservation_released=1,"
      "reservation_released_at_ms=max(?,created_at_ms) WHERE window_id=? AND "
      "reservation_released=0 AND NOT EXISTS(SELECT 1 FROM "
      "voice_transcription_usage WHERE window_id=? AND provider_sent=1) AND "
      "(provider_attempt_started=0 OR EXISTS(SELECT 1 FROM "
      "voice_transcription_usage WHERE window_id=? AND provider_sent=0))");
  update.bind(1, now_ms);
  update.bind(2, window_id);
  update.bind(3, window_id);
  update.bind(4, window_id);
  update.execute();
}

bool SqliteVoiceListeningRepository::kill_switch_enabled() {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT disabled FROM voice_input_control WHERE singleton=1");
  if (!query.step())
    throw std::runtime_error{"Voice input control is missing."};
  return query.column_int64(0) != 0;
}

void SqliteVoiceListeningRepository::set_kill_switch(
    const bool enabled, const DiscordSnowflake actor_user_id,
    std::string change_id, const std::int64_t now_ms) {
  if (!actor_user_id.is_set() || !valid_uuid_v4(change_id) || now_ms < 0)
    throw std::invalid_argument{"Invalid voice input kill-switch change."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto current = connection.prepare(
      "SELECT disabled FROM voice_input_control WHERE singleton=1");
  if (!current.step())
    throw std::runtime_error{"Voice input control is missing."};
  if ((current.column_int64(0) != 0) == enabled) {
    transaction.commit();
    return;
  }
  auto update = connection.prepare(
      "UPDATE voice_input_control SET disabled=?,updated_at_ms=? WHERE "
      "singleton=1");
  update.bind(1, enabled ? 1 : 0);
  update.bind(2, now_ms);
  update.execute();
  auto insert = connection.prepare(
      "INSERT INTO voice_input_kill_change(change_id,disabled,actor_user_id,"
      "occurred_at_ms) VALUES(?,?,?,?)");
  insert.bind(1, change_id);
  insert.bind(2, enabled ? 1 : 0);
  insert.bind(3, actor_user_id.str());
  insert.bind(4, now_ms);
  insert.execute();
  transaction.commit();
}

std::size_t SqliteVoiceListeningRepository::abandon_nonterminal(
    const std::int64_t now_ms, const std::string_view reason,
    const std::string_view transition_prefix) {
  if (now_ms < 0 || reason.empty() || reason.size() > 64 ||
      transition_prefix.empty() || transition_prefix.size() > 64)
    throw std::invalid_argument{"Invalid voice input restart recovery."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT w.window_id,w.state,w.state_version,w.created_at_ms,"
      "t.occurred_at_ms FROM voice_listening_window w JOIN "
      "voice_listening_transition t ON t.window_id=w.window_id AND "
      "t.to_version=w.state_version WHERE "
      "state IN ('proposed','arming_transport','arming_indicator','active',"
      "'transcribing') ORDER BY w.window_id");
  struct Pending {
    std::string id;
    std::string state;
    std::int64_t version;
    std::int64_t effective_now_ms;
  };
  std::vector<Pending> pending;
  while (query.step())
    pending.push_back(
        {query.column_text(0), query.column_text(1), query.column_int64(2),
         std::max(now_ms,
                  std::max(query.column_int64(3), query.column_int64(4)))});
  for (const auto &item : pending) {
    auto update = connection.prepare(
        "UPDATE voice_listening_window SET state='abandoned',"
        "state_version=state_version+1,ended_at_ms=?,terminal_reason=?,"
        "reservation_released=CASE WHEN NOT EXISTS(SELECT 1 FROM "
        "voice_transcription_usage WHERE window_id=? AND provider_sent=1) "
        "AND (provider_attempt_started=0 OR EXISTS(SELECT 1 FROM "
        "voice_transcription_usage WHERE window_id=? AND provider_sent=0)) "
        "THEN 1 ELSE reservation_released END,reservation_released_at_ms="
        "CASE WHEN NOT EXISTS(SELECT 1 FROM voice_transcription_usage WHERE "
        "window_id=? AND provider_sent=1) AND (provider_attempt_started=0 OR "
        "EXISTS(SELECT 1 FROM voice_transcription_usage WHERE window_id=? AND "
        "provider_sent=0)) THEN ? ELSE "
        "reservation_released_at_ms END WHERE window_id=?");
    update.bind(1, item.effective_now_ms);
    update.bind(2, reason);
    update.bind(3, item.id);
    update.bind(4, item.id);
    update.bind(5, item.id);
    update.bind(6, item.id);
    update.bind(7, item.effective_now_ms);
    update.bind(8, item.id);
    update.execute();
    auto insert = connection.prepare(
        "INSERT INTO voice_listening_transition(transition_id,window_id,"
        "from_state,to_state,from_version,to_version,reason,idempotency_key,"
        "occurred_at_ms) VALUES(?,?,?,'abandoned',?,?,?, ?,?)");
    insert.bind(1, std::string{transition_prefix} + item.id);
    insert.bind(2, item.id);
    insert.bind(3, item.state);
    insert.bind(4, item.version);
    insert.bind(5, item.version + 1);
    insert.bind(6, reason);
    insert.bind(7, "voice:abandon:" + item.id);
    insert.bind(8, item.effective_now_ms);
    insert.execute();
  }
  transaction.commit();
  return pending.size();
}

VoiceListeningRepositoryHealth
SqliteVoiceListeningRepository::health(const std::int64_t now_ms) {
  if (now_ms < 0)
    throw std::invalid_argument{"Invalid voice input health time."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto one = [&connection](const std::string_view sql) {
    auto query = connection.prepare(sql);
    if (!query.step())
      throw std::runtime_error{"Voice input health query failed."};
    return query.column_int64(0);
  };
  const auto day_start = now_ms - 86'400'000;
  const auto month_start = calendar_month_start_utc_ms(now_ms);
  auto cost = [&connection](const std::int64_t start) {
    return scalar(connection,
                  "SELECT COALESCE(sum(CASE WHEN u.provider_sent=1 THEN "
                  "u.estimated_micro_usd WHEN w.reservation_released=0 THEN "
                  "w.reserved_micro_usd ELSE 0 END),0) FROM "
                  "voice_listening_window w LEFT JOIN "
                  "voice_transcription_usage u ON u.window_id=w.window_id "
                  "WHERE w.created_at_ms>=?",
                  start);
  };
  auto last = connection.prepare(
      "SELECT result_code FROM voice_transcription_usage ORDER BY "
      "recorded_at_ms DESC,window_id DESC LIMIT 1");
  const auto last_result =
      last.step() ? std::optional{last.column_text(0)} : std::nullopt;
  auto windows = connection.prepare(
      "SELECT count(*) FROM voice_listening_window WHERE created_at_ms>=?");
  windows.bind(1, day_start);
  if (!windows.step())
    throw std::runtime_error{"Voice input health window query failed."};
  return {.active_windows = static_cast<std::size_t>(
              one("SELECT count(*) FROM voice_listening_window WHERE state IN "
                  "('proposed','arming_transport','arming_indicator','active',"
                  "'transcribing')")),
          .day_windows = static_cast<std::size_t>(windows.column_int64(0)),
          .day_micro_usd = cost(day_start),
          .month_micro_usd = cost(month_start),
          .kill_switch = one("SELECT disabled FROM voice_input_control WHERE "
                             "singleton=1") != 0,
          .last_result_code = last_result};
}

} // namespace sanguinius::persistence
