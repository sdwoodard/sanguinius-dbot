#include "sanguinius/persistence/sqlite_chronicle_session_repository.hpp"

#include "sqlite_durable_work_writes.hpp"
#include "sqlite_relationship_writes.hpp"

#include "sanguinius/chronicle.hpp"
#include "sanguinius/pending_notice.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

[[nodiscard]] SessionMutationResult
session_result(const ChronicleSessionResultCode code) {
  return {.code = code,
          .session = std::nullopt,
          .wake_scheduler = false,
          .wake_outbox = false};
}

[[nodiscard]] TitleMutationResult
title_result(const ChronicleSessionResultCode code) {
  return {.code = code, .grant = std::nullopt, .wake_outbox = false};
}

[[nodiscard]] AnniversaryScanResult
anniversary_result(const WorkMutationStatus status) {
  return {
      .status = status, .wake_outbox = false, .next_due_at_ms = std::nullopt};
}

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

void require_uuid(const std::string_view value) {
  if (!valid_uuid_v4(std::string{value}))
    throw std::invalid_argument{"Invalid Chronicle session ID."};
}

void require_key(const std::string_view value) {
  if (value.empty() || value.size() > 160)
    throw std::invalid_argument{"Invalid Chronicle session idempotency key."};
}

void require_context(const DiscordSnowflake guild,
                     const DiscordSnowflake channel,
                     const DiscordSnowflake actor, const std::int64_t now_ms) {
  if (!guild.is_set() || !channel.is_set() || !actor.is_set() || now_ms < 0)
    throw std::invalid_argument{"Invalid Chronicle session context."};
}

[[nodiscard]] ChronicleSessionState session_state(std::string_view value) {
  if (value == "open")
    return ChronicleSessionState::open;
  if (value == "closing")
    return ChronicleSessionState::closing;
  if (value == "closed")
    return ChronicleSessionState::closed;
  if (value == "abandoned")
    return ChronicleSessionState::abandoned;
  throw std::runtime_error{"Invalid stored Chronicle session state."};
}

[[nodiscard]] ChronicleSummaryState summary_state(std::string_view value) {
  if (value == "pending")
    return ChronicleSummaryState::pending;
  if (value == "approved")
    return ChronicleSummaryState::approved;
  if (value == "rejected")
    return ChronicleSummaryState::rejected;
  throw std::runtime_error{"Invalid stored Chronicle summary state."};
}

[[nodiscard]] ChronicleTitleState title_state(std::string_view value) {
  if (value == "proposed")
    return ChronicleTitleState::proposed;
  if (value == "active")
    return ChronicleTitleState::active;
  if (value == "rejected")
    return ChronicleTitleState::rejected;
  if (value == "revoked")
    return ChronicleTitleState::revoked;
  throw std::runtime_error{"Invalid stored Chronicle title state."};
}

[[nodiscard]] ChronicleSession read_session(SqliteConnection &connection,
                                            SqliteStatement &row) {
  ChronicleSession result{
      .session_id = row.column_text(0),
      .guild_id = DiscordSnowflake::parse(row.column_text(1)),
      .channel_id = DiscordSnowflake::parse(row.column_text(2)),
      .opened_by_user_id = DiscordSnowflake::parse(row.column_text(3)),
      .state = session_state(row.column_text(4)),
      .opened_at_ms = row.column_int64(5),
      .closing_at_ms = optional_integer(row, 6),
      .closed_at_ms = optional_integer(row, 7),
      .revision = static_cast<std::size_t>(row.column_int64(8)),
      .participants = {},
      .linked_shared_canon_entries = 0,
      .draft_id = optional_text(row, 9),
      .draft_state = row.column_is_null(10)
                         ? std::nullopt
                         : std::optional{summary_state(row.column_text(10))},
      .draft_revision =
          row.column_is_null(11)
              ? std::nullopt
              : std::optional{static_cast<std::size_t>(row.column_int64(11))},
  };
  auto participants = connection.prepare(
      "SELECT user_id FROM chronicle_session_participant WHERE session_id=? "
      "ORDER BY user_id");
  participants.bind(1, result.session_id);
  while (participants.step())
    result.participants.push_back(
        DiscordSnowflake::parse(participants.column_text(0)));
  auto count = connection.prepare(
      "SELECT count(*) FROM chronicle_session_entry se "
      "JOIN chronicle_entry e ON e.entry_id=se.entry_id "
      "WHERE se.session_id=? AND e.status='canon' AND e.visibility='shared'");
  count.bind(1, result.session_id);
  if (!count.step())
    throw std::runtime_error{"Chronicle session count query failed."};
  result.linked_shared_canon_entries =
      static_cast<std::size_t>(count.column_int64(0));
  return result;
}

[[nodiscard]] std::optional<ChronicleSession>
load_session(SqliteConnection &connection, std::string_view id) {
  auto row = connection.prepare(
      "SELECT s.session_id,s.guild_id,s.channel_id,s.opened_by_user_id,s.state,"
      "s.opened_at_ms,s.closing_at_ms,s.closed_at_ms,s.revision,d.draft_id,"
      "d.state,d.revision FROM chronicle_session s LEFT JOIN "
      "chronicle_summary_draft d "
      "ON d.session_id=s.session_id WHERE s.session_id=?");
  row.bind(1, id);
  if (!row.step())
    return std::nullopt;
  return read_session(connection, row);
}

[[nodiscard]] std::optional<ChronicleSession>
load_latest_session(SqliteConnection &connection,
                    const DiscordSnowflake guild) {
  auto row = connection.prepare(
      "SELECT s.session_id,s.guild_id,s.channel_id,s.opened_by_user_id,s.state,"
      "s.opened_at_ms,s.closing_at_ms,s.closed_at_ms,s.revision,d.draft_id,"
      "d.state,d.revision FROM chronicle_session s LEFT JOIN "
      "chronicle_summary_draft d "
      "ON d.session_id=s.session_id WHERE s.guild_id=? "
      "ORDER BY CASE WHEN s.state IN ('open','closing') THEN 0 ELSE 1 END,"
      "s.opened_at_ms DESC,s.session_id DESC LIMIT 1");
  row.bind(1, guild.str());
  if (!row.step())
    return std::nullopt;
  return read_session(connection, row);
}

[[nodiscard]] bool opted_in(SqliteConnection &connection,
                            const DiscordSnowflake user) {
  auto query = connection.prepare(
      "SELECT chronicle_opt_in FROM user_preference WHERE user_id=?");
  query.bind(1, user.str());
  return query.step() && query.column_int64(0) == 1;
}

[[nodiscard]] EventJournalEntry make_event(
    std::string id, std::string type, std::string aggregate_type,
    std::string aggregate_id, const std::optional<DiscordSnowflake> actor,
    const DiscordSnowflake guild, const DiscordSnowflake channel,
    const std::int64_t now_ms, std::string correlation, std::string idempotency,
    Json payload, std::optional<std::string> causation = std::nullopt) {
  return EventJournalEntry{
      .event_id = std::move(id),
      .event_type = std::move(type),
      .aggregate_type = std::move(aggregate_type),
      .aggregate_id = std::move(aggregate_id),
      .actor_user_id = actor,
      .guild_id = guild,
      .channel_id = channel,
      .source_message_id = std::nullopt,
      .occurred_at_ms = now_ms,
      .recorded_at_ms = now_ms,
      .correlation_id = std::move(correlation),
      .causation_id = std::move(causation),
      .idempotency_key = std::move(idempotency),
      .payload_json = payload.dump(),
  };
}

void complete_claim(SqliteConnection &connection,
                    const ClaimedScheduledJob &job, const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state='completed',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,completed_at_ms=max(?,created_at_"
      "ms),"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms),"
      "last_error_code=NULL "
      "WHERE job_id=? AND state='claimed' AND lease_owner=? AND lease_token=?");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, now_ms);
  update.bind(4, job.job_id);
  update.bind(5, job.lease_owner);
  update.bind(6, job.lease_token);
  update.execute();
}

[[nodiscard]] WorkMutationStatus claim_status(SqliteConnection &connection,
                                              const ClaimedScheduledJob &job) {
  auto query =
      connection.prepare("SELECT state FROM scheduled_job WHERE job_id=?");
  query.bind(1, job.job_id);
  if (!query.step())
    return WorkMutationStatus::not_found;
  return query.column_text(0) == "completed" ? WorkMutationStatus::unchanged
                                             : WorkMutationStatus::stale_claim;
}

void insert_public_outbox(
    SqliteConnection &connection, const std::string &outbox_id,
    const DiscordSnowflake guild, const DiscordSnowflake channel,
    std::string aggregate_type, const std::string &aggregate_id,
    const std::string &idempotency_key, const std::int64_t now_ms,
    const std::string &correlation_id,
    const std::optional<std::string> &causation, InteractionMessage message) {
  const PublicOutboxPayload payload{
      .request = PublicMessageRequest{.guild_id = guild,
                                      .channel_id = channel,
                                      .message = std::move(message)}};
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = std::move(aggregate_type),
      .aggregate_id = aggregate_id,
      .target_guild_id = guild,
      .target_channel_id = channel,
      .target_user_id = std::nullopt,
      .available_at_ms = now_ms,
      .max_attempts = 5,
      .idempotency_key = idempotency_key,
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = now_ms,
  };
  if (!detail::insert_outbox_uncommitted(
          connection, outbox,
          detail::encode_public_payload(payload, correlation_id, causation)))
    throw std::runtime_error{"Chronicle session outbox idempotency conflict."};
}

struct SummaryReviewDeliveryIds {
  std::string_view notice_id;
  std::string_view notice_token_id;
  std::string_view edit_token_id;
  std::string_view approve_token_id;
  std::string_view reject_token_id;
  std::string_view notice_outbox_id;
};

void cancel_summary_review_delivery(
    SqliteConnection &connection, const std::string_view session_id,
    const std::string_view draft_id,
    const std::optional<std::string> &notice_id, const std::int64_t now_ms) {
  auto cancel_controls = connection.prepare(
      "UPDATE interaction_token SET state='cancelled' WHERE "
      "entity_type='chronicle_summary_draft' AND entity_id=? AND "
      "state='active'");
  cancel_controls.bind(1, draft_id);
  cancel_controls.execute();

  if (notice_id) {
    auto cancel_notice_controls = connection.prepare(
        "UPDATE interaction_token SET state='cancelled' WHERE "
        "entity_type='pending_notice' AND entity_id=? AND state='active'");
    cancel_notice_controls.bind(1, *notice_id);
    cancel_notice_controls.execute();

    auto cancel_notice = connection.prepare(
        "UPDATE pending_notice SET state='cancelled' WHERE notice_id=? AND "
        "state IN ('pending','opened')");
    cancel_notice.bind(1, *notice_id);
    cancel_notice.execute();

    auto cancel_public = connection.prepare(
        "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
        "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms) "
        "WHERE kind=? AND aggregate_type='pending_notice' AND aggregate_id=? "
        "AND (state IN ('pending','failed') OR (state='claimed' AND "
        "submission_started_at_ms IS NULL))");
    cancel_public.bind(1, now_ms);
    cancel_public.bind(2, now_ms);
    cancel_public.bind(3, std::string{public_discord_outbox_kind});
    cancel_public.bind(4, *notice_id);
    cancel_public.execute();
  }

  auto cancel_materialization = connection.prepare(
      "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,submission_started_at_ms=NULL,"
      "terminal_at_ms=max(?,created_at_ms),updated_at_ms=max(?,updated_at_ms) "
      "WHERE kind=? AND aggregate_type='chronicle_session' AND aggregate_id=? "
      "AND state IN ('pending','claimed','failed')");
  cancel_materialization.bind(1, now_ms);
  cancel_materialization.bind(2, now_ms);
  cancel_materialization.bind(3, std::string{pending_notice_outbox_kind});
  cancel_materialization.bind(4, session_id);
  cancel_materialization.execute();
}

