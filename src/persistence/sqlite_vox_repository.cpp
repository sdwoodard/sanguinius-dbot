#include "sanguinius/persistence/sqlite_vox_repository.hpp"

#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <ranges>
#include <sqlite3.h>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

[[nodiscard]] VoxCommandResult
make_result(VoxResultCode code,
            std::optional<VoxSession> session = std::nullopt,
            std::string message = {}, bool wake_scheduler = false,
            bool wake_outbox = false) {
  return {.code = code,
          .session = std::move(session),
          .message = std::move(message),
          .wake_scheduler = wake_scheduler,
          .wake_outbox = wake_outbox};
}

[[nodiscard]] VoxState state_from(const std::string_view value) {
  if (value == "connecting")
    return VoxState::connecting;
  if (value == "ready")
    return VoxState::ready;
  if (value == "muted")
    return VoxState::muted;
  if (value == "reconnecting")
    return VoxState::reconnecting;
  if (value == "leaving")
    return VoxState::leaving;
  if (value == "inactive")
    return VoxState::inactive;
  if (value == "failed")
    return VoxState::failed;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored Vox state is invalid."};
}

[[nodiscard]] VoxFixtureState fixture_from(const std::string_view value) {
  if (value == "pending")
    return VoxFixtureState::pending;
  if (value == "queued")
    return VoxFixtureState::queued;
  if (value == "played")
    return VoxFixtureState::played;
  if (value == "failed")
    return VoxFixtureState::failed;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored Vox fixture state is invalid."};
}

[[nodiscard]] std::optional<std::string> optional_text(SqliteStatement &query,
                                                       const int column) {
  return query.column_is_null(column)
             ? std::nullopt
             : std::optional<std::string>{query.column_text(column)};
}

[[nodiscard]] std::optional<std::int64_t> optional_time(SqliteStatement &query,
                                                        const int column) {
  return query.column_is_null(column)
             ? std::nullopt
             : std::optional<std::int64_t>{query.column_int64(column)};
}

[[nodiscard]] VoxSession session_from(SqliteStatement &query) {
  return {.session_id = query.column_text(0),
          .guild_id = DiscordSnowflake::parse(query.column_text(1)),
          .text_channel_id = DiscordSnowflake::parse(query.column_text(2)),
          .voice_channel_id = DiscordSnowflake::parse(query.column_text(3)),
          .summoner_user_id = DiscordSnowflake::parse(query.column_text(4)),
          .deployment_instance_id = query.column_text(5),
          .state = state_from(query.column_text(6)),
          .revision = static_cast<std::size_t>(query.column_int64(7)),
          .connection_generation =
              static_cast<std::uint64_t>(query.column_int64(8)),
          .reconnect_count = static_cast<std::size_t>(query.column_int64(9)),
          .fixture_state = fixture_from(query.column_text(10)),
          .fixture_marker = optional_text(query, 11),
          .empty_since_ms = optional_time(query, 12),
          .timeout_job_id = optional_text(query, 13),
          .muted_at_ms = optional_time(query, 19),
          .mute_until_ms = optional_time(query, 20),
          .mute_job_id = optional_text(query, 21),
          .started_at_ms = query.column_int64(14),
          .last_active_at_ms = query.column_int64(15),
          .ended_at_ms = optional_time(query, 16),
          .end_reason = optional_text(query, 17),
          .last_failure_category = optional_text(query, 18)};
}

[[nodiscard]] std::optional<VoxSession>
load_session(SqliteConnection &connection, const std::string_view session_id) {
  auto query = connection.prepare(
      "SELECT session_id,guild_id,text_channel_id,voice_channel_id,"
      "summoner_user_id,deployment_instance_id,state,state_version,"
      "connection_generation,reconnect_count,fixture_state,fixture_marker,"
      "empty_since_ms,timeout_job_id,started_at_ms,last_active_at_ms,"
      "ended_at_ms,end_reason,last_failure_category,muted_at_ms,mute_until_ms,"
      "mute_job_id FROM voice_session WHERE "
      "session_id=?");
  query.bind(1, session_id);
  if (!query.step())
    return std::nullopt;
  auto result = session_from(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Vox session identity."};
  return result;
}

[[nodiscard]] std::optional<VoxSession>
load_active(SqliteConnection &connection) {
  auto query = connection.prepare(
      "SELECT session_id,guild_id,text_channel_id,voice_channel_id,"
      "summoner_user_id,deployment_instance_id,state,state_version,"
      "connection_generation,reconnect_count,fixture_state,fixture_marker,"
      "empty_since_ms,timeout_job_id,started_at_ms,last_active_at_ms,"
      "ended_at_ms,end_reason,last_failure_category,muted_at_ms,mute_until_ms,"
      "mute_job_id FROM voice_session WHERE "
      "state IN ('connecting','ready','muted','reconnecting','leaving') "
      "ORDER BY started_at_ms DESC LIMIT 1");
  if (!query.step())
    return std::nullopt;
  return session_from(query);
}

void cancel_job(SqliteConnection &connection,
                const std::optional<std::string> &job_id,
                const std::int64_t now_ms) {
  if (!job_id)
    return;
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state='cancelled',"
      "terminal_at_ms=max(?,created_at_ms,updated_at_ms),"
      "updated_at_ms=max(?,created_at_ms,updated_at_ms),lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL "
      "WHERE job_id=? AND state IN ('pending','claimed')");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, *job_id);
  update.execute();
}

void cancel_unsent_public_cards(SqliteConnection &connection,
                                const std::string_view session_id,
                                const std::int64_t now_ms) {
  auto quarantine = connection.prepare(
      "UPDATE outbox_message SET state='failed',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms,updated_at_ms),"
      "updated_at_ms=max(?,created_at_ms,updated_at_ms),"
      "last_error_code='discord_unknown_outcome_vox_closed' WHERE kind=? AND "
      "aggregate_type='voice_session' AND aggregate_id=? AND state='claimed' "
      "AND submission_started_at_ms IS NOT NULL");
  quarantine.bind(1, now_ms);
  quarantine.bind(2, now_ms);
  quarantine.bind(3, std::string{public_discord_outbox_kind});
  quarantine.bind(4, session_id);
  quarantine.execute();

  auto cancel = connection.prepare(
      "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms,updated_at_ms),"
      "updated_at_ms=max(?,created_at_ms,updated_at_ms),"
      "last_error_code='vox_session_closed' WHERE kind=? AND "
      "aggregate_type='voice_session' AND aggregate_id=? AND "
      "state IN ('pending','claimed')");
  cancel.bind(1, now_ms);
  cancel.bind(2, now_ms);
  cancel.bind(3, std::string{public_discord_outbox_kind});
  cancel.bind(4, session_id);
  cancel.execute();
}

void complete_claimed_job(SqliteConnection &connection,
                          const ClaimedScheduledJob &job,
                          const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state='completed',"
      "completed_at_ms=max(?,created_at_ms,updated_at_ms),"
      "terminal_at_ms=max(?,created_at_ms,updated_at_ms),"
      "updated_at_ms=max(?,created_at_ms,updated_at_ms),lease_owner=NULL,"
      "lease_token=NULL,"
      "lease_until_ms=NULL WHERE job_id=? AND state='claimed' AND "
      "lease_owner=? AND lease_token=?");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, now_ms);
  update.bind(4, job.job_id);
  update.bind(5, job.lease_owner);
  update.bind(6, job.lease_token);
  update.execute();
}