void enqueue_summary_review(
    SqliteConnection &connection, const SummaryReviewDeliveryIds &ids,
    const std::string_view session_id, const std::string_view draft_id,
    const DiscordSnowflake guild, const DiscordSnowflake channel,
    const DiscordSnowflake owner, const std::string_view chapter_title,
    const std::string_view summary, const std::size_t draft_revision,
    const std::int64_t now_ms, const std::string_view correlation_id,
    const std::optional<std::string> &causation_event_id,
    const bool announce_publicly) {
  constexpr std::int64_t review_lifetime = 7LL * 24 * 60 * 60 * 1'000;
  const auto action_id = [](const std::string_view id) {
    return std::string{chronicle_session_component_prefix} + std::string{id};
  };
  const auto edit_action_id = [](const std::string_view id) {
    return std::string{chronicle_session_edit_prefix} + std::string{id};
  };
  const auto revision_suffix = ":" + std::to_string(draft_revision);
  const NoticeOutboxPayload notice{
      .notice =
          CreatePendingNoticeRequest{
              .notice_id = std::string{ids.notice_id},
              .token_id = std::string{ids.notice_token_id},
              .target_user_id = owner,
              .guild_id = guild,
              .channel_id = channel,
              .notice_type = "chronicle.session_review",
              .content =
                  PendingNoticeContent{
                      .title = "Chronicle chapter awaits review",
                      .body = "**" + std::string{chapter_title} + "**\n" +
                              std::string{summary} + "\nReference: `" +
                              std::string{draft_id} + "`, revision " +
                              std::to_string(draft_revision) + ".",
                      .actions =
                          {{edit_action_id(ids.edit_token_id), "Edit"},
                           {action_id(ids.approve_token_id), "Approve"},
                           {action_id(ids.reject_token_id), "Reject"}},
                  },
              .source_aggregate_type = "chronicle_session",
              .source_aggregate_id = std::string{session_id},
              .expires_at_ms = now_ms + review_lifetime,
              .notice_idempotency_key =
                  "notice:session-review:" + std::string{session_id} +
                  revision_suffix,
              .token_idempotency_key =
                  "token:session-review-open:" + std::string{session_id} +
                  revision_suffix,
              .created_at_ms = now_ms,
          },
      .announce_publicly = announce_publicly,
  };
  const OutboxEnqueue outbox{
      .outbox_id = std::string{ids.notice_outbox_id},
      .kind = std::string{pending_notice_outbox_kind},
      .aggregate_type = "chronicle_session",
      .aggregate_id = std::string{session_id},
      .target_guild_id = guild,
      .target_channel_id = channel,
      .target_user_id = owner,
      .available_at_ms = now_ms,
      .max_attempts = 5,
      .idempotency_key = "outbox:session-review:" + std::string{session_id} +
                         revision_suffix,
      .provider_nonce =
          discord_nonce_from_uuid(std::string{ids.notice_outbox_id}),
      .created_at_ms = now_ms,
  };
  if (!detail::insert_outbox_uncommitted(
          connection, outbox,
          detail::encode_notice_payload(notice, correlation_id,
                                        causation_event_id)))
    throw std::runtime_error{"Chronicle summary notice conflict."};

  const auto insert_action_token =
      [&](const std::string_view token_id, const std::string_view kind,
          const std::string_view action) {
        auto token = connection.prepare(
            "INSERT INTO interaction_token(token_id,token_version,"
            "interaction_kind,action,entity_type,entity_id,expected_user_id,"
            "guild_id,channel_id,state,expires_at_ms,idempotency_key,"
            "created_at_ms,expected_entity_revision) "
            "VALUES (?,1,?,?,?,?,?,?,?,'active',?,?,?,?)");
        token.bind(1, token_id);
        token.bind(2, kind);
        token.bind(3, action);
        token.bind(4, "chronicle_summary_draft");
        token.bind(5, draft_id);
        token.bind(6, owner.str());
        token.bind(7, guild.str());
        token.bind(8, channel.str());
        token.bind(9, now_ms + review_lifetime);
        token.bind(10, "token:" + std::string{action} + ":" +
                           std::string{session_id} + revision_suffix);
        token.bind(11, now_ms);
        token.bind(12, static_cast<std::int64_t>(draft_revision));
        token.execute();
      };
  insert_action_token(ids.edit_token_id, "modal", "chronicle.summary.edit");
  insert_action_token(ids.approve_token_id, "button",
                      "chronicle.summary.approve");
  insert_action_token(ids.reject_token_id, "button",
                      "chronicle.summary.reject");

  auto notice_ref = connection.prepare(
      "UPDATE chronicle_summary_draft SET review_notice_id=? WHERE draft_id=? "
      "AND state='pending' AND revision=?");
  notice_ref.bind(1, ids.notice_id);
  notice_ref.bind(2, draft_id);
  notice_ref.bind(3, static_cast<std::int64_t>(draft_revision));
  notice_ref.execute();
  if (connection.changes() != 1)
    throw std::runtime_error{"Chronicle summary review lost its fence."};
}

[[nodiscard]] std::string excerpt(std::string_view value,
                                  const std::size_t limit = 180) {
  if (value.size() <= limit)
    return std::string{value};
  auto end = limit - 3;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U)
    --end;
  return std::string{value.substr(0, end)} + "...";
}

[[nodiscard]] ChronicleSummaryValidationContext
load_summary_context(SqliteConnection &connection,
                     const std::string_view session_id) {
  ChronicleSummaryValidationContext result;
  auto users = connection.prepare(
      "SELECT sp.user_id FROM chronicle_session_participant sp JOIN "
      "user_preference p ON p.user_id=sp.user_id WHERE sp.session_id=? "
      "AND p.chronicle_opt_in=1 ORDER BY sp.user_id");
  users.bind(1, session_id);
  while (users.step())
    result.opted_in_participants.push_back(
        DiscordSnowflake::parse(users.column_text(0)));
  auto entries = connection.prepare(
      "SELECT e.entry_id,e.title,e.body FROM chronicle_session_entry se JOIN "
      "chronicle_entry e ON e.entry_id=se.entry_id WHERE se.session_id=? AND "
      "e.status='canon' AND e.visibility='shared' ORDER BY e.occurred_at_ms "
      "DESC,e.entry_id DESC LIMIT ?");
  entries.bind(1, session_id);
  entries.bind(2, static_cast<std::int64_t>(maximum_session_linked_entries));
  std::size_t shared_input_bytes{};
  while (entries.step()) {
    auto rendered = "[" + entries.column_text(0) + "] " +
                    entries.column_text(1) + " — " + entries.column_text(2);
    if (shared_input_bytes + rendered.size() >
        maximum_session_summary_input_bytes) {
      break;
    }
    shared_input_bytes += rendered.size();
    result.shared_entry_ids.push_back(entries.column_text(0));
    result.shared_entry_context.push_back(std::move(rendered));
  }
  std::ranges::reverse(result.shared_entry_ids);
  std::ranges::reverse(result.shared_entry_context);
  auto context =
      connection.prepare("SELECT c.excerpt FROM chronicle_session_context c "
                         "JOIN user_preference p "
                         "ON p.user_id=c.author_user_id WHERE c.session_id=? "
                         "AND p.chronicle_opt_in=1 "
                         "ORDER BY observed_at_ms,context_id");
  context.bind(1, session_id);
  while (context.step())
    result.transient_context.push_back(context.column_text(0));
  return result;
}

} // namespace

SqliteChronicleSessionRepository::SqliteChronicleSessionRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite context is required."};
}

SessionMutationResult
SqliteChronicleSessionRepository::start(const StartSessionRequest &request) {
  require_uuid(request.session_id);
  require_uuid(request.event_id);
  require_key(request.idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.now_ms);
  std::unique_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  auto replay = connection.prepare(
      "SELECT session_id FROM chronicle_session WHERE start_idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    auto result = load_session(connection, replay.column_text(0));
    transaction.commit();
    return {.code = ChronicleSessionResultCode::existing,
            .session = std::move(result)};
  }
  if (!opted_in(connection, request.actor_user_id)) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::opted_out);
  }
  auto active = connection.prepare(
      "SELECT session_id FROM chronicle_session WHERE guild_id=? "
      "AND state IN ('open','closing')");
  active.bind(1, request.guild_id.str());
  if (active.step()) {
    auto result = load_session(connection, active.column_text(0));
    transaction.commit();
    return {.code = ChronicleSessionResultCode::existing,
            .session = std::move(result)};
  }
  auto insert = connection.prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES (?,?,?,?,'open',?,1,?)");
  insert.bind(1, request.session_id);
  insert.bind(2, request.guild_id.str());
  insert.bind(3, request.channel_id.str());
  insert.bind(4, request.actor_user_id.str());
  insert.bind(5, request.now_ms);
  insert.bind(6, request.idempotency_key);
  insert.execute();
  auto participant = connection.prepare(
      "INSERT INTO "
      "chronicle_session_participant(session_id,user_id,joined_at_ms) "
      "VALUES (?,?,?)");
  participant.bind(1, request.session_id);
  participant.bind(2, request.actor_user_id.str());
  participant.bind(3, request.now_ms);
  participant.execute();
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      make_event(request.event_id, "chronicle.session_started.v1",
                 "chronicle_session", request.session_id, request.actor_user_id,
                 request.guild_id, request.channel_id, request.now_ms,
                 request.correlation_id, "event:" + request.idempotency_key,
                 Json{{"revision", 1}})));
  auto result = load_session(connection, request.session_id);
  transaction.commit();
  return {.code = ChronicleSessionResultCode::created,
          .session = std::move(result)};
}

SessionMutationResult
SqliteChronicleSessionRepository::close(const CloseSessionRequest &request) {
  require_uuid(request.draft_id);
  require_uuid(request.summary_job_id);
  require_uuid(request.purge_job_id);
  require_uuid(request.event_id);
  require_key(request.idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  auto row = connection.prepare(
      "SELECT session_id,opened_by_user_id,revision,close_idempotency_key,"
      "opened_at_ms "
      "FROM chronicle_session WHERE guild_id=? AND state IN "
      "('open','closing')");
  row.bind(1, request.guild_id.str());
  if (!row.step()) {
    auto replay = connection.prepare("SELECT session_id FROM chronicle_session "
                                     "WHERE close_idempotency_key=?");
    replay.bind(1, request.idempotency_key);
    if (replay.step()) {
      auto result = load_session(connection, replay.column_text(0));
      transaction.commit();
      return {.code = ChronicleSessionResultCode::existing,
              .session = std::move(result)};
    }
    transaction.commit();
    return session_result(ChronicleSessionResultCode::not_found);
  }
  const auto session_id = row.column_text(0);
  const auto opener = DiscordSnowflake::parse(row.column_text(1));
  const auto revision = static_cast<std::size_t>(row.column_int64(2));
  const auto transition_at_ms = std::max(request.now_ms, row.column_int64(4));
  if (optional_text(row, 3)) {
    auto result = load_session(connection, session_id);
    transaction.commit();
    return {.code = ChronicleSessionResultCode::existing,
            .session = std::move(result)};
  }
  if (request.actor_user_id != opener &&
      request.actor_user_id != request.owner_user_id) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::unauthorized);
  }
  auto prune_expired_context = connection.prepare(
      "DELETE FROM chronicle_session_context WHERE session_id=? AND "
      "observed_at_ms<=?");
  prune_expired_context.bind(1, session_id);
  prune_expired_context.bind(2, request.now_ms - session_context_retention_ms);
  prune_expired_context.execute();
  auto count = connection.prepare(
      "SELECT count(*) FROM chronicle_session_entry se JOIN chronicle_entry e "
      "ON e.entry_id=se.entry_id WHERE se.session_id=? AND e.status='canon' "
      "AND e.visibility='shared'");
  count.bind(1, session_id);
  if (!count.step())
    throw std::runtime_error{"Chronicle session close count failed."};
  const auto entry_count = static_cast<std::size_t>(count.column_int64(0));
  const bool empty = entry_count == 0;
  auto update = connection.prepare(
      empty ? "UPDATE chronicle_session SET state='abandoned',closed_at_ms=?,"
              "revision=revision+1,close_idempotency_key=? WHERE session_id=? "
              "AND state='open' AND revision=?"
            : "UPDATE chronicle_session SET state='closing',closing_at_ms=?,"
              "revision=revision+1,close_idempotency_key=? WHERE session_id=? "
              "AND state='open' AND revision=?");
  update.bind(1, transition_at_ms);
  update.bind(2, request.idempotency_key);
  update.bind(3, session_id);
  update.bind(4, static_cast<std::int64_t>(revision));
  update.execute();
  if (connection.changes() != 1) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::stale_revision);
  }
  const auto event = make_event(
      request.event_id,
      empty ? "chronicle.session_abandoned.v1" : "chronicle.session_closing.v1",
      "chronicle_session", session_id, request.actor_user_id, request.guild_id,
      request.channel_id, transition_at_ms, request.correlation_id,
      "event:" + request.idempotency_key,
      Json{{"entry_count", entry_count}, {"revision", revision + 1}});
  if (!detail::insert_event_uncommitted(connection, event))
    throw std::runtime_error{"Chronicle close event conflict."};
  if (empty) {
    auto purge = connection.prepare(
        "DELETE FROM chronicle_session_context WHERE session_id=?");
    purge.bind(1, session_id);
    purge.execute();
    auto cancel_purge = connection.prepare(
        "UPDATE scheduled_job SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_at_ms=max(?,created_at_"
        "ms),"
        "updated_at_ms=max(?,updated_at_ms) WHERE job_type=? AND "
        "aggregate_id=? "
        "AND state='pending'");
    cancel_purge.bind(1, transition_at_ms);
    cancel_purge.bind(2, transition_at_ms);
    cancel_purge.bind(3, std::string{session_context_purge_job_type});
    cancel_purge.bind(4, session_id);
    cancel_purge.execute();
  } else {
    const auto fallback =
        deterministic_summary_fallback(session_id, entry_count);
    auto draft = connection.prepare(
        "INSERT INTO chronicle_summary_draft(draft_id,session_id,state,"
        "chapter_title,summary,source,revision,created_at_ms,updated_at_ms) "
        "VALUES (?,?,'pending',?,?,'fallback',1,?,?)");
    draft.bind(1, request.draft_id);
    draft.bind(2, session_id);
    draft.bind(3, fallback.chapter_title);
    draft.bind(4, fallback.summary);
    draft.bind(5, transition_at_ms);
    draft.bind(6, transition_at_ms);
    draft.execute();
    const ScheduledJobEnqueue summary_job{
        .job_id = request.summary_job_id,
        .job_type = std::string{session_summary_job_type},
        .aggregate_type = "chronicle_session",
        .aggregate_id = session_id,
        .due_at_ms = transition_at_ms,
        .max_attempts = 5,
        .idempotency_key = "job:summary:" + session_id,
        .created_at_ms = transition_at_ms,
    };
    if (!detail::insert_job_uncommitted(
            connection, summary_job,
            detail::encode_session_summary_payload(
                SessionSummaryJobPayload{.session_id = session_id,
                                         .draft_id = request.draft_id,
                                         .expected_session_revision =
                                             revision + 1,
                                         .expected_draft_revision = 1},
                request.correlation_id, request.event_id)))
      throw std::runtime_error{"Chronicle summary job conflict."};
    auto reschedule_purge = connection.prepare(
        "UPDATE scheduled_job SET "
        "due_at_ms=?,updated_at_ms=max(?,updated_at_ms),"
        "last_error_code=NULL WHERE job_type=? AND aggregate_id=? AND "
        "state='pending'");
    reschedule_purge.bind(1, transition_at_ms + session_context_retention_ms);
    reschedule_purge.bind(2, transition_at_ms);
    reschedule_purge.bind(3, std::string{session_context_purge_job_type});
    reschedule_purge.bind(4, session_id);
    reschedule_purge.execute();
    auto active_purge = connection.prepare(
        "SELECT 1 FROM scheduled_job WHERE job_type=? AND aggregate_id=? AND "
        "state IN ('pending','claimed')");
    active_purge.bind(1, std::string{session_context_purge_job_type});
    active_purge.bind(2, session_id);
    if (!active_purge.step()) {
      const ScheduledJobEnqueue purge_job{
          .job_id = request.purge_job_id,
          .job_type = std::string{session_context_purge_job_type},
          .aggregate_type = "chronicle_session",
          .aggregate_id = session_id,
          .due_at_ms = transition_at_ms + session_context_retention_ms,
          .max_attempts = 5,
          .idempotency_key = "job:context-purge:" + session_id,
          .created_at_ms = transition_at_ms,
      };
      if (!detail::insert_job_uncommitted(
              connection, purge_job,
              detail::encode_session_context_purge_payload(
                  SessionContextPurgeJobPayload{.session_id = session_id},
                  request.correlation_id, request.event_id)))
        throw std::runtime_error{"Chronicle context purge job conflict."};
    }
  }
  auto result = load_session(connection, session_id);
  transaction.commit();
  return {.code = ChronicleSessionResultCode::updated,
          .session = std::move(result),
          .wake_scheduler = !empty};
}

std::optional<ChronicleSession>
SqliteChronicleSessionRepository::status(const DiscordSnowflake &guild_id) {
  const std::scoped_lock lock{context_->mutex()};
  return load_latest_session(context_->connection(), guild_id);
}

bool SqliteChronicleSessionRepository::observe_context(
    const SessionContextObservation &observation) {
  require_uuid(observation.context_id);
  require_key(observation.correlation_id);
  require_context(observation.guild_id, observation.channel_id,
                  observation.author_user_id, observation.observed_at_ms);
  if (!observation.message_id.is_set() ||
      !valid_chronicle_text(observation.excerpt,
                            maximum_session_context_excerpt_bytes))
    throw std::invalid_argument{"Invalid Chronicle session context excerpt."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (!opted_in(connection, observation.author_user_id)) {
    transaction.commit();
    return false;
  }
  auto active = connection.prepare(
      "SELECT session_id FROM chronicle_session WHERE guild_id=? AND "
      "channel_id=? AND state='open'");
  active.bind(1, observation.guild_id.str());
  active.bind(2, observation.channel_id.str());
  if (!active.step()) {
    transaction.commit();
    return false;
  }
  const auto session_id = active.column_text(0);
  auto prune_age = connection.prepare(
      "DELETE FROM chronicle_session_context WHERE session_id=? AND "
      "observed_at_ms<=?");
  prune_age.bind(1, session_id);
  prune_age.bind(2, observation.observed_at_ms - session_context_retention_ms);
  prune_age.execute();
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO chronicle_session_context(context_id,session_id,"
      "message_id,author_user_id,excerpt,observed_at_ms) VALUES (?,?,?,?,?,?)");
  insert.bind(1, observation.context_id);
  insert.bind(2, session_id);
  insert.bind(3, observation.message_id.str());
  insert.bind(4, observation.author_user_id.str());
  insert.bind(5, observation.excerpt);
  insert.bind(6, observation.observed_at_ms);
  insert.execute();
  const bool created = connection.changes() == 1;
  if (created) {
    auto participant = connection.prepare(
        "INSERT OR IGNORE INTO "
        "chronicle_session_participant(session_id,user_id,"
        "joined_at_ms) SELECT session_id,?,? FROM chronicle_session "
        "WHERE guild_id=? AND state='open'");
    participant.bind(1, observation.author_user_id.str());
    participant.bind(2, observation.observed_at_ms);
    participant.bind(3, observation.guild_id.str());
    participant.execute();
    auto prune_rows = connection.prepare(
        "DELETE FROM chronicle_session_context WHERE context_id IN ("
        "SELECT context_id FROM chronicle_session_context WHERE session_id IN "
        "(SELECT session_id FROM chronicle_session WHERE guild_id=? AND "
        "state='open') "
        "ORDER BY observed_at_ms DESC,context_id DESC LIMIT -1 OFFSET ?)");
    prune_rows.bind(1, observation.guild_id.str());
    prune_rows.bind(2, static_cast<std::int64_t>(maximum_session_context_rows));
    prune_rows.execute();
    for (;;) {
      auto total = connection.prepare(
          "SELECT coalesce(sum(length(CAST(excerpt AS BLOB))),0) FROM "
          "chronicle_session_context WHERE session_id IN (SELECT session_id "
          "FROM chronicle_session WHERE guild_id=? AND state='open')");
      total.bind(1, observation.guild_id.str());
      if (!total.step() ||
          total.column_int64(0) <=
              static_cast<std::int64_t>(maximum_session_context_total_bytes))
        break;
      auto oldest = connection.prepare(
          "DELETE FROM chronicle_session_context WHERE context_id=(SELECT "
          "context_id FROM chronicle_session_context WHERE session_id IN "
          "(SELECT session_id FROM chronicle_session WHERE guild_id=? AND "
          "state='open') "
          "ORDER BY observed_at_ms,context_id LIMIT 1)");
      oldest.bind(1, observation.guild_id.str());
      oldest.execute();
    }
    auto purge_job = connection.prepare(
        "SELECT 1 FROM scheduled_job WHERE job_type=? AND aggregate_id=? AND "
        "state IN ('pending','claimed')");
    purge_job.bind(1, std::string{session_context_purge_job_type});
    purge_job.bind(2, session_id);
    if (!purge_job.step()) {
      const ScheduledJobEnqueue job{
          .job_id = observation.context_id,
          .job_type = std::string{session_context_purge_job_type},
          .aggregate_type = "chronicle_session",
          .aggregate_id = session_id,
          .due_at_ms =
              observation.observed_at_ms + session_context_retention_ms,
          .max_attempts = 5,
          .idempotency_key = "job:context-open-purge:" + session_id + ":" +
                             observation.context_id,
          .created_at_ms = observation.observed_at_ms,
      };
      if (!detail::insert_job_uncommitted(
              connection, job,
              detail::encode_session_context_purge_payload(
                  SessionContextPurgeJobPayload{.session_id = session_id},
                  observation.correlation_id, std::nullopt)))
        throw std::runtime_error{"Chronicle open-context purge job conflict."};
    }
  }
  transaction.commit();
  return created;
}

ChronicleSummaryValidationContext
SqliteChronicleSessionRepository::summary_context(std::string_view session_id) {
  require_uuid(session_id);
  const std::scoped_lock lock{context_->mutex()};
  return load_summary_context(context_->connection(), session_id);
}

WorkMutationStatus SqliteChronicleSessionRepository::complete_summary_job(
    const SummaryJobCompletionRequest &request) {
  const auto *payload =
      std::get_if<SessionSummaryJobPayload>(&request.job.payload);
  if (payload == nullptr)
    return WorkMutationStatus::invalid_state;
  require_uuid(payload->session_id);
  require_uuid(payload->draft_id);
  require_uuid(request.event_id);
  require_uuid(request.notice_id);
  require_uuid(request.notice_token_id);
  require_uuid(request.edit_token_id);
  require_uuid(request.approve_token_id);
  require_uuid(request.reject_token_id);
  require_uuid(request.notice_outbox_id);
  std::unordered_set<std::uint64_t> relationship_users;
  for (const auto &ids : request.relationship_event_ids) {
    require_uuid(ids.relationship_event_id);
    if (!ids.participant_user_id.is_set() ||
        !relationship_users.insert(ids.participant_user_id.value()).second)
      throw std::invalid_argument{"Invalid session relationship event IDs."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  auto claim = connection.prepare(
      "SELECT state FROM scheduled_job WHERE job_id=? AND state='claimed' "
      "AND lease_owner=? AND lease_token=?");
  claim.bind(1, request.job.job_id);
  claim.bind(2, request.job.lease_owner);
  claim.bind(3, request.job.lease_token);
  if (!claim.step()) {
    const auto result = claim_status(connection, request.job);
    transaction.commit();
    return result;
  }
  auto session = connection.prepare(
      "SELECT guild_id,channel_id,state,revision,opened_at_ms,closing_at_ms "
      "FROM chronicle_session WHERE session_id=?");
  session.bind(1, payload->session_id);
  if (!session.step() || session.column_text(2) != "closing" ||
      static_cast<std::size_t>(session.column_int64(3)) !=
          payload->expected_session_revision) {
    complete_claim(connection, request.job, request.now_ms);
    transaction.commit();
    return WorkMutationStatus::unchanged;
  }
  const auto guild = DiscordSnowflake::parse(session.column_text(0));
  const auto channel = DiscordSnowflake::parse(session.column_text(1));
  auto draft = connection.prepare(
      "SELECT state,revision,created_at_ms,updated_at_ms FROM "
      "chronicle_summary_draft WHERE draft_id=? AND session_id=?");
  draft.bind(1, payload->draft_id);
  draft.bind(2, payload->session_id);
  if (!draft.step() || draft.column_text(0) != "pending" ||
      static_cast<std::size_t>(draft.column_int64(1)) !=
          payload->expected_draft_revision) {
    complete_claim(connection, request.job, request.now_ms);
    transaction.commit();
    return WorkMutationStatus::unchanged;
  }

  const auto validation = load_summary_context(connection, payload->session_id);
  if (validation != request.generation_context) {
    transaction.commit();
    return WorkMutationStatus::invalid_state;
  }
  const auto transition_at_ms =
      std::max({request.now_ms, session.column_int64(4),
                session.column_int64(5), draft.column_int64(2),
                draft.column_int64(3)});

  const bool valid_candidate =
      request.candidate.has_value() &&
      validate_summary_candidate(*request.candidate, validation) ==
          SummaryValidationCode::valid &&
      request.title_ids.size() == request.candidate->proposed_titles.size();
  if (valid_candidate) {
    auto update = connection.prepare(
        "UPDATE chronicle_summary_draft SET chapter_title=?,summary=?,"
        "source='model',model_failure_category=NULL,revision=revision+1,"
        "updated_at_ms=? WHERE draft_id=? AND state='pending' AND revision=?");
    update.bind(1, request.candidate->chapter_title);
    update.bind(2, request.candidate->summary);
    update.bind(3, transition_at_ms);
    update.bind(4, payload->draft_id);
    update.bind(5, static_cast<std::int64_t>(payload->expected_draft_revision));
    update.execute();
    if (connection.changes() != 1)
      throw std::runtime_error{"Chronicle summary draft lost its fence."};
    for (std::size_t index = 0;
         index < request.candidate->highlighted_entry_ids.size(); ++index) {
      auto insert = connection.prepare(
          "INSERT INTO chronicle_summary_highlight(draft_id,entry_id,position) "
          "VALUES (?,?,?)");
      insert.bind(1, payload->draft_id);
      insert.bind(2, request.candidate->highlighted_entry_ids[index]);
      insert.bind(3, static_cast<std::int64_t>(index));
      insert.execute();
    }
    for (std::size_t index = 0;
         index < request.candidate->proposed_titles.size(); ++index) {
      const auto &proposal = request.candidate->proposed_titles[index];
      const auto &ids = request.title_ids[index];
      require_uuid(ids.definition_id);
      require_uuid(ids.grant_id);
      auto existing = connection.prepare(
          "SELECT 1 FROM chronicle_title_grant g JOIN "
          "chronicle_title_definition d "
          "ON d.definition_id=g.definition_id WHERE d.session_id=? "
          "AND g.recipient_user_id=?");
      existing.bind(1, payload->session_id);
      existing.bind(2, proposal.recipient_user_id.str());
      if (existing.step())
        continue;
      auto definition = connection.prepare(
          "INSERT INTO "
          "chronicle_title_definition(definition_id,title,description,"
          "provenance,session_id,supporting_entry_id,proposed_by_user_id,"
          "created_at_ms) "
          "VALUES (?,?,?,'session_ai',?,?,?,?)");
      definition.bind(1, ids.definition_id);
      definition.bind(2, proposal.title);
      definition.bind(3, proposal.description);
      definition.bind(4, payload->session_id);
      bind_optional(definition, 5, proposal.supporting_entry_id);
      definition.bind(6, request.owner_user_id.str());
      definition.bind(7, transition_at_ms);
      definition.execute();
      auto grant = connection.prepare(
          "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
          "recipient_user_id,state,featured,revision,source_idempotency_key,"
          "proposed_at_ms) VALUES (?,?,?,'proposed',0,1,?,?)");
      grant.bind(1, ids.grant_id);
      grant.bind(2, ids.definition_id);
      grant.bind(3, proposal.recipient_user_id.str());
      grant.bind(4, "title:session:" + payload->session_id + ":" +
                        proposal.recipient_user_id.str());
      grant.bind(5, transition_at_ms);
      grant.execute();
    }
  } else {
    std::string failure = request.failure_category.value_or(
        request.candidate ? "validation_failed" : "model_failed");
    if (failure.empty() || failure.size() > 96)
      failure = "model_failed";
    auto update = connection.prepare(
        "UPDATE chronicle_summary_draft SET model_failure_category=?,"
        "updated_at_ms=? WHERE draft_id=? AND state='pending' AND revision=?");
    update.bind(1, failure);
    update.bind(2, transition_at_ms);
    update.bind(3, payload->draft_id);
    update.bind(4, static_cast<std::int64_t>(payload->expected_draft_revision));
    update.execute();
    if (connection.changes() != 1)
      throw std::runtime_error{"Chronicle summary fallback lost its fence."};
  }

  auto close = connection.prepare(
      "UPDATE chronicle_session SET state='closed',closed_at_ms=?,"
      "revision=revision+1 WHERE session_id=? AND state='closing' AND "
      "revision=?");
  close.bind(1, transition_at_ms);
  close.bind(2, payload->session_id);
  close.bind(3, static_cast<std::int64_t>(payload->expected_session_revision));
  close.execute();
  if (connection.changes() != 1)
    throw std::runtime_error{"Chronicle summary completion lost its fence."};

  auto draft_row = connection.prepare(
      "SELECT chapter_title,summary,revision FROM chronicle_summary_draft "
      "WHERE draft_id=?");
  draft_row.bind(1, payload->draft_id);
  if (!draft_row.step())
    throw std::runtime_error{"Chronicle summary draft disappeared."};
  const auto chapter_title = draft_row.column_text(0);
  const auto summary = draft_row.column_text(1);
  const auto draft_revision =
      static_cast<std::size_t>(draft_row.column_int64(2));
  enqueue_summary_review(
      connection,
      SummaryReviewDeliveryIds{
          .notice_id = request.notice_id,
          .notice_token_id = request.notice_token_id,
          .edit_token_id = request.edit_token_id,
          .approve_token_id = request.approve_token_id,
          .reject_token_id = request.reject_token_id,
          .notice_outbox_id = request.notice_outbox_id,
      },
      payload->session_id, payload->draft_id, guild, channel,
      request.owner_user_id, chapter_title, summary, draft_revision,
      transition_at_ms, request.job.correlation_id, request.event_id, true);

  if (!detail::insert_event_uncommitted(
          connection,
          make_event(request.event_id, "chronicle.session_completed.v1",
                     "chronicle_session", payload->session_id,
                     request.owner_user_id, guild, channel, transition_at_ms,
                     request.job.correlation_id,
                     "event:session-completed:" + payload->session_id,
                     Json{{"draft_id", payload->draft_id},
                          {"source", valid_candidate ? "model" : "fallback"}},
                     request.job.causation_event_id)))
    throw std::runtime_error{"Chronicle summary completion event conflict."};
  for (const auto participant : validation.opted_in_participants) {
    const auto ids =
        std::find_if(request.relationship_event_ids.begin(),
                     request.relationship_event_ids.end(),
                     [participant](const auto &candidate) {
                       return candidate.participant_user_id == participant;
                     });
    if (ids == request.relationship_event_ids.end())
      throw std::invalid_argument{"Missing session relationship event ID."};
    static_cast<void>(detail::insert_relationship_event_uncommitted(
        connection, ids->relationship_event_id, request.event_id,
        "chronicle.session_completed.v1", "session.completed", participant,
        relationship_policy(RelationshipSourceKind::session_completed),
        transition_at_ms, transition_at_ms));
  }
  complete_claim(connection, request.job, transition_at_ms);
  transaction.commit();
  return WorkMutationStatus::applied;
}

WorkMutationStatus SqliteChronicleSessionRepository::purge_context_job(
    const ClaimedScheduledJob &job, const std::int64_t now_ms) {
  const auto *payload =
      std::get_if<SessionContextPurgeJobPayload>(&job.payload);
  if (payload == nullptr)
    return WorkMutationStatus::invalid_state;
  require_uuid(payload->session_id);
  if (now_ms < 0)
    throw std::invalid_argument{"Invalid Chronicle context-purge time."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto claim = connection.prepare(
      "SELECT 1 FROM scheduled_job WHERE job_id=? AND state='claimed' "
      "AND lease_owner=? AND lease_token=?");
  claim.bind(1, job.job_id);
  claim.bind(2, job.lease_owner);
  claim.bind(3, job.lease_token);
  if (!claim.step()) {
    const auto result = claim_status(connection, job);
    transaction.commit();
    return result;
  }
  auto session = connection.prepare(
      "SELECT state,closing_at_ms FROM chronicle_session WHERE session_id=?");
  session.bind(1, payload->session_id);
  const bool session_exists = session.step();
  const auto state = session_exists ? session.column_text(0) : std::string{};
  const bool remains_open = state == "open";
  std::optional<std::int64_t> next_due;
  if (remains_open) {
    auto purge = connection.prepare(
        "DELETE FROM chronicle_session_context WHERE session_id=? AND "
        "observed_at_ms<=?");
    purge.bind(1, payload->session_id);
    purge.bind(2, now_ms - session_context_retention_ms);
    purge.execute();
    auto oldest = connection.prepare(
        "SELECT min(observed_at_ms) FROM chronicle_session_context WHERE "
        "session_id=?");
    oldest.bind(1, payload->session_id);
    if (!oldest.step())
      throw std::runtime_error{"Chronicle context expiry query failed."};
    if (!oldest.column_is_null(0))
      next_due = oldest.column_int64(0) + session_context_retention_ms;
  } else if ((state == "closing" || state == "closed") &&
             !session.column_is_null(1) &&
             now_ms < session.column_int64(1) + session_context_retention_ms) {
    next_due = session.column_int64(1) + session_context_retention_ms;
  } else {
    auto purge = connection.prepare(
        "DELETE FROM chronicle_session_context WHERE session_id=?");
    purge.bind(1, payload->session_id);
    purge.execute();
  }
  if (next_due) {
    auto reschedule = connection.prepare(
        "UPDATE scheduled_job SET state='pending',due_at_ms=?,"
        "attempt_count=max(attempt_count-1,0),lease_owner=NULL,lease_token="
        "NULL,"
        "lease_until_ms=NULL,updated_at_ms=max(?,updated_at_ms),terminal_at_ms="
        "NULL,"
        "last_error_code=NULL WHERE job_id=? AND state='claimed' AND "
        "lease_owner=? AND lease_token=?");
    reschedule.bind(1, *next_due);
    reschedule.bind(2, now_ms);
    reschedule.bind(3, job.job_id);
    reschedule.bind(4, job.lease_owner);
    reschedule.bind(5, job.lease_token);
    reschedule.execute();
    if (connection.changes() != 1) {
      transaction.commit();
      return claim_status(connection, job);
    }
  } else {
    complete_claim(connection, job, now_ms);
  }
  transaction.commit();
  return WorkMutationStatus::applied;
}

SessionMutationResult SqliteChronicleSessionRepository::edit_summary(
    const SummaryEditRequest &request) {
  require_uuid(request.draft_id);
  require_uuid(request.event_id);
  require_key(request.idempotency_key);
  if (request.actor_user_id != request.owner_user_id || request.now_ms < 0)
    return session_result(ChronicleSessionResultCode::unauthorized);
  if (!valid_chronicle_text(request.chapter_title,
                            maximum_chronicle_title_size) ||
      !valid_chronicle_text(request.summary, maximum_chronicle_body_size))
    return session_result(ChronicleSessionResultCode::invalid_input);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay =
      connection.prepare("SELECT 1 FROM event_journal WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::unchanged);
  }
  auto row = connection.prepare(
      "SELECT d.session_id,d.state,d.revision,s.guild_id,s.channel_id,s.state,"
      "d.created_at_ms,d.updated_at_ms,s.closed_at_ms,d.review_notice_id "
      "FROM chronicle_summary_draft d JOIN chronicle_session s "
      "ON s.session_id=d.session_id WHERE d.draft_id=?");
  row.bind(1, request.draft_id);
  if (!row.step()) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::not_found);
  }
  if (row.column_text(1) != "pending" || row.column_text(5) != "closed") {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::invalid_state);
  }
  if (static_cast<std::size_t>(row.column_int64(2)) !=
      request.expected_revision) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::stale_revision);
  }
  require_uuid(request.notice_id);
  require_uuid(request.notice_token_id);
  require_uuid(request.edit_token_id);
  require_uuid(request.approve_token_id);
  require_uuid(request.reject_token_id);
  require_uuid(request.notice_outbox_id);
  if (request.control_token_id)
    require_uuid(*request.control_token_id);
  const auto session_id = row.column_text(0);
  const auto guild = DiscordSnowflake::parse(row.column_text(3));
  const auto channel = DiscordSnowflake::parse(row.column_text(4));
  const auto transition_at_ms =
      std::max({request.now_ms, row.column_int64(6), row.column_int64(7),
                row.column_int64(8)});
  const auto old_notice_id = optional_text(row, 9);
  if (request.control_token_id) {
    auto use_control = connection.prepare(
        "UPDATE interaction_token SET state='used',used_at_ms=? WHERE "
        "token_id=? AND interaction_kind='modal' AND "
        "action='chronicle.summary.edit' AND "
        "entity_type='chronicle_summary_draft' AND entity_id=? AND "
        "expected_user_id=? AND expected_entity_revision=? AND state='active'");
    use_control.bind(1, transition_at_ms);
    use_control.bind(2, *request.control_token_id);
    use_control.bind(3, request.draft_id);
    use_control.bind(4, request.actor_user_id.str());
    use_control.bind(5, static_cast<std::int64_t>(request.expected_revision));
    use_control.execute();
    if (connection.changes() != 1) {
      transaction.commit();
      return session_result(ChronicleSessionResultCode::stale_revision);
    }
  }
  auto update = connection.prepare(
      "UPDATE chronicle_summary_draft SET chapter_title=?,summary=?,"
      "source='manual',revision=revision+1,updated_at_ms=? WHERE draft_id=? "
      "AND state='pending' AND revision=? AND EXISTS (SELECT 1 FROM "
      "chronicle_session s WHERE "
      "s.session_id=chronicle_summary_draft.session_id "
      "AND s.state='closed')");
  update.bind(1, request.chapter_title);
  update.bind(2, request.summary);
  update.bind(3, transition_at_ms);
  update.bind(4, request.draft_id);
  update.bind(5, static_cast<std::int64_t>(request.expected_revision));
  update.execute();
  if (connection.changes() != 1) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::stale_revision);
  }
  cancel_summary_review_delivery(connection, session_id, request.draft_id,
                                 old_notice_id, transition_at_ms);
  enqueue_summary_review(
      connection,
      SummaryReviewDeliveryIds{
          .notice_id = request.notice_id,
          .notice_token_id = request.notice_token_id,
          .edit_token_id = request.edit_token_id,
          .approve_token_id = request.approve_token_id,
          .reject_token_id = request.reject_token_id,
          .notice_outbox_id = request.notice_outbox_id,
      },
      session_id, request.draft_id, guild, channel, request.owner_user_id,
      request.chapter_title, request.summary, request.expected_revision + 1,
      transition_at_ms, request.correlation_id, request.event_id, false);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      make_event(request.event_id, "chronicle.summary_edited.v1",
                 "chronicle_session", session_id, request.actor_user_id, guild,
                 channel, transition_at_ms, request.correlation_id,
                 request.idempotency_key,
                 Json{{"draft_id", request.draft_id},
                      {"revision", request.expected_revision + 1}})));
  auto result = load_session(connection, session_id);
  transaction.commit();
  return {.code = ChronicleSessionResultCode::updated,
          .session = std::move(result),
          .wake_outbox = true};
}