[[nodiscard]] std::string event_type_for(const VoxState state,
                                         const std::string_view reason) {
  if (reason == "restart_abandoned")
    return "vox.session_abandoned.v1";
  switch (state) {
  case VoxState::connecting:
    return "vox.session_connecting.v1";
  case VoxState::ready:
    return reason == "voice_reconnected" ? "vox.session_reconnected.v1"
                                         : "vox.session_ready.v1";
  case VoxState::reconnecting:
    return "vox.session_reconnecting.v1";
  case VoxState::leaving:
    return "vox.session_leaving.v1";
  case VoxState::inactive:
    return "vox.session_ended.v1";
  case VoxState::failed:
    return "vox.session_failed.v1";
  case VoxState::muted:
    return "vox.session_muted.v1";
  }
  return "vox.session_failed.v1";
}

void insert_event(SqliteConnection &connection, const VoxSession &session,
                  const VoxTransitionRequest &request) {
  const auto payload = Json{{"session_id", session.session_id},
                            {"from_state", vox_state_name(session.state)},
                            {"to_state", vox_state_name(request.target)},
                            {"expected_revision", request.expected_revision},
                            {"reason", request.reason}};
  static_cast<void>(detail::insert_event_uncommitted(
      connection, {.event_id = request.event_id,
                   .event_type = event_type_for(request.target, request.reason),
                   .aggregate_type = "voice_session",
                   .aggregate_id = session.session_id,
                   .actor_user_id = request.actor_user_id,
                   .guild_id = session.guild_id,
                   .channel_id = session.text_channel_id,
                   .source_message_id = std::nullopt,
                   .occurred_at_ms = request.now_ms,
                   .recorded_at_ms = request.now_ms,
                   .correlation_id = request.correlation_id,
                   .causation_id = std::nullopt,
                   .idempotency_key = request.idempotency_key + ":event",
                   .payload_json = payload.dump()}));
}

void insert_timeout_job(SqliteConnection &connection, const VoxSession &session,
                        const VoxTransitionRequest &request,
                        const std::size_t new_revision) {
  if (!request.timeout_job_id || !request.timeout_due_at_ms)
    return;
  auto kind = vox_connect_timeout_job_type;
  if (request.target == VoxState::reconnecting)
    kind = vox_reconnect_timeout_job_type;
  else if (request.target == VoxState::leaving)
    kind = vox_leave_timeout_job_type;
  else if (request.target == VoxState::muted)
    kind = vox_mute_expiry_job_type;
  const ScheduledJobEnqueue job{
      .job_id = *request.timeout_job_id,
      .job_type = std::string{kind},
      .aggregate_type = "voice_session",
      .aggregate_id = session.session_id,
      .due_at_ms = *request.timeout_due_at_ms,
      .max_attempts = 3,
      .idempotency_key = "job:" + std::string{kind} + ":" + session.session_id +
                         ":" + std::to_string(new_revision),
      .created_at_ms = request.now_ms};
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_vox_timeout_payload(
          {.session_id = session.session_id, .expected_revision = new_revision},
          request.correlation_id, request.event_id)));
}

void insert_public_card(SqliteConnection &connection, const VoxSession &session,
                        const VoxState target, const std::string_view outbox_id,
                        const std::string_view event_id,
                        const std::string_view correlation_id,
                        const std::int64_t now_ms) {
  std::string content;
  if (target == VoxState::ready)
    content = "Vox Sanguinius is connected in <#" +
              session.voice_channel_id.str() + ">.";
  else if (target == VoxState::failed)
    content = "The Vox connection was lost and has been closed.";
  else
    content = "Vox Sanguinius has left the voice channel.";
  const OutboxEnqueue outbox{
      .outbox_id = std::string{outbox_id},
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "voice_session",
      .aggregate_id = session.session_id,
      .target_guild_id = session.guild_id,
      .target_channel_id = session.text_channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = now_ms,
      .max_attempts = 5,
      .idempotency_key =
          "outbox:vox:" + session.session_id + ":" + vox_state_name(target),
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = now_ms};
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_payload(
          PublicOutboxPayload{
              .request = {.guild_id = session.guild_id,
                          .channel_id = session.text_channel_id,
                          .message = text_message(std::move(content))}},
          correlation_id, std::string{event_id})));
  if (target != VoxState::ready) {
    auto ready = connection.prepare(
        "SELECT outbox_id FROM outbox_message WHERE kind=? AND "
        "aggregate_type='voice_session' AND aggregate_id=? AND "
        "idempotency_key=?");
    ready.bind(1, std::string{public_discord_outbox_kind});
    ready.bind(2, session.session_id);
    ready.bind(3, "outbox:vox:" + session.session_id + ":ready");
    if (ready.step()) {
      auto dependency = connection.prepare(
          "INSERT INTO voice_public_outbox_dependency(session_id,"
          "predecessor_outbox_id,successor_outbox_id,dependency_kind,"
          "created_at_ms) VALUES(?,?,?,'ready_before_terminal',?)");
      dependency.bind(1, session.session_id);
      dependency.bind(2, ready.column_text(0));
      dependency.bind(3, outbox_id);
      dependency.bind(4, now_ms);
      dependency.execute();
    }
  }
}

void insert_transition(SqliteConnection &connection, const VoxSession &before,
                       const VoxTransitionRequest &request,
                       const std::size_t new_revision) {
  auto insert = connection.prepare(
      "INSERT INTO voice_session_transition(transition_id,session_id,"
      "from_state,to_state,from_version,to_version,reason,actor_user_id,"
      "event_id,idempotency_key,occurred_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
  insert.bind(1, request.event_id);
  insert.bind(2, before.session_id);
  insert.bind(3, vox_state_name(before.state));
  insert.bind(4, vox_state_name(request.target));
  insert.bind(5, static_cast<std::int64_t>(before.revision));
  insert.bind(6, static_cast<std::int64_t>(new_revision));
  insert.bind(7, request.reason);
  if (request.actor_user_id)
    insert.bind(8, request.actor_user_id->str());
  else
    insert.bind_null(8);
  insert.bind(9, request.event_id);
  insert.bind(10, request.idempotency_key);
  insert.bind(11, request.now_ms);
  insert.execute();
}

[[nodiscard]] VoxSession fail_queued_fixture_uncommitted(
    SqliteConnection &connection, const VoxSession &before,
    const std::string_view event_id, const std::string_view correlation_id,
    const std::int64_t now_ms,
    const std::string_view failure_category = "playback_interrupted") {
  const bool interruptible_state = before.state == VoxState::ready ||
                                   before.state == VoxState::muted ||
                                   before.state == VoxState::reconnecting ||
                                   before.state == VoxState::leaving;
  if (before.fixture_state != VoxFixtureState::queued || !interruptible_state)
    return before;

  const auto idempotency_key = "vox:fixture-interrupted:" + before.session_id;
  static_cast<void>(detail::insert_event_uncommitted(
      connection, {.event_id = std::string{event_id},
                   .event_type = "vox.static_proof_failed.v1",
                   .aggregate_type = "voice_session",
                   .aggregate_id = before.session_id,
                   .actor_user_id = std::nullopt,
                   .guild_id = before.guild_id,
                   .channel_id = before.text_channel_id,
                   .source_message_id = std::nullopt,
                   .occurred_at_ms = now_ms,
                   .recorded_at_ms = now_ms,
                   .correlation_id = std::string{correlation_id},
                   .causation_id = std::nullopt,
                   .idempotency_key = idempotency_key + ":event",
                   .payload_json = Json{{"fixture_state", "failed"},
                                        {"failure_category", failure_category}}
                                       .dump()}));

  const auto new_revision = before.revision + 1;
  auto update = connection.prepare(
      "UPDATE voice_session SET state_version=?,fixture_state='failed',"
      "last_active_at_ms=max(last_active_at_ms,?),"
      "last_failure_category=? WHERE session_id=? AND state_version=?");
  update.bind(1, static_cast<std::int64_t>(new_revision));
  update.bind(2, now_ms);
  update.bind(3, failure_category);
  update.bind(4, before.session_id);
  update.bind(5, static_cast<std::int64_t>(before.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Queued Vox proof changed during timeout closure."};

  insert_transition(connection, before,
                    {.session_id = before.session_id,
                     .expected_revision = before.revision,
                     .target = before.state,
                     .reason = "fixture_failed",
                     .actor_user_id = std::nullopt,
                     .event_id = std::string{event_id},
                     .idempotency_key = idempotency_key,
                     .correlation_id = std::string{correlation_id},
                     .now_ms = now_ms,
                     .timeout_job_id = std::nullopt,
                     .timeout_due_at_ms = std::nullopt,
                     .failure_category = std::nullopt,
                     .public_card = false},
                    new_revision);
  return *load_session(connection, before.session_id);
}

[[nodiscard]] std::optional<VoxSession>
load_timeout_session(SqliteConnection &connection,
                     const std::string_view job_id) {
  auto query = connection.prepare(
      "SELECT session_id,guild_id,text_channel_id,voice_channel_id,"
      "summoner_user_id,deployment_instance_id,state,state_version,"
      "connection_generation,reconnect_count,fixture_state,fixture_marker,"
      "empty_since_ms,timeout_job_id,started_at_ms,last_active_at_ms,"
      "ended_at_ms,end_reason,last_failure_category,muted_at_ms,mute_until_ms,"
      "mute_job_id FROM voice_session WHERE "
      "(timeout_job_id=? OR mute_job_id=?) AND state IN "
      "('connecting','ready','muted','reconnecting','leaving') LIMIT 1");
  query.bind(1, job_id);
  query.bind(2, job_id);
  return query.step() ? std::optional{session_from(query)} : std::nullopt;
}

[[nodiscard]] bool dead_letter_claimed_job_uncommitted(
    SqliteConnection &connection, const ClaimedScheduledJob &job,
    const std::int64_t now_ms, const std::string_view error_code) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state='dead',lease_owner=NULL,lease_token=NULL,"
      "lease_until_ms=NULL,updated_at_ms=max(?,created_at_ms,updated_at_ms),"
      "terminal_at_ms=max(?,created_at_ms,updated_at_ms),last_error_code=? "
      "WHERE job_id=? AND state='claimed' AND lease_owner=? AND lease_token=?");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, error_code);
  update.bind(4, job.job_id);
  update.bind(5, job.lease_owner);
  update.bind(6, job.lease_token);
  update.execute();
  return connection.changes() == 1;
}

[[nodiscard]] VoxCommandResult
apply_transition(SqliteConnection &connection,
                 const VoxTransitionRequest &request,
                 const std::optional<std::string> &outbox_id) {
  auto replay =
      connection.prepare("SELECT session_id FROM voice_session_transition "
                         "WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step())
    return make_result(VoxResultCode::replay,
                       load_session(connection, replay.column_text(0)));
  auto before = load_session(connection, request.session_id);
  if (!before)
    return make_result(VoxResultCode::inactive);
  if (before->revision != request.expected_revision ||
      !vox_transition_allowed(before->state, request.target, request.reason,
                              before->reconnect_count))
    return make_result(VoxResultCode::invalid_state, before);

  const auto preserve_lifecycle_timeout =
      (before->state == VoxState::ready && request.target == VoxState::muted) ||
      (before->state == VoxState::muted && request.target == VoxState::ready &&
       (request.reason == "mute_off" || request.reason == "mute_expired"));
  if (!preserve_lifecycle_timeout)
    cancel_job(connection, before->timeout_job_id, request.now_ms);
  const auto preserve_existing_mute =
      (before->state == VoxState::muted &&
       request.target == VoxState::reconnecting) ||
      (before->state == VoxState::reconnecting && before->muted_at_ms &&
       request.target == VoxState::muted);
  if (!preserve_existing_mute)
    cancel_job(connection, before->mute_job_id, request.now_ms);
  const auto new_revision = before->revision + 1;
  insert_event(connection, *before, request);
  insert_timeout_job(connection, *before, request, new_revision);

  auto update = connection.prepare(
      "UPDATE voice_session SET state=?,state_version=?,reconnect_count=?,"
      "connection_generation=?,timeout_job_id=?,empty_since_ms=?,last_"
      "active_at_ms=max("
      "last_active_at_ms,?),ended_at_ms=?,end_reason=?,last_failure_category=?,"
      "muted_at_ms=?,mute_until_ms=?,mute_job_id=? WHERE session_id=? AND "
      "state_version=?");
  update.bind(1, vox_state_name(request.target));
  update.bind(2, static_cast<std::int64_t>(new_revision));
  const auto reconnects = before->reconnect_count +
                          (request.target == VoxState::reconnecting ? 1U : 0U);
  update.bind(3, static_cast<std::int64_t>(reconnects));
  const auto generation = before->connection_generation +
                          (request.target == VoxState::reconnecting ? 1U : 0U);
  update.bind(4, static_cast<std::int64_t>(generation));
  if (preserve_lifecycle_timeout && before->timeout_job_id)
    update.bind(5, *before->timeout_job_id);
  else if (request.target != VoxState::muted && request.timeout_job_id)
    update.bind(5, *request.timeout_job_id);
  else
    update.bind_null(5);
  if (preserve_lifecycle_timeout && before->empty_since_ms)
    update.bind(6, *before->empty_since_ms);
  else
    update.bind_null(6);
  update.bind(7, request.now_ms);
  if (request.target == VoxState::inactive ||
      request.target == VoxState::failed) {
    update.bind(8, std::max({request.now_ms, before->started_at_ms,
                             before->last_active_at_ms}));
    update.bind(9, request.reason);
  } else {
    update.bind_null(8);
    update.bind_null(9);
  }
  if (request.failure_category)
    update.bind(10, *request.failure_category);
  else if (request.target == VoxState::ready)
    update.bind_null(10);
  else if (before->last_failure_category)
    update.bind(10, *before->last_failure_category);
  else
    update.bind_null(10);
  const auto preserve_mute =
      (request.target == VoxState::reconnecting &&
       before->state == VoxState::muted) ||
      (request.target == VoxState::muted &&
       before->state == VoxState::reconnecting && before->muted_at_ms);
  if (request.target == VoxState::muted || preserve_mute) {
    update.bind(11, preserve_mute ? *before->muted_at_ms : request.now_ms);
    if (preserve_mute && before->mute_until_ms)
      update.bind(12, *before->mute_until_ms);
    else if (request.timeout_due_at_ms)
      update.bind(12, *request.timeout_due_at_ms);
    else
      update.bind_null(12);
    if (preserve_mute && before->mute_job_id)
      update.bind(13, *before->mute_job_id);
    else if (request.timeout_job_id)
      update.bind(13, *request.timeout_job_id);
    else
      update.bind_null(13);
  } else {
    update.bind_null(11);
    update.bind_null(12);
    update.bind_null(13);
  }
  update.bind(14, before->session_id);
  update.bind(15, static_cast<std::int64_t>(before->revision));
  update.execute();
  if (connection.changes() != 1)
    return make_result(VoxResultCode::invalid_state);
  insert_transition(connection, *before, request, new_revision);
  const auto after = load_session(connection, before->session_id);
  if (request.public_card && outbox_id)
    insert_public_card(connection, *after, request.target, *outbox_id,
                       request.event_id, request.correlation_id,
                       request.now_ms);
  return make_result(VoxResultCode::accepted, after, {},
                     request.timeout_job_id.has_value(),
                     request.public_card && outbox_id.has_value());
}

[[nodiscard]] std::string code_name(const VoxResultCode code) {
  return std::to_string(static_cast<int>(code));
}

void validate_auxiliary_receipt(const std::string_view operation,
                                const std::string_view fingerprint) {
  if ((operation != "say" && operation != "voice" &&
       operation != "speech_test") ||
      fingerprint.empty() || fingerprint.size() > 128 ||
      std::ranges::any_of(fingerprint, [](const char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte < 0x21U || byte > 0x7eU;
      }))
    throw std::invalid_argument{"Vox command receipt is invalid."};
}

void insert_receipt(
    SqliteConnection &connection, const VoxCommandContext &context,
    const std::string_view operation, const VoxCommandResult &result,
    const std::optional<std::string_view> request_fingerprint = std::nullopt) {
  auto request = Json{{"operation", operation},
                      {"actor_user_id", context.actor_user_id.str()}};
  if (request_fingerprint)
    request["request_fingerprint"] = *request_fingerprint;
  Json response{{"code", code_name(result.code)}, {"message", result.message}};
  if (result.session) {
    response["state"] = vox_state_name(result.session->state);
    response["revision"] = result.session->revision;
  }
  auto insert = connection.prepare(
      "INSERT INTO voice_interaction_receipt(idempotency_key,operation,"
      "actor_user_id,guild_id,channel_id,request_json,result_json,session_id,"
      "created_at_ms) VALUES(?,?,?,?,?,?,?,?,?)");
  insert.bind(1, context.interaction_idempotency_key);
  insert.bind(2, operation);
  insert.bind(3, context.actor_user_id.str());
  insert.bind(4, context.guild_id.str());
  insert.bind(5, context.text_channel_id.str());
  insert.bind(6, request.dump());
  insert.bind(7, response.dump());
  if (result.session)
    insert.bind(8, result.session->session_id);
  else
    insert.bind_null(8);
  insert.bind(9, context.now_ms);
  insert.execute();
}

[[nodiscard]] std::optional<VoxCommandResult> load_receipt(
    SqliteConnection &connection, const VoxCommandContext &context,
    const std::string_view operation,
    const std::optional<std::string_view> request_fingerprint = std::nullopt) {
  auto query = connection.prepare(
      "SELECT operation,actor_user_id,guild_id,channel_id,request_json,"
      "result_json,session_id FROM voice_interaction_receipt WHERE "
      "idempotency_key=?");
  query.bind(1, context.interaction_idempotency_key);
  if (!query.step())
    return std::nullopt;
  if (query.column_text(0) != operation ||
      query.column_text(1) != context.actor_user_id.str() ||
      query.column_text(2) != context.guild_id.str() ||
      query.column_text(3) != context.text_channel_id.str())
    throw std::invalid_argument{"Vox interaction idempotency key was reused."};
  const auto stored_request = Json::parse(query.column_text(4));
  if (request_fingerprint &&
      stored_request.value("request_fingerprint", std::string{}) !=
          *request_fingerprint)
    throw std::invalid_argument{"Vox interaction idempotency key was reused."};
  const auto result = Json::parse(query.column_text(5));
  std::optional<VoxSession> session;
  if (!query.column_is_null(6))
    session = load_session(connection, query.column_text(6));
  return VoxCommandResult{.code = VoxResultCode::replay,
                          .session = std::move(session),
                          .message = result.value("message", std::string{})};
}

[[nodiscard]] std::optional<VoxSession>
load_summon_fingerprint(SqliteConnection &connection,
                        const VoxCommandContext &context) {
  auto query =
      connection.prepare("SELECT session_id,from_version,to_state,reason FROM "
                         "voice_session_transition WHERE idempotency_key=?");
  query.bind(1, context.interaction_idempotency_key);
  if (!query.step())
    return std::nullopt;
  if (query.column_int64(1) != 0 || query.column_text(2) != "connecting" ||
      query.column_text(3) != "summoned") {
    throw std::invalid_argument{"Vox interaction idempotency key was reused."};
  }
  const auto session = load_session(connection, query.column_text(0));
  if (!session || session->guild_id != context.guild_id ||
      session->text_channel_id != context.text_channel_id ||
      session->summoner_user_id != context.actor_user_id) {
    throw std::invalid_argument{"Vox interaction idempotency key was reused."};
  }
  return session;
}

[[nodiscard]] std::optional<std::string>
load_initial_summon_key(SqliteConnection &connection,
                        const std::string_view session_id) {
  auto query = connection.prepare(
      "SELECT idempotency_key FROM voice_session_transition WHERE "
      "session_id=? AND from_version=0 AND to_state='connecting' AND "
      "reason='summoned'");
  query.bind(1, session_id);
  if (!query.step())
    return std::nullopt;
  return query.column_text(0);
}

[[nodiscard]] std::optional<VoxCommandResult>
recover_summon_receipt(SqliteConnection &connection,
                       const VoxCommandContext &context) {
  const auto session = load_summon_fingerprint(connection, context);
  if (!session)
    return std::nullopt;
  const bool active = session->state != VoxState::inactive &&
                      session->state != VoxState::failed;
  auto stored = make_result(
      active ? VoxResultCode::accepted : VoxResultCode::unavailable, session,
      active ? "The Vox connection is being established."
             : "The prior Vox summon was interrupted by a restart.");
  insert_receipt(connection, context, "summon", stored);
  stored.code = VoxResultCode::replay;
  return stored;
}

} // namespace