SessionMutationResult SqliteChronicleSessionRepository::decide_summary(
    const SummaryDecisionRequest &request) {
  require_uuid(request.draft_id);
  require_uuid(request.entry_id);
  require_uuid(request.event_id);
  require_uuid(request.outbox_id);
  require_key(request.idempotency_key);
  if (request.control_token_id)
    require_uuid(*request.control_token_id);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.now_ms);
  if (request.actor_user_id != request.owner_user_id)
    return session_result(ChronicleSessionResultCode::unauthorized);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT aggregate_id FROM event_journal WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    auto result = load_session(connection, replay.column_text(0));
    transaction.commit();
    return {.code = ChronicleSessionResultCode::unchanged,
            .session = std::move(result)};
  }
  auto row = connection.prepare(
      "SELECT d.session_id,d.state,d.revision,d.chapter_title,d.summary,"
      "s.closed_at_ms,s.state,d.created_at_ms,d.updated_at_ms,"
      "d.review_notice_id FROM chronicle_summary_draft d JOIN "
      "chronicle_session s "
      "ON s.session_id=d.session_id WHERE d.draft_id=? AND s.guild_id=? "
      "AND s.channel_id=?");
  row.bind(1, request.draft_id);
  row.bind(2, request.guild_id.str());
  row.bind(3, request.channel_id.str());
  if (!row.step()) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::not_found);
  }
  if (row.column_text(1) != "pending" || row.column_text(6) != "closed") {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::invalid_state);
  }
  if (static_cast<std::size_t>(row.column_int64(2)) !=
      request.expected_revision) {
    transaction.commit();
    return session_result(ChronicleSessionResultCode::stale_revision);
  }
  const auto session_id = row.column_text(0);
  const auto chapter_title = row.column_text(3);
  const auto summary = row.column_text(4);
  const auto occurred_at =
      row.column_is_null(5) ? request.now_ms : row.column_int64(5);
  const auto transition_at_ms =
      std::max({request.now_ms, occurred_at, row.column_int64(7),
                row.column_int64(8)});
  const auto review_notice_id = optional_text(row, 9);
  if (request.approve) {
    auto insert = connection.prepare(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
        "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_"
        "ms,"
        "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
        "source_message_id,source_author_user_id,source_text,"
        "source_text_truncated,source_attachment_count,revision,source_kind) "
        "VALUES (?,'session_summary',?,?,'shared','canon',?,?,?,?,?,?,?, "
        "?,NULL,"
        "?,'',0,0,1,'session_summary')");
    insert.bind(1, request.entry_id);
    insert.bind(2, chapter_title);
    insert.bind(3, summary);
    insert.bind(4, occurred_at);
    insert.bind(5, transition_at_ms);
    insert.bind(6, request.actor_user_id.str());
    insert.bind(7, transition_at_ms);
    insert.bind(8, transition_at_ms);
    insert.bind(9, request.actor_user_id.str());
    insert.bind(10, request.guild_id.str());
    insert.bind(11, request.channel_id.str());
    insert.bind(12, request.actor_user_id.str());
    insert.execute();
    auto participants = connection.prepare(
        "INSERT INTO chronicle_participant(entry_id,user_id,role) "
        "SELECT ?,sp.user_id,'session_participant' FROM "
        "chronicle_session_participant sp JOIN user_preference p "
        "ON p.user_id=sp.user_id WHERE sp.session_id=? AND "
        "p.chronicle_opt_in=1");
    participants.bind(1, request.entry_id);
    participants.bind(2, session_id);
    participants.execute();
    insert_public_outbox(
        connection, request.outbox_id, request.guild_id, request.channel_id,
        "chronicle_session", session_id,
        "outbox:summary-approved:" + session_id, transition_at_ms,
        request.correlation_id, request.event_id,
        InteractionMessage{.content = "**A Chronicle chapter is sealed**\n**" +
                                      chapter_title + "**\n" + summary,
                           .embed = std::nullopt,
                           .buttons = {},
                           .allowed_user_mentions = {}});
  }
  auto update = connection.prepare(
      request.approve
          ? "UPDATE chronicle_summary_draft SET "
            "state='approved',decided_at_ms=?,"
            "decided_by_user_id=?,approved_entry_id=?,revision=revision+1,"
            "updated_at_ms=? WHERE draft_id=? AND state='pending' AND "
            "revision=? "
            "AND EXISTS (SELECT 1 FROM chronicle_session s WHERE "
            "s.session_id=chronicle_summary_draft.session_id AND "
            "s.state='closed')"
          : "UPDATE chronicle_summary_draft SET "
            "state='rejected',decided_at_ms=?,"
            "decided_by_user_id=?,revision=revision+1,updated_at_ms=? WHERE "
            "draft_id=? AND state='pending' AND revision=? AND EXISTS (SELECT "
            "1 "
            "FROM chronicle_session s WHERE "
            "s.session_id=chronicle_summary_draft.session_id AND "
            "s.state='closed')");
  update.bind(1, transition_at_ms);
  update.bind(2, request.actor_user_id.str());
  std::size_t next_bind = 3;
  if (request.approve)
    update.bind(next_bind++, request.entry_id);
  update.bind(next_bind++, transition_at_ms);
  update.bind(next_bind++, request.draft_id);
  update.bind(next_bind, static_cast<std::int64_t>(request.expected_revision));
  update.execute();
  if (connection.changes() != 1)
    throw std::runtime_error{"Chronicle summary decision lost its fence."};
  if (!request.approve) {
    auto reject_titles = connection.prepare(
        "UPDATE chronicle_title_grant SET state='rejected',featured=0,"
        "decided_at_ms=?,decided_by_user_id=?,revision=revision+1 WHERE "
        "state='proposed' AND definition_id IN (SELECT definition_id FROM "
        "chronicle_title_definition WHERE session_id=?)");
    reject_titles.bind(1, transition_at_ms);
    reject_titles.bind(2, request.actor_user_id.str());
    reject_titles.bind(3, session_id);
    reject_titles.execute();
  }
  auto purge = connection.prepare(
      "DELETE FROM chronicle_session_context WHERE session_id=?");
  purge.bind(1, session_id);
  purge.execute();
  auto cancel_purge = connection.prepare(
      "UPDATE scheduled_job SET "
      "state='cancelled',lease_owner=NULL,lease_token=NULL,"
      "lease_until_ms=NULL,terminal_at_ms=max(?,created_at_ms),updated_at_ms="
      "max(?,"
      "updated_at_ms) WHERE job_type=? AND aggregate_id=? AND state IN "
      "('pending','claimed')");
  cancel_purge.bind(1, transition_at_ms);
  cancel_purge.bind(2, transition_at_ms);
  cancel_purge.bind(3, std::string{session_context_purge_job_type});
  cancel_purge.bind(4, session_id);
  cancel_purge.execute();
  if (request.control_token_id) {
    auto use_control = connection.prepare(
      "UPDATE interaction_token SET state='used',used_at_ms=? WHERE "
      "token_id=? "
      "AND entity_type='chronicle_summary_draft' AND entity_id=? AND "
      "expected_user_id=? AND guild_id=? AND channel_id=? AND "
      "expected_entity_revision=? AND state='active'");
    use_control.bind(1, transition_at_ms);
    use_control.bind(2, *request.control_token_id);
    use_control.bind(3, request.draft_id);
    use_control.bind(4, request.actor_user_id.str());
    use_control.bind(5, request.guild_id.str());
    use_control.bind(6, request.channel_id.str());
    use_control.bind(7, static_cast<std::int64_t>(request.expected_revision));
    use_control.execute();
    if (connection.changes() != 1)
      throw std::runtime_error{"Chronicle summary control lost its fence."};
  }
  cancel_summary_review_delivery(connection, session_id, request.draft_id,
                                 review_notice_id, transition_at_ms);
  Json decision_payload{{"draft_id", request.draft_id}};
  decision_payload["entry_id"] =
      request.approve ? Json{request.entry_id} : Json{nullptr};
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      make_event(request.event_id,
                 request.approve ? "chronicle.summary_approved.v1"
                                 : "chronicle.summary_rejected.v1",
                 "chronicle_session", session_id, request.actor_user_id,
                 request.guild_id, request.channel_id, transition_at_ms,
                 request.correlation_id, request.idempotency_key,
                 std::move(decision_payload))));
  auto result = load_session(connection, session_id);
  transaction.commit();
  return {.code = ChronicleSessionResultCode::updated,
          .session = std::move(result),
          .wake_outbox = request.approve};
}