SqliteVoxRepository::SqliteVoxRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite Vox context is required."};
}

VoxCommandResult
SqliteVoxRepository::preflight_summon(const VoxCommandContext &context) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "summon")) {
    transaction.commit();
    return *receipt;
  }
  if (const auto recovered = recover_summon_receipt(connection, context)) {
    transaction.commit();
    return *recovered;
  }
  if (const auto existing = load_active(connection)) {
    VoxCommandResult result{
        .code = VoxResultCode::active_session,
        .session = existing,
        .message = "A Vox session is already active; leave it first."};
    insert_receipt(connection, context, "summon", result);
    transaction.commit();
    return result;
  }
  transaction.commit();
  return make_result(VoxResultCode::accepted);
}

VoxCommandResult
SqliteVoxRepository::record_summon_rejection(const VoxCommandContext &context,
                                             const VoxResultCode code,
                                             std::string message) {
  if (code != VoxResultCode::no_voice &&
      code != VoxResultCode::unsupported_channel &&
      code != VoxResultCode::permission_denied &&
      code != VoxResultCode::channel_full && code != VoxResultCode::unavailable)
    throw std::invalid_argument{"Invalid Vox summon rejection code."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "summon")) {
    transaction.commit();
    return *receipt;
  }
  auto result = make_result(code, std::nullopt, std::move(message));
  insert_receipt(connection, context, "summon", result);
  transaction.commit();
  return result;
}