std::optional<SummaryControlResolution>
SqliteChronicleSessionRepository::resolve_summary_control(
    const std::string_view token_id, const DiscordSnowflake &actor_user_id,
    const DiscordSnowflake &guild_id, const DiscordSnowflake &channel_id,
    const InteractionKind interaction_kind,
    const std::string_view interaction_idempotency_key,
    const std::int64_t now_ms) {
  require_uuid(token_id);
  require_key(interaction_idempotency_key);
  require_context(guild_id, channel_id, actor_user_id, now_ms);
  const auto expected_kind =
      interaction_kind == InteractionKind::modal_submit ? "modal"
      : interaction_kind == InteractionKind::button     ? "button"
                                                        : "";
  if (expected_kind[0] == '\0')
    return std::nullopt;
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT entity_id,action,expected_entity_revision FROM interaction_token "
      "WHERE token_id=? AND token_version=1 AND entity_type="
      "'chronicle_summary_draft' AND expected_user_id=? AND guild_id=? AND "
      "channel_id=? AND interaction_kind=? AND state='active' AND "
      "expires_at_ms>?");
  query.bind(1, token_id);
  query.bind(2, actor_user_id.str());
  query.bind(3, guild_id.str());
  query.bind(4, channel_id.str());
  query.bind(5, expected_kind);
  query.bind(6, now_ms);
  if (query.step()) {
    return SummaryControlResolution{
        .draft_id = query.column_text(0),
        .action = query.column_text(1),
        .expected_revision = static_cast<std::size_t>(query.column_int64(2)),
    };
  }
  auto replay = context_->connection().prepare(
      "SELECT t.entity_id,t.action,t.expected_entity_revision FROM "
      "interaction_token t JOIN chronicle_summary_draft d ON "
      "d.draft_id=t.entity_id JOIN event_journal e ON "
      "e.aggregate_type='chronicle_session' AND e.aggregate_id=d.session_id "
      "WHERE t.token_id=? AND t.token_version=1 AND t.entity_type="
      "'chronicle_summary_draft' AND t.expected_user_id=? AND t.guild_id=? AND "
      "t.channel_id=? AND t.interaction_kind=? AND t.state='used' AND "
      "e.idempotency_key=? AND ((t.action='chronicle.summary.edit' AND "
      "e.event_type='chronicle.summary_edited.v1') OR "
      "(t.action='chronicle.summary.approve' AND "
      "e.event_type='chronicle.summary_approved.v1') OR "
      "(t.action='chronicle.summary.reject' AND "
      "e.event_type='chronicle.summary_rejected.v1'))");
  replay.bind(1, token_id);
  replay.bind(2, actor_user_id.str());
  replay.bind(3, guild_id.str());
  replay.bind(4, channel_id.str());
  replay.bind(5, expected_kind);
  replay.bind(6, interaction_idempotency_key);
  if (!replay.step())
    return std::nullopt;
  return SummaryControlResolution{
      .draft_id = replay.column_text(0),
      .action = replay.column_text(1),
      .expected_revision = static_cast<std::size_t>(replay.column_int64(2)),
  };
}