VoxCommandResult SqliteVoxRepository::start(const VoxStartRequest &request) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt =
          load_receipt(connection, request.context, "summon")) {
    transaction.commit();
    return *receipt;
  }
  if (const auto recovered =
          recover_summon_receipt(connection, request.context)) {
    transaction.commit();
    return *recovered;
  }
  if (const auto existing = load_active(connection)) {
    VoxCommandResult result{
        .code = VoxResultCode::active_session,
        .session = existing,
        .message = "A Vox session is already active; leave it first."};
    insert_receipt(connection, request.context, "summon", result);
    transaction.commit();
    return result;
  }

  const VoxSession session{.session_id = request.session_id,
                           .guild_id = request.context.guild_id,
                           .text_channel_id = request.context.text_channel_id,
                           .voice_channel_id = request.voice_channel_id,
                           .summoner_user_id = request.context.actor_user_id,
                           .deployment_instance_id =
                               request.deployment_instance_id,
                           .state = VoxState::connecting,
                           .revision = 1,
                           .connection_generation = 1,
                           .reconnect_count = 0,
                           .fixture_state = VoxFixtureState::pending,
                           .fixture_marker = std::nullopt,
                           .empty_since_ms = std::nullopt,
                           .timeout_job_id = request.timeout_job_id,
                           .muted_at_ms = std::nullopt,
                           .mute_until_ms = std::nullopt,
                           .mute_job_id = std::nullopt,
                           .started_at_ms = request.context.now_ms,
                           .last_active_at_ms = request.context.now_ms,
                           .ended_at_ms = std::nullopt,
                           .end_reason = std::nullopt,
                           .last_failure_category = std::nullopt};
  const auto transition = VoxTransitionRequest{
      .session_id = session.session_id,
      .expected_revision = 0,
      .target = VoxState::connecting,
      .reason = "summoned",
      .actor_user_id = request.context.actor_user_id,
      .event_id = request.event_id,
      .idempotency_key = request.context.interaction_idempotency_key,
      .correlation_id = request.context.correlation_id,
      .now_ms = request.context.now_ms,
      .timeout_job_id = request.timeout_job_id,
      .timeout_due_at_ms = request.context.now_ms + vox_connect_timeout_ms,
      .failure_category = std::nullopt,
      .public_card = false};
  auto initial = session;
  initial.state = VoxState::inactive;
  initial.revision = 0;
  initial.timeout_job_id = std::nullopt;
  insert_event(connection, initial, transition);
  insert_timeout_job(connection, session, transition, 1);
  auto insert = connection.prepare(
      "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
      "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
      "state_version,connection_generation,reconnect_count,fixture_state,"
      "fixture_marker,fixture_queued_at_ms,fixture_played_at_ms,empty_since_ms,"
      "timeout_job_id,narration_event_rowid_floor,started_at_ms,last_active_at_ms,ended_at_ms,end_reason,"
      "last_failure_category) VALUES(?,?,?,?,?,?,'connecting',1,1,0,'pending',"
      "NULL,NULL,NULL,NULL,?,(SELECT COALESCE(max(rowid),0) FROM event_journal),"
      "?,?,NULL,NULL,NULL)");
  insert.bind(1, session.session_id);
  insert.bind(2, session.guild_id.str());
  insert.bind(3, session.text_channel_id.str());
  insert.bind(4, session.voice_channel_id.str());
  insert.bind(5, session.summoner_user_id.str());
  insert.bind(6, session.deployment_instance_id);
  insert.bind(7, request.timeout_job_id);
  insert.bind(8, session.started_at_ms);
  insert.bind(9, session.last_active_at_ms);
  insert.execute();
  insert_transition(connection, initial, transition, 1);
  auto result = make_result(VoxResultCode::accepted,
                            load_session(connection, session.session_id),
                            "The Vox connection is being established.", true);
  transaction.commit();
  return result;
}

VoxCommandResult SqliteVoxRepository::finalize_summon(
    const VoxCommandContext &context, const std::string_view session_id,
    const std::size_t expected_revision, const bool gateway_accepted,
    std::string event_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "summon")) {
    transaction.commit();
    return *receipt;
  }
  const auto current = load_session(connection, session_id);
  if (!current || current->revision != expected_revision ||
      current->state != VoxState::connecting ||
      current->guild_id != context.guild_id ||
      current->text_channel_id != context.text_channel_id ||
      current->summoner_user_id != context.actor_user_id) {
    transaction.commit();
    return make_result(VoxResultCode::invalid_state, current);
  }

  VoxCommandResult result;
  if (gateway_accepted) {
    result = make_result(VoxResultCode::accepted, current,
                         "The Vox connection is being established.");
  } else {
    result = apply_transition(
        connection,
        {.session_id = current->session_id,
         .expected_revision = current->revision,
         .target = VoxState::failed,
         .reason = "gateway_rejected",
         .actor_user_id = context.actor_user_id,
         .event_id = std::move(event_id),
         .idempotency_key =
             context.interaction_idempotency_key + ":gateway_rejected",
         .correlation_id = context.correlation_id,
         .now_ms = context.now_ms,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = "gateway_unavailable",
         .public_card = false},
        std::nullopt);
    if (result.code != VoxResultCode::accepted) {
      transaction.commit();
      return result;
    }
    result.code = VoxResultCode::unavailable;
    result.message = "Vox could not begin a new session.";
  }
  insert_receipt(connection, context, "summon", result);
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::command_status(const VoxCommandContext &context) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "status")) {
    transaction.commit();
    return *receipt;
  }
  auto session = load_active(connection);
  auto result = make_result(
      session ? VoxResultCode::accepted : VoxResultCode::inactive, session);
  result.message =
      render_vox_status(session ? &*session : nullptr, context.now_ms);
  insert_receipt(connection, context, "status", result);
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::command_leave(const VoxCommandContext &context,
                                   std::string event_id,
                                   std::string timeout_job_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "leave")) {
    transaction.commit();
    return *receipt;
  }
  const auto current = load_active(connection);
  if (!current) {
    auto result = make_result(VoxResultCode::inactive, std::nullopt,
                              "Vox is already inactive.");
    insert_receipt(connection, context, "leave", result);
    transaction.commit();
    return result;
  }
  if (context.actor_user_id != context.owner_user_id &&
      context.actor_user_id != current->summoner_user_id) {
    VoxCommandResult result{.code = VoxResultCode::unauthorized,
                            .session = current,
                            .message =
                                "Only the summoner or owner may dismiss Vox."};
    insert_receipt(connection, context, "leave", result);
    transaction.commit();
    return result;
  }
  auto result = apply_transition(
      connection,
      {.session_id = current->session_id,
       .expected_revision = current->revision,
       .target = VoxState::leaving,
       .reason = "commanded_leave",
       .actor_user_id = context.actor_user_id,
       .event_id = std::move(event_id),
       .idempotency_key = context.interaction_idempotency_key + ":leaving",
       .correlation_id = context.correlation_id,
       .now_ms = context.now_ms,
       .timeout_job_id = std::move(timeout_job_id),
       .timeout_due_at_ms = context.now_ms + vox_connect_timeout_ms,
       .failure_category = std::nullopt,
       .public_card = false},
      std::nullopt);
  result.message = "Vox Sanguinius is leaving the voice channel.";
  insert_receipt(connection, context, "leave", result);
  transaction.commit();
  return result;
}

VoxCommandResult SqliteVoxRepository::command_mute(
    const VoxCommandContext &context, const bool unmute,
    const std::optional<std::int64_t> mute_until_ms, std::string event_id,
    std::optional<std::string> mute_job_id) {
  if (unmute && (mute_until_ms || mute_job_id))
    throw std::invalid_argument{"Unmute must not carry expiry metadata."};
  if (mute_until_ms.has_value() != mute_job_id.has_value() ||
      (mute_until_ms && *mute_until_ms <= context.now_ms))
    throw std::invalid_argument{"Timed mute metadata is incomplete."};

  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt = load_receipt(connection, context, "mute")) {
    transaction.commit();
    return *receipt;
  }
  const auto current = load_active(connection);
  VoxCommandResult result;
  if (!current) {
    result = make_result(VoxResultCode::inactive, std::nullopt,
                         "Vox is not active.");
  } else if (context.actor_user_id != context.owner_user_id &&
             context.actor_user_id != current->summoner_user_id) {
    result = make_result(VoxResultCode::unauthorized, current,
                         "Only the summoner or owner may mute Vox.");
  } else if (unmute && current->state == VoxState::ready) {
    result = make_result(VoxResultCode::replay, current,
                         "Vox speech is already unmuted.");
  } else if (!unmute && current->state == VoxState::muted) {
    result = make_result(VoxResultCode::replay, current,
                         "Vox speech is already muted.");
  } else if ((unmute && current->state != VoxState::muted) ||
             (!unmute && current->state != VoxState::ready)) {
    result = make_result(VoxResultCode::invalid_state, current,
                         "Vox must be ready before changing mute state.");
  } else {
    result = apply_transition(
        connection,
        {.session_id = current->session_id,
         .expected_revision = current->revision,
         .target = unmute ? VoxState::ready : VoxState::muted,
         .reason = unmute ? "mute_off" : "mute_on",
         .actor_user_id = context.actor_user_id,
         .event_id = std::move(event_id),
         .idempotency_key = context.interaction_idempotency_key +
                            (unmute ? ":unmute" : ":mute"),
         .correlation_id = context.correlation_id,
         .now_ms = context.now_ms,
         .timeout_job_id = std::move(mute_job_id),
         .timeout_due_at_ms = mute_until_ms,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    result.message = unmute          ? "Vox speech is unmuted."
                     : mute_until_ms ? "Vox speech is muted until the "
                                       "selected expiry."
                                     : "Vox speech is muted for this "
                                       "session.";
  }
  insert_receipt(connection, context, "mute", result);
  transaction.commit();
  return result;
}

std::optional<VoxCommandResult> SqliteVoxRepository::command_receipt(
    const VoxCommandContext &context, const std::string_view operation,
    const std::string_view request_fingerprint) {
  validate_auxiliary_receipt(operation, request_fingerprint);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto receipt =
      load_receipt(connection, context, operation, request_fingerprint);
  transaction.commit();
  return receipt;
}

VoxCommandResult SqliteVoxRepository::record_command_receipt(
    const VoxCommandContext &context, const std::string_view operation,
    const std::string_view request_fingerprint, VoxCommandResult result) {
  validate_auxiliary_receipt(operation, request_fingerprint);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (auto receipt =
          load_receipt(connection, context, operation, request_fingerprint)) {
    transaction.commit();
    return *receipt;
  }
  insert_receipt(connection, context, operation, result, request_fingerprint);
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::command_test_disconnect(const VoxCommandContext &context,
                                             std::string event_id,
                                             std::string timeout_job_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto receipt =
          load_receipt(connection, context, "test_disconnect")) {
    transaction.commit();
    return *receipt;
  }
  const auto current = load_active(connection);
  VoxCommandResult result;
  if (context.actor_user_id != context.owner_user_id) {
    result = {.code = VoxResultCode::unauthorized,
              .session = current,
              .message = "This owner-only command is unavailable."};
  } else if (!current || current->state != VoxState::ready ||
             current->reconnect_count != 0) {
    result = {.code = VoxResultCode::invalid_state,
              .session = current,
              .message = "Vox is not ready for a reconnect exercise."};
  } else {
    result = apply_transition(
        connection,
        {.session_id = current->session_id,
         .expected_revision = current->revision,
         .target = VoxState::reconnecting,
         .reason = "owner_test_disconnect",
         .actor_user_id = context.actor_user_id,
         .event_id = std::move(event_id),
         .idempotency_key = context.interaction_idempotency_key + ":transition",
         .correlation_id = context.correlation_id,
         .now_ms = context.now_ms,
         .timeout_job_id = std::move(timeout_job_id),
         .timeout_due_at_ms = context.now_ms + vox_connect_timeout_ms,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    result.message = "A test disconnect was requested; Vox will rejoin once.";
  }
  insert_receipt(connection, context, "test_disconnect", result);
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::transition(const VoxTransitionRequest &request,
                                const std::optional<std::string> outbox_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto result = apply_transition(connection, request, outbox_id);
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::fixture(const VoxFixtureRequest &request) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay =
      connection.prepare("SELECT session_id FROM voice_session_transition "
                         "WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    auto result = make_result(VoxResultCode::replay,
                              load_session(connection, replay.column_text(0)));
    transaction.commit();
    return result;
  }
  auto before = load_session(connection, request.session_id);
  if (!before || before->revision != request.expected_revision) {
    transaction.commit();
    return make_result(VoxResultCode::invalid_state, before);
  }
  const bool ready_transition =
      before->state == VoxState::ready &&
      ((before->fixture_state == VoxFixtureState::pending &&
        request.target == VoxFixtureState::queued) ||
       (before->fixture_state == VoxFixtureState::queued &&
        request.target == VoxFixtureState::played));
  const bool interrupted_transition =
      before->fixture_state == VoxFixtureState::queued &&
      request.target == VoxFixtureState::failed &&
      (before->state == VoxState::ready || before->state == VoxState::muted ||
       before->state == VoxState::reconnecting ||
       before->state == VoxState::leaving);
  const bool valid = ready_transition || interrupted_transition;
  if (!valid) {
    transaction.commit();
    return make_result(VoxResultCode::invalid_state, before);
  }
  const auto new_revision = before->revision + 1;
  const auto state_name = vox_fixture_state_name(request.target);
  const auto event_type = request.target == VoxFixtureState::queued
                              ? "vox.static_proof_queued.v1"
                          : request.target == VoxFixtureState::played
                              ? "vox.static_proof_played.v1"
                              : "vox.static_proof_failed.v1";
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      {.event_id = request.event_id,
       .event_type = event_type,
       .aggregate_type = "voice_session",
       .aggregate_id = before->session_id,
       .actor_user_id = std::nullopt,
       .guild_id = before->guild_id,
       .channel_id = before->text_channel_id,
       .source_message_id = std::nullopt,
       .occurred_at_ms = request.now_ms,
       .recorded_at_ms = request.now_ms,
       .correlation_id = request.correlation_id,
       .causation_id = std::nullopt,
       .idempotency_key = request.idempotency_key + ":event",
       .payload_json = Json{{"fixture_state", state_name}}.dump()}));
  auto update = connection.prepare(
      "UPDATE voice_session SET state_version=?,fixture_state=?,"
      "fixture_marker=?,fixture_queued_at_ms=CASE WHEN ?='queued' THEN ? ELSE "
      "fixture_queued_at_ms END,fixture_played_at_ms=CASE WHEN ?='played' THEN "
      "? ELSE fixture_played_at_ms END,last_active_at_ms=max(last_active_at_ms,"
      "?),last_failure_category=CASE WHEN ?='failed' THEN ? ELSE "
      "last_failure_category END WHERE session_id=? AND state_version=?");
  update.bind(1, static_cast<std::int64_t>(new_revision));
  update.bind(2, state_name);
  update.bind(3, request.marker);
  update.bind(4, state_name);
  update.bind(5, request.now_ms);
  update.bind(6, state_name);
  update.bind(7, request.now_ms);
  update.bind(8, request.now_ms);
  update.bind(9, state_name);
  if (request.failure_category)
    update.bind(10, *request.failure_category);
  else
    update.bind_null(10);
  update.bind(11, before->session_id);
  update.bind(12, static_cast<std::int64_t>(before->revision));
  update.execute();
  const VoxTransitionRequest transition{
      .session_id = before->session_id,
      .expected_revision = before->revision,
      .target = before->state,
      .reason = std::string{"fixture_"} + state_name,
      .actor_user_id = std::nullopt,
      .event_id = request.event_id,
      .idempotency_key = request.idempotency_key,
      .correlation_id = request.correlation_id,
      .now_ms = request.now_ms,
      .timeout_job_id = std::nullopt,
      .timeout_due_at_ms = std::nullopt,
      .failure_category = std::nullopt,
      .public_card = false};
  insert_transition(connection, *before, transition, new_revision);
  auto result = make_result(VoxResultCode::accepted,
                            load_session(connection, before->session_id));
  transaction.commit();
  return result;
}

VoxCommandResult
SqliteVoxRepository::occupancy(const VoxOccupancyRequest &request) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto before = load_session(connection, request.session_id);
  if (!before || before->revision != request.expected_revision ||
      (before->state != VoxState::ready && before->state != VoxState::muted)) {
    transaction.commit();
    return make_result(VoxResultCode::invalid_state, before);
  }
  const bool becoming_empty =
      request.human_count == 0 && !before->empty_since_ms.has_value();
  const bool becoming_occupied =
      request.human_count > 0 && before->empty_since_ms.has_value();
  if (!becoming_empty && !becoming_occupied) {
    transaction.commit();
    return make_result(VoxResultCode::replay, before);
  }
  const auto new_revision = before->revision + 1;
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      {.event_id = request.event_id,
       .event_type =
           becoming_empty ? "vox.channel_empty.v1" : "vox.channel_occupied.v1",
       .aggregate_type = "voice_session",
       .aggregate_id = before->session_id,
       .actor_user_id = std::nullopt,
       .guild_id = before->guild_id,
       .channel_id = before->text_channel_id,
       .source_message_id = std::nullopt,
       .occurred_at_ms = request.now_ms,
       .recorded_at_ms = request.now_ms,
       .correlation_id = request.correlation_id,
       .causation_id = std::nullopt,
       .idempotency_key = request.idempotency_key + ":event",
       .payload_json = Json{{"human_count", request.human_count}}.dump()}));
  if (becoming_empty) {
    if (!request.empty_job_id)
      throw std::invalid_argument{"Empty Vox channel requires a timeout job."};
    const ScheduledJobEnqueue job{
        .job_id = *request.empty_job_id,
        .job_type = std::string{vox_empty_timeout_job_type},
        .aggregate_type = "voice_session",
        .aggregate_id = before->session_id,
        .due_at_ms = request.now_ms + vox_empty_timeout_ms,
        .max_attempts = 3,
        .idempotency_key = "job:vox-empty:" + before->session_id + ":" +
                           std::to_string(new_revision),
        .created_at_ms = request.now_ms};
    static_cast<void>(detail::insert_job_uncommitted(
        connection, job,
        detail::encode_vox_timeout_payload({.session_id = before->session_id,
                                            .expected_revision = new_revision},
                                           request.correlation_id,
                                           request.event_id)));
  } else {
    cancel_job(connection, before->timeout_job_id, request.now_ms);
  }
  auto update = connection.prepare(
      "UPDATE voice_session SET state_version=?,empty_since_ms=?,"
      "timeout_job_id=?,last_active_at_ms=max(last_active_at_ms,?) WHERE "
      "session_id=? AND state_version=?");
  update.bind(1, static_cast<std::int64_t>(new_revision));
  if (becoming_empty)
    update.bind(2, request.now_ms);
  else
    update.bind_null(2);
  if (becoming_empty)
    update.bind(3, *request.empty_job_id);
  else
    update.bind_null(3);
  update.bind(4, request.now_ms);
  update.bind(5, before->session_id);
  update.bind(6, static_cast<std::int64_t>(before->revision));
  update.execute();
  const VoxTransitionRequest transition{
      .session_id = before->session_id,
      .expected_revision = before->revision,
      .target = before->state,
      .reason = becoming_empty ? "channel_empty" : "channel_occupied",
      .actor_user_id = std::nullopt,
      .event_id = request.event_id,
      .idempotency_key = request.idempotency_key,
      .correlation_id = request.correlation_id,
      .now_ms = request.now_ms,
      .timeout_job_id = std::nullopt,
      .timeout_due_at_ms = std::nullopt,
      .failure_category = std::nullopt,
      .public_card = false};
  insert_transition(connection, *before, transition, new_revision);
  auto result = make_result(VoxResultCode::accepted,
                            load_session(connection, before->session_id), {},
                            becoming_empty);
  transaction.commit();
  return result;
}