TitleMutationResult SqliteChronicleSessionRepository::propose_title(
    const ProposeTitleRequest &request) {
  require_uuid(request.definition_id);
  require_uuid(request.grant_id);
  require_uuid(request.event_id);
  require_key(request.idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.now_ms);
  if (request.actor_user_id != request.owner_user_id)
    return title_result(ChronicleSessionResultCode::unauthorized);
  if (!valid_chronicle_text(request.title, maximum_chronicle_title_size) ||
      !valid_chronicle_text(request.description, maximum_memory_text_size))
    return title_result(ChronicleSessionResultCode::invalid_input);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT "
      "g.grant_id,g.recipient_user_id,d.title,d.description,d.provenance,"
      "g.state,g.featured,g.revision FROM chronicle_title_grant g JOIN "
      "chronicle_title_definition d ON d.definition_id=g.definition_id WHERE "
      "g.source_idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    ChronicleTitleGrant existing{
        .grant_id = replay.column_text(0),
        .recipient_user_id = DiscordSnowflake::parse(replay.column_text(1)),
        .title = replay.column_text(2),
        .description = replay.column_text(3),
        .provenance = replay.column_text(4) == "session_ai"
                          ? ChronicleTitleProvenance::session_ai
                          : ChronicleTitleProvenance::owner_curated,
        .state = title_state(replay.column_text(5)),
        .featured = replay.column_int64(6) != 0,
        .revision = static_cast<std::size_t>(replay.column_int64(7)),
    };
    transaction.commit();
    return {.code = ChronicleSessionResultCode::existing,
            .grant = std::move(existing),
            .wake_outbox = false};
  }
  if (!opted_in(connection, request.recipient_user_id)) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::opted_out);
  }
  auto definition = connection.prepare(
      "INSERT INTO chronicle_title_definition(definition_id,title,description,"
      "provenance,session_id,supporting_entry_id,proposed_by_user_id,created_"
      "at_ms) "
      "VALUES (?,?,?,'owner_curated',NULL,NULL,?,?)");
  definition.bind(1, request.definition_id);
  definition.bind(2, request.title);
  definition.bind(3, request.description);
  definition.bind(4, request.actor_user_id.str());
  definition.bind(5, request.now_ms);
  definition.execute();
  auto grant = connection.prepare(
      "INSERT INTO "
      "chronicle_title_grant(grant_id,definition_id,recipient_user_id,"
      "state,featured,revision,source_idempotency_key,proposed_at_ms) "
      "VALUES (?,?,?,'proposed',0,1,?,?)");
  grant.bind(1, request.grant_id);
  grant.bind(2, request.definition_id);
  grant.bind(3, request.recipient_user_id.str());
  grant.bind(4, request.idempotency_key);
  grant.bind(5, request.now_ms);
  grant.execute();
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      make_event(request.event_id, "chronicle.title_proposed.v1",
                 "chronicle_title", request.grant_id, request.actor_user_id,
                 request.guild_id, request.channel_id, request.now_ms,
                 request.correlation_id, "event:" + request.idempotency_key,
                 Json{{"recipient_user_id", request.recipient_user_id.str()},
                      {"provenance", "owner_curated"}})));
  transaction.commit();
  return {.code = ChronicleSessionResultCode::created,
          .grant = ChronicleTitleGrant{
              .grant_id = request.grant_id,
              .recipient_user_id = request.recipient_user_id,
              .title = request.title,
              .description = request.description,
              .provenance = ChronicleTitleProvenance::owner_curated,
              .state = ChronicleTitleState::proposed,
              .featured = false,
              .revision = 1}};
}

TitleMutationResult SqliteChronicleSessionRepository::mutate_title(
    const TitleMutationRequest &request) {
  require_uuid(request.grant_id);
  require_uuid(request.award_entry_id);
  require_uuid(request.event_id);
  require_uuid(request.outbox_id);
  require_uuid(request.relationship_event_id);
  require_key(request.idempotency_key);
  require_context(request.guild_id, request.channel_id, request.actor_user_id,
                  request.now_ms);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay =
      connection.prepare("SELECT 1 FROM event_journal WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::unchanged);
  }
  auto row = connection.prepare(
      "SELECT g.recipient_user_id,g.state,g.featured,g.revision,d.title,"
      "d.description,d.provenance FROM chronicle_title_grant g JOIN "
      "chronicle_title_definition d ON d.definition_id=g.definition_id "
      "WHERE g.grant_id=?");
  row.bind(1, request.grant_id);
  if (!row.step()) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::not_found);
  }
  const auto recipient = DiscordSnowflake::parse(row.column_text(0));
  const auto old_state = title_state(row.column_text(1));
  const auto featured = row.column_int64(2) != 0;
  const auto revision = static_cast<std::size_t>(row.column_int64(3));
  const auto title = row.column_text(4);
  const auto description = row.column_text(5);
  const auto provenance = row.column_text(6) == "session_ai"
                              ? ChronicleTitleProvenance::session_ai
                              : ChronicleTitleProvenance::owner_curated;
  const bool owner_action = request.action == TitleAction::approve ||
                            request.action == TitleAction::reject;
  const bool allowed = owner_action
                           ? request.actor_user_id == request.owner_user_id
                       : request.action == TitleAction::feature
                           ? request.actor_user_id == recipient
                           : request.actor_user_id == recipient ||
                                 request.actor_user_id == request.owner_user_id;
  if (!allowed) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::unauthorized);
  }
  if (request.action == TitleAction::approve &&
      !opted_in(connection, recipient)) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::opted_out);
  }
  if (revision != request.expected_revision) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::stale_revision);
  }
  const auto next_state = transition_title(old_state, request.action);
  if (!next_state) {
    transaction.commit();
    return title_result(ChronicleSessionResultCode::invalid_state);
  }
  bool next_featured = featured;
  if (request.action == TitleAction::approve) {
    auto prior_award_count = connection.prepare(
        "SELECT count(*) FROM chronicle_title_grant WHERE recipient_user_id=? "
        "AND award_entry_id IS NOT NULL");
    prior_award_count.bind(1, recipient.str());
    if (!prior_award_count.step())
      throw std::runtime_error{"Title feature query failed."};
    next_featured = prior_award_count.column_int64(0) == 0;
  } else if (request.action == TitleAction::feature) {
    auto clear = connection.prepare(
        "UPDATE chronicle_title_grant SET featured=0,revision=revision+1 "
        "WHERE recipient_user_id=? AND featured=1 AND grant_id<>?");
    clear.bind(1, recipient.str());
    clear.bind(2, request.grant_id);
    clear.execute();
    next_featured = true;
  } else if (request.action == TitleAction::revoke ||
             request.action == TitleAction::reject) {
    next_featured = false;
  }
  if (request.action == TitleAction::approve) {
    auto update = connection.prepare(
        "UPDATE chronicle_title_grant SET state='active',featured=?,"
        "decided_at_ms=?,decided_by_user_id=?,revision=revision+1 "
        "WHERE grant_id=? AND state='proposed' AND revision=?");
    update.bind(1, static_cast<std::int64_t>(next_featured));
    update.bind(2, request.now_ms);
    update.bind(3, request.actor_user_id.str());
    update.bind(4, request.grant_id);
    update.bind(5, static_cast<std::int64_t>(revision));
    update.execute();
    if (connection.changes() != 1)
      throw std::runtime_error{"Chronicle title transition lost its fence."};
    auto entry = connection.prepare(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
        "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_"
        "ms,"
        "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
        "source_message_id,source_author_user_id,source_text,"
        "source_text_truncated,source_attachment_count,revision,source_kind) "
        "VALUES (?,'title_award',?,?,'shared','canon',?,?,?,?,?,?,?, ?,NULL,"
        "?,'',0,0,1,'title_award')");
    entry.bind(1, request.award_entry_id);
    entry.bind(2, title);
    entry.bind(3, description);
    entry.bind(4, request.now_ms);
    entry.bind(5, request.now_ms);
    entry.bind(6, request.actor_user_id.str());
    entry.bind(7, request.now_ms);
    entry.bind(8, request.now_ms);
    entry.bind(9, request.actor_user_id.str());
    entry.bind(10, request.guild_id.str());
    entry.bind(11, request.channel_id.str());
    entry.bind(12, request.actor_user_id.str());
    entry.execute();
    auto participant = connection.prepare(
        "INSERT INTO chronicle_participant(entry_id,user_id,role) "
        "VALUES (?,?,'title_recipient')");
    participant.bind(1, request.award_entry_id);
    participant.bind(2, recipient.str());
    participant.execute();
    auto award_reference = connection.prepare(
        "UPDATE chronicle_title_grant SET award_entry_id=? WHERE grant_id=? "
        "AND state='active' AND award_entry_id IS NULL");
    award_reference.bind(1, request.award_entry_id);
    award_reference.bind(2, request.grant_id);
    award_reference.execute();
    if (connection.changes() != 1)
      throw std::runtime_error{
          "Chronicle title award reference was not stored."};
    insert_public_outbox(
        connection, request.outbox_id, request.guild_id, request.channel_id,
        "chronicle_title", request.grant_id,
        "outbox:title-awarded:" + request.grant_id, request.now_ms,
        request.correlation_id, request.event_id,
        InteractionMessage{.content =
                               "**A title is entered into the Chronicle**\n"
                               "<@" +
                               recipient.str() + "> — **" + title + "**\n" +
                               description,
                           .embed = std::nullopt,
                           .buttons = {},
                           .allowed_user_mentions = {recipient}});
  } else if (request.action == TitleAction::reject) {
    auto update = connection.prepare(
        "UPDATE chronicle_title_grant SET state='rejected',featured=0,"
        "decided_at_ms=?,decided_by_user_id=?,revision=revision+1 WHERE "
        "grant_id=? AND state='proposed' AND revision=?");
    update.bind(1, request.now_ms);
    update.bind(2, request.actor_user_id.str());
    update.bind(3, request.grant_id);
    update.bind(4, static_cast<std::int64_t>(revision));
    update.execute();
  } else if (request.action == TitleAction::feature) {
    auto update = connection.prepare(
        "UPDATE chronicle_title_grant SET featured=1,revision=revision+1 "
        "WHERE grant_id=? AND state='active' AND revision=?");
    update.bind(1, request.grant_id);
    update.bind(2, static_cast<std::int64_t>(revision));
    update.execute();
  } else {
    auto update = connection.prepare(
        "UPDATE chronicle_title_grant SET state='revoked',featured=0,"
        "revoked_at_ms=?,revoked_by_user_id=?,revision=revision+1 WHERE "
        "grant_id=? AND state='active' AND revision=?");
    update.bind(1, request.now_ms);
    update.bind(2, request.actor_user_id.str());
    update.bind(3, request.grant_id);
    update.bind(4, static_cast<std::int64_t>(revision));
    update.execute();
  }
  if (connection.changes() != 1)
    throw std::runtime_error{"Chronicle title transition lost its fence."};
  const std::string event_type =
      request.action == TitleAction::approve   ? "chronicle.title_awarded.v1"
      : request.action == TitleAction::reject  ? "chronicle.title_rejected.v1"
      : request.action == TitleAction::feature ? "chronicle.title_featured.v1"
                                               : "chronicle.title_revoked.v1";
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      make_event(request.event_id, event_type, "chronicle_title",
                 request.grant_id, request.actor_user_id, request.guild_id,
                 request.channel_id, request.now_ms, request.correlation_id,
                 request.idempotency_key,
                 Json{{"recipient_user_id", recipient.str()},
                      {"revision", revision + 1}})));
  if (request.action == TitleAction::approve) {
    static_cast<void>(detail::insert_relationship_event_uncommitted(
        connection, request.relationship_event_id, request.event_id, event_type,
        "title.awarded", recipient,
        relationship_policy(RelationshipSourceKind::title_awarded),
        request.now_ms, request.now_ms));
  }
  transaction.commit();
  return {.code = ChronicleSessionResultCode::updated,
          .grant = ChronicleTitleGrant{.grant_id = request.grant_id,
                                       .recipient_user_id = recipient,
                                       .title = title,
                                       .description = description,
                                       .provenance = provenance,
                                       .state = *next_state,
                                       .featured = next_featured,
                                       .revision = revision + 1},
          .wake_outbox = request.action == TitleAction::approve};
}