VoxCommandResult SqliteVoxRepository::handle_timeout(
    const ClaimedScheduledJob &job, const std::int64_t now_ms,
    std::string event_id, std::string outbox_id, std::string fixture_event_id,
    const std::optional<std::size_t> observed_humans) {
  const auto *payload = std::get_if<VoxTimeoutJobPayload>(&job.payload);
  if (!payload) {
    std::scoped_lock lock{context_->mutex()};
    auto &connection = context_->connection();
    Transaction transaction{connection, TransactionMode::immediate};
    auto before = load_timeout_session(connection, job.job_id);
    if (!dead_letter_claimed_job_uncommitted(connection, job, now_ms,
                                             "payload_invalid")) {
      transaction.commit();
      return make_result(VoxResultCode::invalid_state, before);
    }
    if (!before) {
      transaction.commit();
      return make_result(VoxResultCode::invalid_state);
    }
    *before = fail_queued_fixture_uncommitted(
        connection, *before, fixture_event_id, job.correlation_id, now_ms);
    const auto target = before->state == VoxState::leaving ? VoxState::inactive
                                                           : VoxState::failed;
    const bool public_card = before->state != VoxState::connecting;
    auto result = apply_transition(
        connection,
        {.session_id = before->session_id,
         .expected_revision = before->revision,
         .target = target,
         .reason = "timeout_payload_invalid",
         .actor_user_id = std::nullopt,
         .event_id = std::move(event_id),
         .idempotency_key = "vox:timeout-payload-invalid:" + job.job_id,
         .correlation_id = job.correlation_id,
         .now_ms = now_ms,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = "payload_invalid",
         .public_card = public_card},
        public_card ? std::optional{std::move(outbox_id)} : std::nullopt);
    transaction.commit();
    return result;
  }
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto before = load_session(connection, payload->session_id);
  const auto linked_job = before && job.job_type == vox_mute_expiry_job_type
                              ? before->mute_job_id
                          : before ? before->timeout_job_id
                                   : std::nullopt;
  if (!before || linked_job != job.job_id) {
    complete_claimed_job(connection, job, now_ms);
    transaction.commit();
    return make_result(VoxResultCode::replay, before);
  }
  if (job.job_type == vox_empty_timeout_job_type && observed_humans &&
      *observed_humans > 0) {
    const auto new_revision = before->revision + 1;
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        {.event_id = event_id,
         .event_type = "vox.empty_timeout_cancelled.v1",
         .aggregate_type = "voice_session",
         .aggregate_id = before->session_id,
         .actor_user_id = std::nullopt,
         .guild_id = before->guild_id,
         .channel_id = before->text_channel_id,
         .source_message_id = std::nullopt,
         .occurred_at_ms = now_ms,
         .recorded_at_ms = now_ms,
         .correlation_id = job.correlation_id,
         .causation_id = job.causation_event_id,
         .idempotency_key = "vox:empty-recheck:" + job.job_id,
         .payload_json = Json{{"human_count", *observed_humans}}.dump()}));
    auto update = connection.prepare(
        "UPDATE voice_session SET state_version=?,empty_since_ms=NULL,"
        "timeout_job_id=NULL,last_active_at_ms=max(last_active_at_ms,?) WHERE "
        "session_id=? AND state_version=?");
    update.bind(1, static_cast<std::int64_t>(new_revision));
    update.bind(2, now_ms);
    update.bind(3, before->session_id);
    update.bind(4, static_cast<std::int64_t>(before->revision));
    update.execute();
    insert_transition(
        connection, *before,
        {.session_id = before->session_id,
         .expected_revision = before->revision,
         .target = before->state,
         .reason = "empty_timeout_cancelled",
         .actor_user_id = std::nullopt,
         .event_id = event_id,
         .idempotency_key = "vox:empty-recheck-transition:" + job.job_id,
         .correlation_id = job.correlation_id,
         .now_ms = now_ms,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = std::nullopt,
         .public_card = false},
        new_revision);
    complete_claimed_job(connection, job, now_ms);
    auto result = make_result(VoxResultCode::replay,
                              load_session(connection, before->session_id));
    transaction.commit();
    return result;
  }
  if (job.job_type == vox_mute_expiry_job_type &&
      before->state == VoxState::reconnecting) {
    const auto new_revision = before->revision + 1;
    static_cast<void>(detail::insert_event_uncommitted(
        connection, {.event_id = event_id,
                     .event_type = "vox.session_unmuted.v1",
                     .aggregate_type = "voice_session",
                     .aggregate_id = before->session_id,
                     .actor_user_id = std::nullopt,
                     .guild_id = before->guild_id,
                     .channel_id = before->text_channel_id,
                     .source_message_id = std::nullopt,
                     .occurred_at_ms = now_ms,
                     .recorded_at_ms = now_ms,
                     .correlation_id = job.correlation_id,
                     .causation_id = job.causation_event_id,
                     .idempotency_key = "vox:mute-expired:" + job.job_id,
                     .payload_json = Json{{"session_id", before->session_id},
                                          {"state", "reconnecting"}}
                                         .dump()}));
    auto update = connection.prepare(
        "UPDATE voice_session SET state_version=?,muted_at_ms=NULL,"
        "mute_until_ms=NULL,mute_job_id=NULL,last_active_at_ms=max("
        "last_active_at_ms,?) WHERE session_id=? AND state_version=?");
    update.bind(1, static_cast<std::int64_t>(new_revision));
    update.bind(2, now_ms);
    update.bind(3, before->session_id);
    update.bind(4, static_cast<std::int64_t>(before->revision));
    update.execute();
    const VoxTransitionRequest transition{
        .session_id = before->session_id,
        .expected_revision = before->revision,
        .target = VoxState::reconnecting,
        .reason = "mute_expired",
        .actor_user_id = std::nullopt,
        .event_id = event_id,
        .idempotency_key = "vox:mute-expired-transition:" + job.job_id,
        .correlation_id = job.correlation_id,
        .now_ms = now_ms,
        .timeout_job_id = std::nullopt,
        .timeout_due_at_ms = std::nullopt,
        .failure_category = std::nullopt,
        .public_card = false};
    insert_transition(connection, *before, transition, new_revision);
    complete_claimed_job(connection, job, now_ms);
    auto result = make_result(VoxResultCode::accepted,
                              load_session(connection, before->session_id));
    transaction.commit();
    return result;
  }
  VoxState target = VoxState::failed;
  std::string reason = "connect_timeout";
  bool public_card = false;
  if (job.job_type == vox_empty_timeout_job_type) {
    target = VoxState::inactive;
    reason = "empty_timeout";
    public_card = true;
  } else if (job.job_type == vox_leave_timeout_job_type) {
    target = VoxState::inactive;
    reason = "leave_timeout";
    public_card = true;
  } else if (job.job_type == vox_reconnect_timeout_job_type) {
    reason = "reconnect_timeout";
    public_card = true;
  } else if (job.job_type == vox_mute_expiry_job_type) {
    target = VoxState::ready;
    reason = "mute_expired";
  }
  if (job.job_type != vox_mute_expiry_job_type)
    *before = fail_queued_fixture_uncommitted(
        connection, *before, fixture_event_id, job.correlation_id, now_ms);
  complete_claimed_job(connection, job, now_ms);
  auto result = apply_transition(
      connection,
      {.session_id = before->session_id,
       .expected_revision = before->revision,
       .target = target,
       .reason = reason,
       .actor_user_id = std::nullopt,
       .event_id = std::move(event_id),
       .idempotency_key = "vox:timeout:" + job.job_id,
       .correlation_id = job.correlation_id,
       .now_ms = now_ms,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = target == VoxState::failed
                               ? std::optional<std::string>{reason}
                               : std::nullopt,
       .public_card = public_card},
      public_card ? std::optional{std::move(outbox_id)} : std::nullopt);
  transaction.commit();
  return result;
}