ChronicleTitlePage SqliteChronicleSessionRepository::list_titles(
    const DiscordSnowflake &viewer, const DiscordSnowflake &target,
    const bool owner_view, const std::size_t page) {
  if (!viewer.is_set() || !target.is_set() ||
      page >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) /
              chronicle_title_page_size) {
    throw std::invalid_argument{"Invalid Chronicle title page."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  const bool own = viewer == target;
  const bool all_states = owner_view || own;
  auto count = connection.prepare(
      all_states ? "SELECT count(*) FROM chronicle_title_grant WHERE "
                   "recipient_user_id=?"
                 : "SELECT count(*) FROM chronicle_title_grant WHERE "
                   "recipient_user_id=? "
                   "AND state='active'");
  count.bind(1, target.str());
  if (!count.step())
    throw std::runtime_error{"Chronicle title count failed."};
  ChronicleTitlePage result{
      .page = page,
      .total = static_cast<std::size_t>(count.column_int64(0)),
      .grants = {},
  };
  auto query = connection.prepare(
      all_states
          ? "SELECT g.grant_id,g.recipient_user_id,d.title,d.description,"
            "d.provenance,g.state,g.featured,g.revision FROM "
            "chronicle_title_grant g JOIN chronicle_title_definition d ON "
            "d.definition_id=g.definition_id WHERE g.recipient_user_id=? "
            "ORDER BY g.proposed_at_ms DESC,g.grant_id DESC LIMIT ? OFFSET ?"
          : "SELECT g.grant_id,g.recipient_user_id,d.title,d.description,"
            "d.provenance,g.state,g.featured,g.revision FROM "
            "chronicle_title_grant g JOIN chronicle_title_definition d ON "
            "d.definition_id=g.definition_id WHERE g.recipient_user_id=? "
            "AND g.state='active' ORDER BY g.featured DESC,g.proposed_at_ms "
            "DESC "
            "LIMIT ? OFFSET ?");
  query.bind(1, target.str());
  query.bind(2, static_cast<std::int64_t>(chronicle_title_page_size));
  query.bind(3, static_cast<std::int64_t>(page * chronicle_title_page_size));
  while (query.step()) {
    result.grants.push_back(ChronicleTitleGrant{
        .grant_id = query.column_text(0),
        .recipient_user_id = DiscordSnowflake::parse(query.column_text(1)),
        .title = query.column_text(2),
        .description = query.column_text(3),
        .provenance = query.column_text(4) == "session_ai"
                          ? ChronicleTitleProvenance::session_ai
                          : ChronicleTitleProvenance::owner_curated,
        .state = title_state(query.column_text(5)),
        .featured = query.column_int64(6) != 0,
        .revision = static_cast<std::size_t>(query.column_int64(7)),
    });
  }
  return result;
}

ChronicleSearchPage SqliteChronicleSessionRepository::begin_search(
    const DiscordSnowflake &viewer, const ChronicleSearchFilter &filter,
    std::string cursor_id, const std::int64_t now_ms) {
  require_uuid(cursor_id);
  if (!viewer.is_set() || now_ms < 0 || filter.query.size() > 200)
    throw std::invalid_argument{"Invalid Chronicle search request."};
  static const std::unordered_set<std::string> allowed_types{
      "quote",           "deed",       "prediction", "incident", "custom",
      "session_summary", "title_award"};
  if (filter.entry_type && !allowed_types.contains(*filter.entry_type))
    throw std::invalid_argument{"Invalid Chronicle search type."};
  if (filter.from_ms && filter.to_ms && *filter.from_ms > *filter.to_ms)
    throw std::invalid_argument{"Invalid Chronicle search dates."};
  if (filter.presentation != "recall" && filter.presentation != "timeline")
    throw std::invalid_argument{"Invalid Chronicle search presentation."};
  const auto match = literal_fts_query(filter.query);
  std::unique_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (!opted_in(connection, viewer)) {
    transaction.commit();
    return {};
  }
  auto cleanup_items = connection.prepare(
      "DELETE FROM chronicle_search_cursor WHERE expires_at_ms<=?");
  cleanup_items.bind(1, now_ms);
  cleanup_items.execute();
  auto cursor = connection.prepare(
      "INSERT INTO "
      "chronicle_search_cursor(cursor_id,viewer_user_id,presentation,"
      "created_at_ms,expires_at_ms) VALUES (?,?,?,?,?)");
  cursor.bind(1, cursor_id);
  cursor.bind(2, viewer.str());
  cursor.bind(3, filter.presentation);
  cursor.bind(4, now_ms);
  cursor.bind(5, now_ms + chronicle_search_cursor_lifetime_ms);
  cursor.execute();

  std::string sql =
      match.empty() ? "SELECT e.entry_id,0,e.occurred_at_ms FROM "
                      "chronicle_entry e WHERE e.status='canon' AND "
                      "e.visibility='shared'"
                    : "SELECT e.entry_id,bm25(chronicle_entry_fts),"
                      "e.occurred_at_ms FROM chronicle_entry_fts JOIN "
                      "chronicle_entry e ON e.rowid=chronicle_entry_fts.rowid "
                      "WHERE chronicle_entry_fts MATCH ? AND e.status='canon' "
                      "AND e.visibility='shared'";
  if (filter.participant)
    sql += " AND EXISTS (SELECT 1 FROM chronicle_participant cp WHERE "
           "cp.entry_id=e.entry_id AND cp.user_id=?)";
  if (filter.entry_type)
    sql += " AND e.entry_type=?";
  if (filter.from_ms)
    sql += " AND e.occurred_at_ms>=?";
  if (filter.to_ms)
    sql += " AND e.occurred_at_ms<=?";
  sql += match.empty()
             ? " ORDER BY e.occurred_at_ms DESC,e.entry_id DESC LIMIT 50"
             : " ORDER BY bm25(chronicle_entry_fts),e.occurred_at_ms DESC,"
               "e.entry_id DESC LIMIT 50";
  auto entries = connection.prepare(sql);
  std::size_t binding = 1;
  if (!match.empty())
    entries.bind(binding++, match);
  if (filter.participant)
    entries.bind(binding++, filter.participant->str());
  if (filter.entry_type)
    entries.bind(binding++, *filter.entry_type);
  if (filter.from_ms)
    entries.bind(binding++, *filter.from_ms);
  if (filter.to_ms)
    entries.bind(binding++, *filter.to_ms);
  struct SearchCandidate {
    std::string kind;
    std::string id;
    std::int64_t occurred_at_ms{};
  };
  std::vector<SearchCandidate> entry_candidates;
  while (entries.step() &&
         entry_candidates.size() < chronicle_search_maximum_items) {
    entry_candidates.push_back({.kind = "entry",
                                .id = entries.column_text(0),
                                .occurred_at_ms = entries.column_int64(2)});
  }
  std::vector<SearchCandidate> memory_candidates;
  const bool include_memories = filter.presentation == "recall" &&
                                !filter.entry_type && !filter.participant &&
                                !filter.from_ms && !filter.to_ms;
  if (include_memories) {
    auto memories = connection.prepare(
        "SELECT m.memory_id,m.created_at_ms FROM memory m WHERE "
        "m.status='confirmed' AND (m.expires_at_ms IS NULL OR "
        "m.expires_at_ms>?) "
        "AND instr(m.text,?)>0 AND ((m.visibility='shared' AND "
        "m.sensitivity='ordinary') OR (m.visibility='self_only' AND EXISTS "
        "(SELECT 1 FROM memory_subject s WHERE s.memory_id=m.memory_id AND "
        "s.subject_type='user' AND s.subject_id=?))) ORDER BY m.created_at_ms "
        "DESC,m.memory_id DESC LIMIT ?");
    memories.bind(1, now_ms);
    memories.bind(2, match.empty() ? std::string_view{}
                                   : std::string_view{filter.query});
    memories.bind(3, viewer.str());
    memories.bind(4, static_cast<std::int64_t>(chronicle_search_maximum_items));
    while (memories.step() &&
           memory_candidates.size() < chronicle_search_maximum_items) {
      memory_candidates.push_back({.kind = "memory",
                                   .id = memories.column_text(0),
                                   .occurred_at_ms = memories.column_int64(1)});
    }
  }

  std::vector<SearchCandidate> selected;
  selected.reserve(chronicle_search_maximum_items);
  if (match.empty()) {
    std::size_t entry_index{};
    std::size_t memory_index{};
    while (selected.size() < chronicle_search_maximum_items &&
           (entry_index < entry_candidates.size() ||
            memory_index < memory_candidates.size())) {
      const bool take_entry =
          memory_index == memory_candidates.size() ||
          (entry_index < entry_candidates.size() &&
           (entry_candidates[entry_index].occurred_at_ms >
                memory_candidates[memory_index].occurred_at_ms ||
            (entry_candidates[entry_index].occurred_at_ms ==
                 memory_candidates[memory_index].occurred_at_ms &&
             entry_candidates[entry_index].id >
                 memory_candidates[memory_index].id)));
      selected.push_back(take_entry ? entry_candidates[entry_index++]
                                    : memory_candidates[memory_index++]);
    }
  } else {
    const auto reserved_memories =
        std::min(memory_candidates.size(), chronicle_search_page_size);
    const auto entry_count =
        std::min(entry_candidates.size(),
                 chronicle_search_maximum_items - reserved_memories);
    selected.insert(selected.end(), entry_candidates.begin(),
                    entry_candidates.begin() +
                        static_cast<std::ptrdiff_t>(entry_count));
    const auto memory_count =
        std::min(memory_candidates.size(),
                 chronicle_search_maximum_items - selected.size());
    selected.insert(selected.end(), memory_candidates.begin(),
                    memory_candidates.begin() +
                        static_cast<std::ptrdiff_t>(memory_count));
  }

  std::size_t position{};
  for (const auto &candidate : selected) {
    auto item = connection.prepare(
        "INSERT INTO "
        "chronicle_search_item(cursor_id,position,item_kind,item_id,"
        "rank_value,occurred_at_ms) VALUES (?,?,?,?,0,?)");
    item.bind(1, cursor_id);
    item.bind(2, static_cast<std::int64_t>(position++));
    item.bind(3, candidate.kind);
    item.bind(4, candidate.id);
    item.bind(5, candidate.occurred_at_ms);
    item.execute();
  }
  if (position > chronicle_search_page_size) {
    auto scope = connection.prepare(
        "SELECT guild_id,primary_channel_id FROM guild_config LIMIT 1");
    if (!scope.step())
      throw std::runtime_error{"Chronicle search scope is unavailable."};
    auto token = connection.prepare(
        "INSERT INTO interaction_token(token_id,token_version,interaction_kind,"
        "action,entity_type,entity_id,expected_user_id,guild_id,channel_id,"
        "state,"
        "expires_at_ms,idempotency_key,created_at_ms,expected_entity_revision) "
        "VALUES "
        "(?,1,'button','chronicle.search.next','chronicle_search_cursor',"
        "?,?,?,?, 'active',?,?,?,1)");
    token.bind(1, cursor_id);
    token.bind(2, cursor_id);
    token.bind(3, viewer.str());
    token.bind(4, scope.column_text(0));
    token.bind(5, scope.column_text(1));
    token.bind(6, now_ms + chronicle_search_cursor_lifetime_ms);
    token.bind(7, "token:search:" + cursor_id);
    token.bind(8, now_ms);
    token.execute();
  }
  transaction.commit();
  lock.unlock();
  auto result = search_page(viewer, cursor_id, 0, now_ms);
  if (position > chronicle_search_page_size)
    result.navigation_token_id = cursor_id;
  return result;
}

ChronicleSearchPage SqliteChronicleSessionRepository::search_page(
    const DiscordSnowflake &viewer, const std::string_view cursor_id,
    const std::size_t page, const std::int64_t now_ms) {
  require_uuid(cursor_id);
  if (page > chronicle_search_maximum_items / chronicle_search_page_size)
    throw std::invalid_argument{"Invalid Chronicle search page."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  if (!opted_in(connection, viewer))
    return {};
  auto cursor = connection.prepare(
      "SELECT c.presentation,(SELECT count(*) FROM chronicle_search_item i "
      "WHERE i.cursor_id=c.cursor_id) FROM chronicle_search_cursor c WHERE "
      "c.cursor_id=? AND c.viewer_user_id=? AND c.expires_at_ms>?");
  cursor.bind(1, cursor_id);
  cursor.bind(2, viewer.str());
  cursor.bind(3, now_ms);
  if (!cursor.step())
    return {};
  ChronicleSearchPage result{
      .cursor_id = std::string{cursor_id},
      .page = page,
      .total = static_cast<std::size_t>(cursor.column_int64(1)),
      .items = {},
      .navigation_token_id = std::nullopt,
      .presentation = cursor.column_text(0)};
  auto items = connection.prepare(
      "SELECT i.item_kind,i.item_id,i.occurred_at_ms FROM "
      "chronicle_search_item i "
      "JOIN chronicle_search_cursor c ON c.cursor_id=i.cursor_id WHERE "
      "c.cursor_id=? AND c.viewer_user_id=? AND c.expires_at_ms>? AND "
      "i.position>=? AND i.position<? ORDER BY i.position");
  items.bind(1, cursor_id);
  items.bind(2, viewer.str());
  items.bind(3, now_ms);
  const auto first = page * chronicle_search_page_size;
  items.bind(4, static_cast<std::int64_t>(first));
  items.bind(5, static_cast<std::int64_t>(first + chronicle_search_page_size));
  while (items.step()) {
    const auto kind = items.column_text(0);
    const auto id = items.column_text(1);
    if (kind == "entry") {
      auto entry = connection.prepare(
          "SELECT title,body,occurred_at_ms FROM chronicle_entry WHERE "
          "entry_id=? AND status='canon' AND visibility='shared'");
      entry.bind(1, id);
      if (entry.step())
        result.items.push_back({.item_id = id,
                                .title = entry.column_text(0),
                                .excerpt = excerpt(entry.column_text(1)),
                                .occurred_at_ms = entry.column_int64(2)});
    } else {
      auto memory = connection.prepare(
          "SELECT m.text,m.created_at_ms FROM memory m JOIN memory_subject s "
          "ON s.memory_id=m.memory_id AND s.subject_type='user' WHERE "
          "m.memory_id=? AND m.status='confirmed' AND (m.expires_at_ms IS NULL "
          "OR m.expires_at_ms>?) AND ((m.visibility='shared' AND "
          "m.sensitivity='ordinary') OR (m.visibility='self_only' AND "
          "s.subject_id=?))");
      memory.bind(1, id);
      memory.bind(2, now_ms);
      memory.bind(3, viewer.str());
      if (memory.step())
        result.items.push_back({.item_id = id,
                                .title = "Explicit memory",
                                .excerpt = excerpt(memory.column_text(0)),
                                .occurred_at_ms = memory.column_int64(1)});
    }
  }
  return result;
}

ChronicleSearchPage SqliteChronicleSessionRepository::advance_search(
    const DiscordSnowflake &viewer, const DiscordSnowflake &guild_id,
    const DiscordSnowflake &channel_id, const std::string_view token_id,
    std::string next_token_id, const std::int64_t now_ms) {
  require_uuid(token_id);
  require_uuid(next_token_id);
  require_context(guild_id, channel_id, viewer, now_ms);
  std::unique_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (!opted_in(connection, viewer)) {
    transaction.commit();
    return {};
  }
  auto token = connection.prepare(
      "SELECT t.entity_id,t.expected_entity_revision FROM interaction_token t "
      "JOIN chronicle_search_cursor c ON c.cursor_id=t.entity_id WHERE "
      "t.token_id=? AND t.token_version=1 AND t.interaction_kind='button' AND "
      "t.action='chronicle.search.next' AND "
      "t.entity_type='chronicle_search_cursor' AND t.expected_user_id=? AND "
      "t.guild_id=? AND t.channel_id=? AND t.state='active' AND "
      "t.expires_at_ms>? AND c.expires_at_ms>?");
  token.bind(1, token_id);
  token.bind(2, viewer.str());
  token.bind(3, guild_id.str());
  token.bind(4, channel_id.str());
  token.bind(5, now_ms);
  token.bind(6, now_ms);
  if (!token.step()) {
    transaction.commit();
    return {};
  }
  const auto cursor_id = token.column_text(0);
  const auto page = static_cast<std::size_t>(token.column_int64(1));
  auto consume = connection.prepare(
      "UPDATE interaction_token SET state='used',used_at_ms=? WHERE token_id=? "
      "AND state='active'");
  consume.bind(1, now_ms);
  consume.bind(2, token_id);
  consume.execute();
  if (connection.changes() != 1) {
    transaction.commit();
    return {};
  }
  auto count = connection.prepare(
      "SELECT count(*) FROM chronicle_search_item WHERE cursor_id=?");
  count.bind(1, cursor_id);
  if (!count.step())
    throw std::runtime_error{"Chronicle search count failed."};
  const auto total = static_cast<std::size_t>(count.column_int64(0));
  const bool has_next = (page + 1) * chronicle_search_page_size < total;
  if (has_next) {
    auto next = connection.prepare(
        "INSERT INTO interaction_token(token_id,token_version,interaction_kind,"
        "action,entity_type,entity_id,expected_user_id,guild_id,channel_id,"
        "state,"
        "expires_at_ms,idempotency_key,created_at_ms,expected_entity_revision) "
        "SELECT ?,1,'button','chronicle.search.next','chronicle_search_cursor',"
        "c.cursor_id,c.viewer_user_id,?,?, 'active',c.expires_at_ms,?,?,? FROM "
        "chronicle_search_cursor c WHERE c.cursor_id=?");
    next.bind(1, next_token_id);
    next.bind(2, guild_id.str());
    next.bind(3, channel_id.str());
    next.bind(4, "token:search:" + next_token_id);
    next.bind(5, now_ms);
    next.bind(6, static_cast<std::int64_t>(page + 1));
    next.bind(7, cursor_id);
    next.execute();
  }
  transaction.commit();
  lock.unlock();
  auto result = search_page(viewer, cursor_id, page, now_ms);
  if (has_next)
    result.navigation_token_id = std::move(next_token_id);
  return result;
}

bool SqliteChronicleSessionRepository::set_anniversary_reminders(
    const DiscordSnowflake &user_id, const bool enabled,
    const std::int64_t now_ms) {
  if (!user_id.is_set() || now_ms < 0)
    throw std::invalid_argument{"Invalid anniversary preference request."};
  const std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE user_preference SET anniversary_reminders_enabled=?,"
      "updated_at_ms=max(?,updated_at_ms) WHERE user_id=? AND "
      "chronicle_opt_in=1 AND anniversary_reminders_enabled<>?");
  update.bind(1, static_cast<std::int64_t>(enabled));
  update.bind(2, now_ms);
  update.bind(3, user_id.str());
  update.bind(4, static_cast<std::int64_t>(enabled));
  update.execute();
  return context_->connection().changes() == 1;
}

bool SqliteChronicleSessionRepository::queue_anniversary_scan(
    const ScheduledJobEnqueue &job, const AnniversaryScanJobPayload &payload,
    const std::string_view correlation_id) {
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto existing = context_->connection().prepare(
      "SELECT job_type FROM scheduled_job WHERE idempotency_key=?");
  existing.bind(1, job.idempotency_key);
  if (existing.step()) {
    if (existing.column_text(0) != anniversary_scan_job_type)
      throw std::runtime_error{"Anniversary job idempotency conflict."};
    transaction.commit();
    return false;
  }
  const auto created = detail::insert_job_uncommitted(
      context_->connection(), job,
      detail::encode_anniversary_scan_payload(payload, correlation_id,
                                              std::nullopt));
  transaction.commit();
  return created;
}

AnniversaryScanResult SqliteChronicleSessionRepository::run_anniversary_scan(
    const ClaimedScheduledJob &job, const std::string_view timezone,
    const bool test_run, const std::int64_t now_ms,
    PersistentIdGenerator &ids) {
  const auto *payload = std::get_if<AnniversaryScanJobPayload>(&job.payload);
  if (payload == nullptr)
    return anniversary_result(WorkMutationStatus::invalid_state);
  using namespace std::chrono;
  const auto *zone = locate_zone(std::string{timezone});
  const sys_time<milliseconds> now{milliseconds{now_ms}};
  const auto local_day = floor<days>(zoned_time{zone, now}.get_local_time());
  const year_month_day today{local_day};
  const auto format_date = [](const year_month_day value) {
    std::ostringstream stream;
    stream << static_cast<int>(value.year()) << '-' << std::setfill('0')
           << std::setw(2) << static_cast<unsigned>(value.month()) << '-'
           << std::setw(2) << static_cast<unsigned>(value.day());
    return stream.str();
  };
  const auto local_date = format_date(today);
  const bool effective_test = test_run || payload->test_run;
  const auto next_due = next_anniversary_scan_ms(now_ms, timezone);
  const sys_time<milliseconds> next_time{milliseconds{next_due}};
  const year_month_day next_date{
      floor<days>(zoned_time{zone, next_time}.get_local_time())};

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto claim = connection.prepare(
      "SELECT 1 FROM scheduled_job WHERE job_id=? AND state='claimed' "
      "AND lease_owner=? AND lease_token=?");
  claim.bind(1, job.job_id);
  claim.bind(2, job.lease_owner);
  claim.bind(3, job.lease_token);
  if (!claim.step()) {
    const auto result = claim_status(connection, job);
    transaction.commit();
    return anniversary_result(result);
  }

  std::optional<std::string> selected_id;
  std::string selected_title;
  std::string selected_body;
  DiscordSnowflake selected_guild;
  DiscordSnowflake selected_channel;
  if (effective_test || payload->local_date == local_date) {
    auto candidates = connection.prepare(
        effective_test
            ? "SELECT e.entry_id,e.title,e.body,e.occurred_at_ms,"
              "e.source_guild_id,e.source_channel_id FROM chronicle_entry e "
              "WHERE e.status='canon' AND e.visibility='shared' AND EXISTS "
              "(SELECT 1 FROM chronicle_tag t WHERE t.entry_id=e.entry_id AND "
              "t.tag='owner-test') AND EXISTS (SELECT 1 FROM "
              "chronicle_participant cp WHERE cp.entry_id=e.entry_id) AND "
              "NOT EXISTS (SELECT 1 FROM chronicle_participant cp LEFT JOIN "
              "user_preference p ON p.user_id=cp.user_id WHERE "
              "cp.entry_id=e.entry_id AND (p.user_id IS NULL OR "
              "p.chronicle_opt_in=0 OR p.anniversary_reminders_enabled=0 OR "
              "(p.quiet_until_ms IS NOT NULL AND p.quiet_until_ms>?))) "
              "ORDER BY e.occurred_at_ms DESC,e.entry_id DESC"
            : "SELECT e.entry_id,e.title,e.body,e.occurred_at_ms,"
              "e.source_guild_id,e.source_channel_id FROM chronicle_entry e "
              "WHERE e.status='canon' AND e.visibility='shared' AND NOT EXISTS "
              "(SELECT 1 FROM chronicle_tag t WHERE t.entry_id=e.entry_id AND "
              "t.tag='owner-test') AND EXISTS (SELECT 1 FROM "
              "chronicle_participant "
              "cp WHERE cp.entry_id=e.entry_id) AND NOT EXISTS (SELECT 1 FROM "
              "chronicle_participant cp LEFT JOIN user_preference p ON "
              "p.user_id=cp.user_id WHERE cp.entry_id=e.entry_id AND "
              "(p.user_id IS NULL OR p.chronicle_opt_in=0 OR "
              "p.anniversary_reminders_enabled=0 OR "
              "(p.quiet_until_ms IS NOT NULL AND p.quiet_until_ms>?))) "
              "ORDER BY e.occurred_at_ms DESC,e.entry_id DESC");
    candidates.bind(1, now_ms);
    while (candidates.step()) {
      const auto occurred =
          sys_time<milliseconds>{milliseconds{candidates.column_int64(3)}};
      const year_month_day anniversary_date{
          floor<days>(zoned_time{zone, occurred}.get_local_time())};
      if (!effective_test && (anniversary_date.month() != today.month() ||
                              anniversary_date.day() != today.day() ||
                              anniversary_date.year() >= today.year()))
        continue;
      auto delivered = connection.prepare(
          "SELECT 1 FROM chronicle_anniversary_delivery WHERE "
          "(entry_id=? AND local_year=? AND is_test=?) OR "
          "(local_date=? AND is_test=?)");
      delivered.bind(1, candidates.column_text(0));
      delivered.bind(2,
                     static_cast<std::int64_t>(static_cast<int>(today.year())));
      delivered.bind(3, static_cast<std::int64_t>(effective_test));
      delivered.bind(4, local_date);
      delivered.bind(5, static_cast<std::int64_t>(effective_test));
      if (delivered.step())
        continue;
      selected_id = candidates.column_text(0);
      selected_title = candidates.column_text(1);
      selected_body = candidates.column_text(2);
      selected_guild = DiscordSnowflake::parse(candidates.column_text(4));
      selected_channel = DiscordSnowflake::parse(candidates.column_text(5));
      break;
    }
  }

  bool wake_outbox = false;
  if (selected_id) {
    const auto outbox_id = ids.next_id();
    const auto delivery_id = ids.next_id();
    const auto event_id = ids.next_id();
    insert_public_outbox(
        connection, outbox_id, selected_guild, selected_channel,
        "chronicle_anniversary", *selected_id,
        std::string{"outbox:anniversary:"} + (effective_test ? "test:" : "") +
            local_date,
        now_ms, job.correlation_id, event_id,
        InteractionMessage{.content =
                               (effective_test
                                    ? "**[TEST DATA — ANNIVERSARY]**\n"
                                    : "**From the Chronicle, on this day**\n") +
                               std::string{"**"} + selected_title + "**\n" +
                               excerpt(selected_body, 600),
                           .embed = std::nullopt,
                           .buttons = {},
                           .allowed_user_mentions = {}});
    auto delivery = connection.prepare(
        "INSERT INTO chronicle_anniversary_delivery(delivery_id,entry_id,"
        "local_year,local_date,is_test,outbox_id,created_at_ms) VALUES "
        "(?,?,?,?,?,?,?)");
    delivery.bind(1, delivery_id);
    delivery.bind(2, *selected_id);
    delivery.bind(3, static_cast<std::int64_t>(static_cast<int>(today.year())));
    delivery.bind(4, local_date);
    delivery.bind(5, static_cast<std::int64_t>(effective_test));
    delivery.bind(6, outbox_id);
    delivery.bind(7, now_ms);
    delivery.execute();
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        make_event(
            event_id, "chronicle.anniversary_delivered.v1", "chronicle_entry",
            *selected_id, std::nullopt, selected_guild, selected_channel,
            now_ms, job.correlation_id,
            "event:anniversary:" + std::string{effective_test ? "test:" : ""} +
                local_date,
            Json{{"test_run", effective_test}, {"local_date", local_date}},
            job.causation_event_id)));
    wake_outbox = true;
  }

  if (!effective_test) {
    const auto next_job_id = ids.next_id();
    const auto next_key = "job:anniversary:" + format_date(next_date);
    auto existing = connection.prepare(
        "SELECT job_type FROM scheduled_job WHERE idempotency_key=?");
    existing.bind(1, next_key);
    if (existing.step()) {
      if (existing.column_text(0) != anniversary_scan_job_type)
        throw std::runtime_error{"Anniversary job idempotency conflict."};
    } else {
      static_cast<void>(detail::insert_job_uncommitted(
          connection,
          ScheduledJobEnqueue{
              .job_id = next_job_id,
              .job_type = std::string{anniversary_scan_job_type},
              .aggregate_type = "chronicle_anniversary",
              .aggregate_id = format_date(next_date),
              .due_at_ms = next_due,
              .max_attempts = 5,
              .idempotency_key = next_key,
              .created_at_ms = now_ms,
          },
          detail::encode_anniversary_scan_payload(
              AnniversaryScanJobPayload{.local_date = format_date(next_date),
                                        .test_run = false},
              job.correlation_id, job.causation_event_id)));
    }
  }
  complete_claim(connection, job, now_ms);
  transaction.commit();
  return {.status = WorkMutationStatus::applied,
          .wake_outbox = wake_outbox,
          .next_due_at_ms =
              effective_test ? std::nullopt : std::optional{next_due}};
}

} // namespace sanguinius::persistence