WorkMutationStatus SqliteVoxRepository::fail_timeout_job(
    const ClaimedScheduledJob &job, const std::int64_t now_ms,
    const std::int64_t retry_at_ms, std::string error_code,
    const bool retryable) {
  return SqliteDurableWorkRepository{context_}.fail_job(
      job, now_ms, retry_at_ms, std::move(error_code), retryable);
}

std::optional<VoxSession> SqliteVoxRepository::active() {
  std::scoped_lock lock{context_->mutex()};
  return load_active(context_->connection());
}

std::size_t SqliteVoxRepository::recover(const std::string_view instance_id,
                                         const std::int64_t now_ms,
                                         std::string event_id,
                                         std::string fixture_event_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto before = load_active(connection);
  if (!before || before->deployment_instance_id == instance_id) {
    transaction.commit();
    return 0;
  }
  const auto reason = before->state == VoxState::leaving ? "restart_cleanup"
                                                         : "restart_abandoned";
  const auto initial_summon_key =
      load_initial_summon_key(connection, before->session_id);
  cancel_unsent_public_cards(connection, before->session_id, now_ms);
  *before = fail_queued_fixture_uncommitted(
      connection, *before, fixture_event_id, "vox.startup.recovery", now_ms);
  auto result = apply_transition(
      connection,
      {.session_id = before->session_id,
       .expected_revision = before->revision,
       .target = VoxState::inactive,
       .reason = reason,
       .actor_user_id = std::nullopt,
       .event_id = std::move(event_id),
       .idempotency_key = "vox:recovery:" + before->session_id + ":" +
                          std::string{instance_id},
       .correlation_id = "vox.startup.recovery",
       .now_ms = now_ms,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = std::nullopt,
       .public_card = false},
      std::nullopt);
  if (result.code == VoxResultCode::accepted && result.session &&
      initial_summon_key) {
    VoxCommandContext summon_context{.guild_id = before->guild_id,
                                     .text_channel_id = before->text_channel_id,
                                     .actor_user_id = before->summoner_user_id,
                                     .owner_user_id = {},
                                     .interaction_idempotency_key =
                                         *initial_summon_key,
                                     .correlation_id = "vox.startup.recovery",
                                     .now_ms = now_ms};
    if (!load_receipt(connection, summon_context, "summon")) {
      auto interrupted =
          make_result(VoxResultCode::unavailable, result.session,
                      "The prior Vox summon was interrupted by a restart.");
      insert_receipt(connection, summon_context, "summon", interrupted);
    }
  }
  transaction.commit();
  return result.code == VoxResultCode::accepted ? 1 : 0;
}

VoxCommandResult SqliteVoxRepository::shutdown(
    const std::int64_t now_ms, std::string event_id,
    std::string fixture_event_id,
    std::optional<std::string> queued_fixture_failure_category) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto before = load_active(connection);
  if (!before) {
    transaction.commit();
    return make_result(VoxResultCode::inactive);
  }
  const auto fixture_failure_category =
      queued_fixture_failure_category.value_or("playback_interrupted");
  before = fail_queued_fixture_uncommitted(connection, *before,
                                           fixture_event_id, "vox.shutdown",
                                           now_ms, fixture_failure_category);
  cancel_unsent_public_cards(connection, before->session_id, now_ms);
  auto result =
      apply_transition(connection,
                       {.session_id = before->session_id,
                        .expected_revision = before->revision,
                        .target = VoxState::inactive,
                        .reason = "shutdown",
                        .actor_user_id = std::nullopt,
                        .event_id = std::move(event_id),
                        .idempotency_key = "vox:shutdown:" + before->session_id,
                        .correlation_id = "vox.shutdown",
                        .now_ms = now_ms,
                        .timeout_job_id = std::nullopt,
                        .timeout_due_at_ms = std::nullopt,
                        .failure_category = std::nullopt,
                        .public_card = false},
                       std::nullopt);
  transaction.commit();
  return result;
}

} // namespace sanguinius::persistence
