#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"

#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/tarot_integration.hpp"
#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] bool blank(const std::string_view value) {
  return value.empty() ||
         std::ranges::all_of(value, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

[[nodiscard]] const char *visibility_name(const TarotVisibility value) {
  return value == TarotVisibility::public_result ? "public" : "private";
}

[[nodiscard]] TarotVisibility visibility_value(const std::string_view value) {
  if (value == "public")
    return TarotVisibility::public_result;
  if (value == "private")
    return TarotVisibility::private_result;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored House visibility is invalid."};
}

[[nodiscard]] const char *authority_name(const HouseResolutionAuthority value) {
  switch (value) {
  case HouseResolutionAuthority::draw:
    return "draw";
  case HouseResolutionAuthority::public_draw:
    return "public_draw";
  case HouseResolutionAuthority::owner:
    return "owner";
  }
  return "owner";
}

enum class SettlementCause { observed_draw, owner_resolution, deadline };

[[nodiscard]] const char *
resolution_authority_name(const SettlementCause cause,
                          const HouseResolutionAuthority authority) {
  if (cause == SettlementCause::deadline)
    return "deadline";
  if (cause == SettlementCause::owner_resolution)
    return "owner";
  return authority_name(authority);
}

[[nodiscard]] const char *action_kind_name(const SettlementCause cause) {
  switch (cause) {
  case SettlementCause::observed_draw:
    return "automatic_observation";
  case SettlementCause::owner_resolution:
    return "owner_resolution";
  case SettlementCause::deadline:
    return "deadline";
  }
  return "deadline";
}

[[nodiscard]] HouseResolutionAuthority
authority_value(const std::string_view value) {
  if (value == "draw")
    return HouseResolutionAuthority::draw;
  if (value == "public_draw")
    return HouseResolutionAuthority::public_draw;
  if (value == "owner")
    return HouseResolutionAuthority::owner;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored House authority is invalid."};
}

[[nodiscard]] HouseWagerState state_value(const std::string_view value) {
  if (value == "accepted_funded")
    return HouseWagerState::accepted_funded;
  if (value == "resolved")
    return HouseWagerState::resolved;
  if (value == "void_refunded")
    return HouseWagerState::void_refunded;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored House state is invalid."};
}

[[nodiscard]] std::optional<HouseResult> result_value(SqliteStatement &query,
                                                      const int column) {
  if (query.column_is_null(column))
    return std::nullopt;
  const auto value = query.column_text(column);
  if (value == "win")
    return HouseResult::win;
  if (value == "loss")
    return HouseResult::loss;
  if (value == "void")
    return HouseResult::void_wager;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored House result is invalid."};
}

[[nodiscard]] HouseWagerRecord record_from(SqliteStatement &query) {
  return {.wager_id = query.column_text(0),
          .user_id = DiscordSnowflake::parse(query.column_text(1)),
          .template_slug = query.column_text(2),
          .catalog_version = query.column_text(3),
          .proposition = query.column_text(4),
          .choice_slug = query.column_text(5),
          .choice_label = query.column_text(6),
          .odds_numerator = query.column_int64(7),
          .odds_denominator = query.column_int64(8),
          .stake = query.column_int64(9),
          .profit = query.column_int64(10),
          .visibility = visibility_value(query.column_text(11)),
          .authority = authority_value(query.column_text(12)),
          .state = state_value(query.column_text(13)),
          .result = result_value(query, 14),
          .accepted_at_ms = query.column_int64(15),
          .outcome_due_at_ms = query.column_int64(16),
          .terminal_cooldown_ms = query.column_int64(17),
          .recovery = query.column_int64(18) != 0,
          .is_test = query.column_int64(19) != 0};
}

[[nodiscard]] std::optional<HouseWagerRecord>
load_wager(SqliteConnection &connection, const std::string_view wager_id) {
  auto query = connection.prepare(
      "SELECT wager_id,user_id,template_slug,catalog_version,proposition,"
      "choice_slug,choice_label,odds_numerator,odds_denominator,stake,profit,"
      "visibility,authority,state,result,accepted_at_ms,outcome_due_at_ms,"
      "terminal_cooldown_ms,recovery,is_test FROM tarot_house_wager WHERE "
      "wager_id=?");
  query.bind(1, wager_id);
  if (!query.step())
    return std::nullopt;
  auto result = record_from(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate House wager identity."};
  return result;
}

[[nodiscard]] std::string system_account(SqliteConnection &connection,
                                         const std::string_view kind) {
  auto query = connection.prepare(
      "SELECT account_id FROM tarot_account WHERE account_kind=?");
  query.bind(1, kind);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot system account is missing."};
  const auto result = query.column_text(0);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot system account."};
  return result;
}

[[nodiscard]] std::string human_account(SqliteConnection &connection,
                                        const DiscordSnowflake &user_id) {
  auto query = connection.prepare(
      "SELECT account_id FROM tarot_account WHERE account_kind='HUMAN' "
      "AND user_id=?");
  query.bind(1, user_id.str());
  if (!query.step())
    throw std::invalid_argument{"A House player must have a Fate account."};
  return query.column_text(0);
}

[[nodiscard]] std::int64_t account_balance(SqliteConnection &connection,
                                           const std::string_view account_id) {
  auto query = connection.prepare(
      "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post "
      "JOIN tarot_transaction tx ON tx.transaction_id=post.transaction_id "
      "WHERE post.account_id=? AND tx.state='committed'");
  query.bind(1, account_id);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot balance query failed."};
  return query.column_int64(0);
}

[[nodiscard]] std::string next_id(const std::function<std::string()> &factory) {
  if (!factory)
    throw std::invalid_argument{"House ID factory is required."};
  auto result = factory();
  if (!valid_uuid_v4(result))
    throw std::invalid_argument{"House identities must be UUIDv4 values."};
  return result;
}

[[nodiscard]] std::int64_t
maximum_profit(const HouseTemplateDefinition &definition) {
  std::int64_t maximum{};
  for (const auto &choice : definition.choices) {
    for (const auto stake : definition.allowed_stakes) {
      const auto scaled = stake * choice.profit_numerator;
      if (scaled % choice.profit_denominator != 0)
        throw std::invalid_argument{"House catalog payout is not integral."};
      maximum = std::max(maximum, scaled / choice.profit_denominator);
    }
  }
  return maximum;
}

void insert_event(SqliteConnection &connection, const std::string_view event_id,
                  const std::string_view type,
                  const std::string_view aggregate_id,
                  const TarotInvocation &call,
                  const std::string_view idempotency_key,
                  const nlohmann::json &payload) {
  auto insert = connection.prepare(
      "INSERT INTO "
      "event_journal(event_id,event_type,aggregate_type,aggregate_id,"
      "actor_user_id,guild_id,channel_id,source_message_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,causation_id,idempotency_key,payload_json)"
      " "
      "VALUES(?,?,'tarot_house_wager',?,?,?,?,NULL,?,?,?,NULL,?,?)");
  insert.bind(1, event_id);
  insert.bind(2, type);
  insert.bind(3, aggregate_id);
  insert.bind(4, call.user_id.str());
  insert.bind(5, call.guild_id.str());
  insert.bind(6, call.channel_id.str());
  insert.bind(7, call.now_ms);
  insert.bind(8, call.now_ms);
  insert.bind(9, call.correlation_id);
  insert.bind(10, idempotency_key);
  insert.bind(11, payload.dump());
  insert.execute();
}

void insert_posting(SqliteConnection &connection,
                    const std::string_view posting_id,
                    const std::string_view transaction_id,
                    const std::string_view account_id,
                    const std::int64_t amount,
                    const std::int64_t created_at_ms) {
  if (amount == 0)
    throw std::invalid_argument{"Zero-value House postings are forbidden."};
  auto insert = connection.prepare(
      "INSERT INTO tarot_posting(posting_id,transaction_id,account_id,amount,"
      "created_at_ms) VALUES(?,?,?,?,?)");
  insert.bind(1, posting_id);
  insert.bind(2, transaction_id);
  insert.bind(3, account_id);
  insert.bind(4, amount);
  insert.bind(5, created_at_ms);
  insert.execute();
}

void insert_transaction(
    SqliteConnection &connection, const std::string_view transaction_id,
    const std::string_view transaction_type, const std::int64_t posting_count,
    const std::string_view event_id, const std::string_view idempotency_key,
    const TarotInvocation &call, const bool is_test,
    const std::string_view reason,
    const std::optional<std::string_view> reversal_of = std::nullopt) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
      "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
      "is_test,reversal_of_transaction_id,created_at_ms,committed_at_ms) "
      "VALUES(?,?,'prepared',?,?,?,?,?, ?,?,?,NULL)");
  insert.bind(1, transaction_id);
  insert.bind(2, transaction_type);
  insert.bind(3, posting_count);
  insert.bind(4, event_id);
  insert.bind(5, idempotency_key);
  insert.bind(6, call.user_id.str());
  insert.bind(7, reason);
  insert.bind(8, is_test ? 1LL : 0LL);
  if (reversal_of)
    insert.bind(9, *reversal_of);
  else
    insert.bind_null(9);
  insert.bind(10, call.now_ms);
  insert.execute();
}

void commit_transaction(SqliteConnection &connection,
                        const std::string_view transaction_id,
                        const std::int64_t committed_at_ms) {
  auto update = connection.prepare(
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=? "
      "WHERE transaction_id=? AND state='prepared'");
  update.bind(1, committed_at_ms);
  update.bind(2, transaction_id);
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "House ledger transaction did not seal."};
}

void insert_observation(SqliteConnection &connection,
                        const std::string_view event_id,
                        const std::string_view event_type,
                        const TarotVisibility visibility, const bool is_test,
                        const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO tarot_integration_observation(source_event_id,"
      "event_type,visibility,is_test,state,attempts,next_attempt_at_ms,"
      "last_error,created_at_ms,processed_at_ms) "
      "VALUES(?,?,?,?, 'pending',0,?,NULL,?,NULL)");
  insert.bind(1, event_id);
  insert.bind(2, event_type);
  insert.bind(3, visibility_name(visibility));
  insert.bind(4, is_test ? 1LL : 0LL);
  insert.bind(5, now_ms);
  insert.bind(6, now_ms);
  insert.execute();
}

[[nodiscard]] std::optional<std::string> enqueue_public_flavor(
    SqliteConnection &connection, const HouseWagerRecord &wager,
    const std::string_view event_id, const TarotInvocation &call,
    const std::string_view phase, const std::string_view result,
    const std::function<std::string()> &factory) {
  if (wager.visibility != TarotVisibility::public_result)
    return std::nullopt;
  const auto outbox_id = next_id(factory);
  std::string content = wager.is_test ? "[TEST] " : "";
  if (phase == "funded") {
    content += "A member accepted the House augury " + wager.template_slug +
               ". Fate terms remain private.";
  } else {
    content += "The House augury " + wager.template_slug + " settled as " +
               std::string{result} + ". Fate terms remain private.";
  }
  auto message = text_message(std::move(content));
  message.allowed_user_mentions.clear();
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "tarot_house_wager",
      .aggregate_id = wager.wager_id,
      .target_guild_id = call.guild_id,
      .target_channel_id = call.channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = call.now_ms,
      .max_attempts = 5,
      .idempotency_key =
          "outbox:tarot-house:" + wager.wager_id + ":" + std::string{phase},
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = call.now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_payload(
          PublicOutboxPayload{
              .request = PublicMessageRequest{.guild_id = call.guild_id,
                                              .channel_id = call.channel_id,
                                              .message = std::move(message)}},
          call.correlation_id, std::string{event_id})));
  return outbox_id;
}

void insert_public_dependency(SqliteConnection &connection,
                              const std::string_view predecessor_outbox_id,
                              const std::string_view successor_outbox_id,
                              const std::string_view dependency_kind,
                              const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO tarot_public_outbox_dependency("
      "predecessor_outbox_id,successor_outbox_id,dependency_kind,created_at_ms) "
      "VALUES(?,?,?,?)");
  insert.bind(1, predecessor_outbox_id);
  insert.bind(2, successor_outbox_id);
  insert.bind(3, dependency_kind);
  insert.bind(4, now_ms);
  insert.execute();
}

void insert_terminal_dependencies(
    SqliteConnection &connection, const HouseWagerRecord &wager,
    const std::string_view terminal_outbox_id,
    const std::optional<std::string_view> source_draw_id,
    const std::int64_t now_ms) {
  auto funded = connection.prepare(
      "SELECT outbox_id FROM outbox_message WHERE idempotency_key=?");
  funded.bind(1, "outbox:tarot-house:" + wager.wager_id + ":funded");
  if (funded.step())
    insert_public_dependency(connection, funded.column_text(0),
                             terminal_outbox_id, "funded_before_terminal",
                             now_ms);
  if (!source_draw_id)
    return;
  auto draw = connection.prepare(
      "SELECT outbox_id FROM tarot_draw_public_delivery WHERE draw_id=?");
  draw.bind(1, *source_draw_id);
  if (draw.step())
    insert_public_dependency(connection, draw.column_text(0),
                             terminal_outbox_id, "draw_before_terminal",
                             now_ms);
}

void retire_public_offer_card(SqliteConnection &connection,
                              const std::string_view offer_id,
                              const DiscordSnowflake &guild_id,
                              const DiscordSnowflake &channel_id,
                              const std::int64_t now_ms,
                              const std::string_view correlation_id,
                              const std::string_view causation_event_id,
                              const bool is_test, const std::string_view reason,
                              const std::function<std::string()> &factory) {
  auto source = connection.prepare(
      "SELECT card.create_outbox_id,outbox.state,outbox.submission_started_at_"
      "ms FROM tarot_house_public_card card JOIN outbox_message outbox ON "
      "outbox.outbox_id=card.create_outbox_id WHERE card.offer_id=?");
  source.bind(1, offer_id);
  if (!source.step())
    return;
  const auto source_outbox_id = source.column_text(0);
  const auto source_state = source.column_text(1);
  if (source_state == "pending" ||
      (source_state == "claimed" && source.column_is_null(2))) {
    auto cancel = connection.prepare(
        "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_at_ms=max(?,created_at_"
        "ms),updated_at_ms=max(?,updated_at_ms),last_error_code=? WHERE "
        "outbox_id=? AND (state='pending' OR (state='claimed' AND "
        "submission_started_at_ms IS NULL))");
    cancel.bind(1, now_ms);
    cancel.bind(2, now_ms);
    cancel.bind(3, reason == "claimed" ? "house_offer_claimed"
                                       : "house_offer_expired");
    cancel.bind(4, source_outbox_id);
    cancel.execute();
    if (connection.changes() == 1)
      return;
  }
  if (source_state != "delivered" && source_state != "claimed")
    return;

  const auto outbox_id = next_id(factory);
  std::string content = is_test ? "[TEST] " : "";
  content += reason == "claimed"
                 ? "The Last Standard has been claimed; its Fate terms "
                   "remain private."
                 : "The Last Standard offer has closed unclaimed.";
  auto replacement = text_message(std::move(content));
  replacement.buttons.clear();
  replacement.allowed_user_mentions.clear();
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = "discord.message_edit.v1",
      .aggregate_type = "tarot_house_offer",
      .aggregate_id = std::string{offer_id},
      .target_guild_id = guild_id,
      .target_channel_id = channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = now_ms,
      .max_attempts = 20,
      .idempotency_key =
          "outbox:tarot-house-offer-edit:" + std::string{offer_id},
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_edit_payload(
          PublicEditOutboxPayload{
              .replacement =
                  PublicMessageRequest{.guild_id = guild_id,
                                       .channel_id = channel_id,
                                       .message = std::move(replacement)},
              .source_outbox_id = source_outbox_id,
              .wager_revision = 2},
          correlation_id, std::string{causation_event_id})));
  auto link = connection.prepare(
      "UPDATE tarot_house_public_card SET terminal_edit_outbox_id=? WHERE "
      "offer_id=? AND terminal_edit_outbox_id IS NULL");
  link.bind(1, outbox_id);
  link.bind(2, offer_id);
  link.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "House offer card was already retired."};
}

void schedule_deadline(SqliteConnection &connection,
                       const HousePlayRequest &request,
                       const std::string_view wager_id,
                       const std::int64_t due_at_ms) {
  const auto job_id = next_id(request.next_id);
  const ScheduledJobEnqueue job{
      .job_id = job_id,
      .job_type = std::string{tarot_house_deadline_job_type},
      .aggregate_type = "tarot_house_wager",
      .aggregate_id = std::string{wager_id},
      .due_at_ms = due_at_ms,
      .max_attempts = 10,
      .idempotency_key =
          "job:tarot-house:" + std::string{wager_id} + ":outcome",
      .created_at_ms = request.invocation.now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_house_deadline_payload(
          HouseDeadlineJobPayload{.wager_id = std::string{wager_id},
                                  .expected_revision = 1},
          request.invocation.correlation_id, std::nullopt)));
  auto link = connection.prepare(
      "INSERT INTO tarot_house_deadline(wager_id,job_id,phase,"
      "expected_revision,due_at_ms) VALUES(?,?,'outcome',1,?)");
  link.bind(1, wager_id);
  link.bind(2, job_id);
  link.bind(3, due_at_ms);
  link.execute();
}

void schedule_offer_expiry(SqliteConnection &connection,
                           const HouseWeeklyOfferRequest &request,
                           const std::string_view offer_id,
                           const std::string_view opened_event_id,
                           const std::int64_t due_at_ms) {
  const auto job_id = next_id(request.next_id);
  const ScheduledJobEnqueue job{
      .job_id = job_id,
      .job_type = std::string{tarot_house_offer_expiry_job_type},
      .aggregate_type = "tarot_house_offer",
      .aggregate_id = std::string{offer_id},
      .due_at_ms = due_at_ms,
      .max_attempts = 10,
      .idempotency_key =
          "job:tarot-house-offer:" + std::string{offer_id} + ":expiry",
      .created_at_ms = request.now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_house_offer_expiry_payload(
          HouseOfferExpiryJobPayload{.offer_id = std::string{offer_id}},
          request.job.correlation_id,
          std::optional<std::string>{opened_event_id})));
  auto link = connection.prepare(
      "INSERT INTO tarot_house_offer_deadline(offer_id,job_id,due_at_ms) "
      "VALUES(?,?,?)");
  link.bind(1, offer_id);
  link.bind(2, job_id);
  link.bind(3, due_at_ms);
  link.execute();
}

void complete_job_claim(SqliteConnection &connection,
                        const ClaimedScheduledJob &job,
                        const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state='completed',lease_owner=NULL,"
      "lease_token=NULL,lease_until_ms=NULL,updated_at_ms=max(?,updated_at_ms),"
      "completed_at_ms=max(?,created_at_ms),terminal_at_ms=max(?,created_at_ms)"
      ","
      "last_error_code=NULL WHERE job_id=? AND state='claimed' AND "
      "lease_owner=? AND lease_token=?");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, now_ms);
  update.bind(4, job.job_id);
  update.bind(5, job.lease_owner);
  update.bind(6, job.lease_token);
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "House deadline claim became stale."};
}

[[nodiscard]] HouseMutationResult
settle_uncommitted(SqliteConnection &connection, HouseWagerRecord wager,
                   const TarotInvocation &call, const HouseResult result,
                   const std::string_view reason, const SettlementCause cause,
                   const std::optional<std::string_view> source_draw_id,
                   const std::function<std::string()> &factory) {
  if (wager.state != HouseWagerState::accepted_funded)
    return {.status = HouseMutationStatus::invalid_state,
            .wager = wager,
            .event_types = {}};
  const auto event_id = next_id(factory);
  const auto transaction_id = next_id(factory);
  const auto transfer_id = next_id(factory);
  const auto resolution_id = next_id(factory);
  const auto action_id = next_id(factory);
  const auto total = wager.stake + wager.profit;
  const auto is_void = result == HouseResult::void_wager;
  const auto transaction_type = is_void ? "WAGER_REFUND" : "WAGER_PAYOUT";
  const auto event_type =
      is_void ? "tarot.house_voided.v1" : "tarot.house_resolved.v1";
  insert_event(connection, event_id, event_type, wager.wager_id, call,
               "tarot.house.settle:" + wager.wager_id,
               {{"wager_id", wager.wager_id},
                {"template", wager.template_slug},
                {"result", house_result_name(result)},
                {"visibility", visibility_name(wager.visibility)},
                {"is_test", wager.is_test}});

  std::int64_t posting_count{2};
  if (is_void && !wager.recovery)
    posting_count = 3;
  insert_transaction(connection, transaction_id, transaction_type,
                     posting_count, event_id,
                     "tarot.house.ledger:" + wager.wager_id + ":terminal", call,
                     wager.is_test, reason);
  const auto escrow = system_account(connection, "ESCROW");
  insert_posting(connection, next_id(factory), transaction_id, escrow, -total,
                 call.now_ms);
  if (result == HouseResult::win) {
    insert_posting(connection, next_id(factory), transaction_id,
                   human_account(connection, wager.user_id), total,
                   call.now_ms);
  } else if (result == HouseResult::loss) {
    insert_posting(connection, next_id(factory), transaction_id,
                   system_account(connection, "HOUSE"), total, call.now_ms);
  } else if (wager.recovery) {
    insert_posting(connection, next_id(factory), transaction_id,
                   system_account(connection, "MINT"), total, call.now_ms);
  } else {
    insert_posting(connection, next_id(factory), transaction_id,
                   human_account(connection, wager.user_id), wager.stake,
                   call.now_ms);
    insert_posting(connection, next_id(factory), transaction_id,
                   system_account(connection, "HOUSE"), wager.profit,
                   call.now_ms);
  }

  auto transfer = connection.prepare(
      "INSERT INTO tarot_house_transfer(transfer_id,wager_id,transfer_kind,"
      "transaction_id,created_at_ms) VALUES(?,?,?,?,?)");
  transfer.bind(1, transfer_id);
  transfer.bind(2, wager.wager_id);
  transfer.bind(3, is_void ? "refund" : "payout");
  transfer.bind(4, transaction_id);
  transfer.bind(5, call.now_ms);
  transfer.execute();
  auto resolution = connection.prepare(
      "INSERT INTO tarot_house_resolution(resolution_id,wager_id,result,"
      "authority,actor_user_id,reason,transaction_id,event_id,created_at_ms) "
      "VALUES(?,?,?,?,?,?,?,?,?)");
  resolution.bind(1, resolution_id);
  resolution.bind(2, wager.wager_id);
  resolution.bind(3, house_result_name(result));
  resolution.bind(4, resolution_authority_name(cause, wager.authority));
  if (cause == SettlementCause::owner_resolution)
    resolution.bind(5, call.user_id.str());
  else
    resolution.bind_null(5);
  resolution.bind(6, reason);
  resolution.bind(7, transaction_id);
  resolution.bind(8, event_id);
  resolution.bind(9, call.now_ms);
  resolution.execute();
  commit_transaction(connection, transaction_id, call.now_ms);

  if (call.now_ms >
      std::numeric_limits<std::int64_t>::max() - wager.terminal_cooldown_ms)
    throw std::overflow_error{"House terminal cooldown overflowed."};

  auto update = connection.prepare(
      "UPDATE tarot_house_wager SET state=?,result=?,terminal_reason=?,"
      "cooldown_until_ms=?,terminal_at_ms=?,terminal_event_id=?,revision=2 "
      "WHERE wager_id=? "
      "AND state='accepted_funded' AND revision=1");
  update.bind(1, is_void ? "void_refunded" : "resolved");
  update.bind(2, house_result_name(result));
  update.bind(3, reason);
  update.bind(4, call.now_ms + wager.terminal_cooldown_ms);
  update.bind(5, call.now_ms);
  update.bind(6, event_id);
  update.bind(7, wager.wager_id);
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "House wager settlement raced."};
  auto action = connection.prepare(
      "INSERT INTO tarot_house_action(action_id,wager_id,action_kind,"
      "actor_user_id,expected_revision,reason,created_at_ms) "
      "VALUES(?,?,?,?,1,?,?)");
  action.bind(1, action_id);
  action.bind(2, wager.wager_id);
  action.bind(3, action_kind_name(cause));
  if (cause == SettlementCause::owner_resolution)
    action.bind(4, call.user_id.str());
  else
    action.bind_null(4);
  action.bind(5, reason);
  action.bind(6, call.now_ms);
  action.execute();

  wager.state =
      is_void ? HouseWagerState::void_refunded : HouseWagerState::resolved;
  wager.result = result;
  insert_observation(connection, event_id, event_type, wager.visibility,
                     wager.is_test, call.now_ms);
  const auto public_outbox =
      enqueue_public_flavor(connection, wager, event_id, call, "terminal",
                            house_result_name(result), factory);
  if (public_outbox) {
    insert_terminal_dependencies(connection, wager, *public_outbox,
                                 source_draw_id, call.now_ms);
    auto card = connection.prepare(
        "UPDATE tarot_house_public_card SET outcome_outbox_id=? WHERE "
        "offer_id=(SELECT offer_id FROM tarot_house_wager WHERE wager_id=?) "
        "AND outcome_outbox_id IS NULL");
    card.bind(1, *public_outbox);
    card.bind(2, wager.wager_id);
    card.execute();
  }
  return {.status = HouseMutationStatus::applied,
          .wager = wager,
          .event_types = {std::string{event_type}}};
}

struct QualifyingDraw {
  std::string draw_id;
  std::int64_t drawn_at_ms{};
};

[[nodiscard]] std::optional<QualifyingDraw>
qualifying_draw(SqliteConnection &connection, const HouseWagerRecord &wager) {
  if (wager.authority == HouseResolutionAuthority::owner)
    return std::nullopt;
  auto query = connection.prepare(
      "SELECT draw.draw_id,draw.drawn_at_ms FROM tarot_card_draw draw JOIN "
      "discord_user drawer ON drawer.user_id=draw.user_id JOIN "
      "tarot_event_order draw_order ON draw_order.event_id=draw.event_id JOIN "
      "tarot_house_wager wager ON wager.wager_id=? JOIN tarot_event_order "
      "accepted_order ON accepted_order.event_id=wager.accepted_event_id "
      "WHERE draw.is_test=? AND "
      "draw.guild_id=wager.guild_id AND draw.channel_id=wager.channel_id AND "
      "draw_order.sequence_id>accepted_order.sequence_id AND "
      "draw.drawn_at_ms<=wager.outcome_due_at_ms AND "
      "((wager.authority='draw' AND draw.user_id=wager.user_id) OR "
      "(wager.authority='public_draw' AND draw.user_id<>wager.user_id AND "
      "drawer.is_bot=0 AND draw.visibility='public')) ORDER BY "
      "draw_order.sequence_id "
      "LIMIT 1");
  query.bind(1, wager.wager_id);
  query.bind(2, wager.is_test ? 1LL : 0LL);
  if (!query.step())
    return std::nullopt;
  return QualifyingDraw{.draw_id = query.column_text(0),
                        .drawn_at_ms = query.column_int64(1)};
}

[[nodiscard]] HouseResult observed_draw_outcome(const HouseWagerRecord &wager) {
  if (wager.authority == HouseResolutionAuthority::draw)
    return HouseResult::win;
  if (wager.authority == HouseResolutionAuthority::public_draw)
    return wager.choice_slug == "no" ? HouseResult::loss : HouseResult::win;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA,
                      "Owner-authority wager cannot use a draw outcome."};
}

[[nodiscard]] HouseResult deadline_outcome(SqliteConnection &connection,
                                           const HouseWagerRecord &wager) {
  if (qualifying_draw(connection, wager))
    return observed_draw_outcome(wager);
  if (wager.recovery || wager.authority == HouseResolutionAuthority::owner)
    return HouseResult::void_wager;
  if (wager.authority == HouseResolutionAuthority::public_draw &&
      wager.choice_slug == "no")
    return HouseResult::win;
  return HouseResult::loss;
}

[[nodiscard]] HouseMutationResult settle_qualifying_draw_uncommitted(
    SqliteConnection &connection, const HouseWagerRecord &wager,
    const QualifyingDraw &draw, const std::int64_t now_ms,
    const std::function<std::string()> &factory) {
  auto scope = connection.prepare(
      "SELECT guild_id,channel_id FROM tarot_house_wager WHERE wager_id=?");
  scope.bind(1, wager.wager_id);
  if (!scope.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House wager scope is missing."};
  const TarotInvocation call{
      .user_id = wager.user_id,
      .guild_id = DiscordSnowflake::parse(scope.column_text(0)),
      .channel_id = DiscordSnowflake::parse(scope.column_text(1)),
      .display_name = "House draw reconciliation",
      .interaction_idempotency_key =
          "tarot.house.draw:" + draw.draw_id + ":" + wager.wager_id,
      .correlation_id = "tarot-house-draw-reconciliation",
      .now_ms = std::max({now_ms, draw.drawn_at_ms, wager.accepted_at_ms})};
  return settle_uncommitted(connection, wager, call,
                            observed_draw_outcome(wager),
                            "Observed qualifying Tarot draw",
                            SettlementCause::observed_draw, draw.draw_id,
                            factory);
}

[[nodiscard]] HouseMutationStatus receipt_status(const std::string_view value) {
  if (value == "applied")
    return HouseMutationStatus::replay;
  if (value == "not_found")
    return HouseMutationStatus::not_found;
  if (value == "forbidden")
    return HouseMutationStatus::forbidden;
  if (value == "ineligible")
    return HouseMutationStatus::ineligible;
  if (value == "cooldown")
    return HouseMutationStatus::cooldown;
  if (value == "insufficient_funds")
    return HouseMutationStatus::insufficient_funds;
  if (value == "exposure_blocked")
    return HouseMutationStatus::exposure_blocked;
  if (value == "invalid_state")
    return HouseMutationStatus::invalid_state;
  return HouseMutationStatus::replay;
}

[[nodiscard]] std::string_view
receipt_status_name(const HouseMutationStatus status) {
  switch (status) {
  case HouseMutationStatus::applied:
    return "applied";
  case HouseMutationStatus::replay:
    return "replay";
  case HouseMutationStatus::not_found:
    return "not_found";
  case HouseMutationStatus::forbidden:
    return "forbidden";
  case HouseMutationStatus::ineligible:
    return "ineligible";
  case HouseMutationStatus::cooldown:
    return "cooldown";
  case HouseMutationStatus::insufficient_funds:
    return "insufficient_funds";
  case HouseMutationStatus::exposure_blocked:
    return "exposure_blocked";
  case HouseMutationStatus::invalid_state:
    return "invalid_state";
  }
  throw std::logic_error{"Unknown House mutation status."};
}

[[nodiscard]] std::string play_fingerprint(const HousePlayRequest &request) {
  return nlohmann::json{{"catalog_version", request.catalog_version},
                        {"channel_id", request.invocation.channel_id.str()},
                        {"choice_slug", request.choice_slug},
                        {"guild_id", request.invocation.guild_id.str()},
                        {"is_test", request.is_test},
                        {"offer_id", request.offer_id},
                        {"stake", request.stake},
                        {"template_slug", request.definition->slug},
                        {"user_id", request.invocation.user_id.str()},
                        {"visibility", visibility_name(request.visibility)}}
      .dump();
}

[[nodiscard]] std::string
resolution_fingerprint(const HouseResolveRequest &request) {
  std::string result{"resolve-v1|"};
  const auto append = [&result](const std::string_view value) {
    result += std::to_string(value.size());
    result.push_back(':');
    result.append(value);
  };
  append(request.wager_id);
  append(request.invocation.user_id.str());
  append(request.invocation.guild_id.str());
  append(request.invocation.channel_id.str());
  append(*request.observed_choice);
  append(house_result_name(request.result));
  append(request.reason);
  return result;
}

[[nodiscard]] std::string
cleanup_fingerprint(const HouseTestCleanupRequest &request) {
  std::string result{"test-cleanup-v1|"};
  const auto append = [&result](const std::string_view value) {
    result += std::to_string(value.size());
    result.push_back(':');
    result.append(value);
  };
  append(request.wager_id);
  append(request.invocation.user_id.str());
  append(request.invocation.guild_id.str());
  append(request.invocation.channel_id.str());
  append(request.reason);
  return result;
}

void save_receipt(SqliteConnection &connection, const HousePlayRequest &request,
                  const HouseMutationStatus status,
                  const std::optional<std::string_view> wager_id,
                  const std::string_view fingerprint) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_house_receipt(idempotency_key,wager_id,operation,"
      "status,request_fingerprint,created_at_ms) VALUES(?,?,'play',?,?,?)");
  insert.bind(1, request.invocation.interaction_idempotency_key);
  if (wager_id)
    insert.bind(2, *wager_id);
  else
    insert.bind_null(2);
  insert.bind(3, receipt_status_name(status));
  insert.bind(4, fingerprint);
  insert.bind(5, request.invocation.now_ms);
  insert.execute();
}

struct PlayerProjectionValue {
  std::int64_t wins{};
  std::int64_t losses{};
  std::int64_t win_streak{};
  std::int64_t loss_streak{};
  std::int64_t settled_house{};
  std::int64_t rebuilt_at_ms{};
  std::string last_event_id;
};

struct ExpectedPlayerProjection {
  std::size_t event_count{};
  std::unordered_map<std::string, PlayerProjectionValue> users;
};

[[nodiscard]] ExpectedPlayerProjection
expected_player_projection(SqliteConnection &connection) {
  ExpectedPlayerProjection expected;
  auto count = connection.prepare("SELECT count(*) FROM tarot_player_event");
  if (!count.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot player event count failed."};
  expected.event_count = static_cast<std::size_t>(count.column_int64(0));
  auto events = connection.prepare(
      "SELECT player.source_event_id,player.user_id,player.result,"
      "player.wager_kind,player.occurred_at_ms FROM tarot_player_event player "
      "JOIN tarot_event_order event_order ON event_order.event_id=player."
      "source_event_id WHERE player.is_test=0 ORDER BY player.user_id,"
      "event_order.sequence_id");
  while (events.step()) {
    auto &value = expected.users[events.column_text(1)];
    value.last_event_id = events.column_text(0);
    value.rebuilt_at_ms = events.column_int64(4);
    const auto result = events.column_text(2);
    if (result == "win") {
      ++value.wins;
      ++value.win_streak;
      value.loss_streak = 0;
    } else if (result == "loss") {
      ++value.losses;
      ++value.loss_streak;
      value.win_streak = 0;
    }
    if (events.column_text(3) == "house")
      ++value.settled_house;
  }
  return expected;
}

[[nodiscard]] TarotPlayerProjectionReport
check_player_projection_unlocked(SqliteConnection &connection) {
  const auto expected = expected_player_projection(connection);
  std::size_t actual_count{};
  std::size_t mismatches{};
  std::unordered_map<std::string, bool> seen;
  auto actual = connection.prepare(
      "SELECT user_id,wins,losses,current_win_streak,current_loss_streak,"
      "settled_house_wagers,last_event_id FROM tarot_player_stats");
  while (actual.step()) {
    ++actual_count;
    const auto user_id = actual.column_text(0);
    seen[user_id] = true;
    const auto found = expected.users.find(user_id);
    if (found == expected.users.end()) {
      ++mismatches;
      continue;
    }
    const auto &value = found->second;
    if (actual.column_int64(1) != value.wins ||
        actual.column_int64(2) != value.losses ||
        actual.column_int64(3) != value.win_streak ||
        actual.column_int64(4) != value.loss_streak ||
        actual.column_int64(5) != value.settled_house ||
        actual.column_is_null(6) ||
        actual.column_text(6) != value.last_event_id)
      ++mismatches;
  }
  for (const auto &[user_id, ignored] : expected.users) {
    static_cast<void>(ignored);
    if (!seen.contains(user_id))
      ++mismatches;
  }
  return {.valid = mismatches == 0,
          .event_count = expected.event_count,
          .projection_count = actual_count,
          .mismatch_count = mismatches};
}

} // namespace

SqliteTarotHouseRepository::SqliteTarotHouseRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite House context is required."};
}

void SqliteTarotHouseRepository::ensure_weekly_schedule(
    const std::int64_t now_ms, const std::int64_t due_at_ms,
    std::string catalog_version, std::string job_id) {
  if (now_ms < 0 || due_at_ms <= now_ms || catalog_version.empty() ||
      !valid_uuid_v4(job_id))
    throw std::invalid_argument{"House weekly schedule is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT job_id,job_type,state,payload_json FROM scheduled_job WHERE "
      "idempotency_key='job:tarot-house:friday-offer'");
  if (existing.step()) {
    if (existing.column_text(1) != tarot_house_weekly_offer_job_type)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "House weekly schedule version conflicts."};
    const auto state = existing.column_text(2);
    if (state == "dead" || state == "cancelled" || state == "completed") {
      auto rearm = connection.prepare(
          "UPDATE scheduled_job SET "
          "state='pending',attempt_count=0,due_at_ms=?,"
          "lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,"
          "last_error_code=NULL,completed_at_ms=NULL,terminal_at_ms=NULL,"
          "updated_at_ms=max(?,updated_at_ms) WHERE job_id=?");
      rearm.bind(1, due_at_ms);
      rearm.bind(2, due_at_ms);
      rearm.bind(3, existing.column_text(0));
      rearm.execute();
    }
    transaction.commit();
    return;
  }
  const ScheduledJobEnqueue job{
      .job_id = std::move(job_id),
      .job_type = std::string{tarot_house_weekly_offer_job_type},
      .aggregate_type = "tarot_house_offer",
      .aggregate_id = "friday-1800-america-new-york",
      .due_at_ms = due_at_ms,
      .max_attempts = 10,
      .idempotency_key = "job:tarot-house:friday-offer",
      .created_at_ms = now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_tarot_house_weekly_offer_payload(
          TarotHouseWeeklyOfferJobPayload{
              .schedule_key = "friday-1800-america-new-york",
              .catalog_version = std::move(catalog_version)},
          "tarot-house-weekly-offer", std::nullopt)));
  transaction.commit();
}

HouseWeeklyOfferResult SqliteTarotHouseRepository::handle_weekly_offer(
    const HouseWeeklyOfferRequest &request) {
  const auto *payload =
      std::get_if<TarotHouseWeeklyOfferJobPayload>(&request.job.payload);
  if (payload == nullptr || request.definition == nullptr ||
      !request.definition->scheduled || !request.next_id ||
      request.catalog_version != payload->catalog_version ||
      request.exposure_cap < 1)
    throw std::invalid_argument{"House weekly offer request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (request.now_ms < request.job.due_at_ms) {
    transaction.commit();
    return {.status = HouseWeeklyOfferStatus::deferred,
            .offer_id = std::nullopt,
            .outbox_created = false};
  }
  auto existing = connection.prepare(
      "SELECT offer_id,state FROM tarot_house_offer WHERE template_slug=? AND "
      "opens_at_ms=? AND is_test=?");
  existing.bind(1, request.definition->slug);
  existing.bind(2, request.job.due_at_ms);
  existing.bind(3, request.is_test ? 1LL : 0LL);
  if (existing.step()) {
    auto result = HouseWeeklyOfferResult{
        .status = HouseWeeklyOfferStatus::replay,
        .offer_id = existing.column_text(0),
        .outbox_created = existing.column_text(1) == "open"};
    transaction.commit();
    return result;
  }

  const auto reserved_profit = maximum_profit(*request.definition);
  std::optional<std::string> skip_reason;
  if (request.now_ms > request.job.due_at_ms + 15 * 60 * 1'000)
    skip_reason = "missed_slot";
  else if (!request.operational)
    skip_reason = "runtime_degraded";
  auto quiet = connection.prepare(
      "SELECT globally_disabled,quiet_until_ms FROM appearance_control_state "
      "WHERE singleton=1");
  if (!skip_reason && quiet.step() &&
      (quiet.column_int64(0) != 0 ||
       (!quiet.column_is_null(1) && quiet.column_int64(1) > request.now_ms)))
    skip_reason = "quiet_or_disabled";
  auto exposure = connection.prepare(
      "SELECT COALESCE((SELECT sum(profit) FROM tarot_house_wager WHERE "
      "is_test=? AND state='accepted_funded' AND "
      "recovery=0),0)+COALESCE((SELECT "
      "sum(reserved_profit) FROM tarot_house_offer WHERE is_test=? AND "
      "state='open'),0)");
  exposure.bind(1, request.is_test ? 1LL : 0LL);
  exposure.bind(2, request.is_test ? 1LL : 0LL);
  if (!exposure.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House offer exposure query failed."};
  if (!skip_reason &&
      exposure.column_int64(0) > request.exposure_cap - reserved_profit)
    skip_reason = "exposure_blocked";

  const auto offer_id = next_id(request.next_id);
  const auto event_id = next_id(request.next_id);
  const auto boundaries =
      request.is_test
          ? HouseWeeklyBoundaries{.closes_at_ms = request.job.due_at_ms +
                                                  30LL * 60 * 60 * 1'000,
                                  .resolution_due_at_ms =
                                      request.job.due_at_ms +
                                      request.definition->outcome_window_ms}
          : house_weekly_boundaries_ms(request.job.due_at_ms);
  const auto closes_at_ms = boundaries.closes_at_ms;
  const auto resolution_due_at_ms = boundaries.resolution_due_at_ms;
  auto offer = connection.prepare(
      "INSERT INTO tarot_house_offer(offer_id,catalog_version,template_slug,"
      "guild_id,channel_id,proposition,state,reserved_profit,opens_at_ms,"
      "closes_at_ms,resolution_due_at_ms,is_test,skip_reason,created_at_ms) "
      "VALUES(?,?,?,?,?,?,?, ?,?,?,?,?,?,?)");
  offer.bind(1, offer_id);
  offer.bind(2, request.catalog_version);
  offer.bind(3, request.definition->slug);
  offer.bind(4, request.scope.guild_id.str());
  offer.bind(5, request.scope.primary_channel_id.str());
  offer.bind(6, request.definition->proposition);
  offer.bind(7, skip_reason ? "skipped" : "open");
  offer.bind(8, skip_reason ? 0 : reserved_profit);
  offer.bind(9, request.job.due_at_ms);
  offer.bind(10, closes_at_ms);
  offer.bind(11, resolution_due_at_ms);
  offer.bind(12, request.is_test ? 1LL : 0LL);
  if (skip_reason)
    offer.bind(13, *skip_reason);
  else
    offer.bind_null(13);
  offer.bind(14, request.now_ms);
  offer.execute();

  const auto event_type = skip_reason ? "tarot.house_offer_skipped.v1"
                                      : "tarot.house_offer_opened.v1";
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      EventJournalEntry{
          .event_id = event_id,
          .event_type = event_type,
          .aggregate_type = "tarot_house_offer",
          .aggregate_id = offer_id,
          .actor_user_id = std::nullopt,
          .guild_id = request.scope.guild_id,
          .channel_id = request.scope.primary_channel_id,
          .source_message_id = std::nullopt,
          .occurred_at_ms = request.now_ms,
          .recorded_at_ms = request.now_ms,
          .correlation_id = request.job.correlation_id,
          .causation_id = std::nullopt,
          .idempotency_key = "tarot.house.offer.slot:" +
                             std::string{request.is_test ? "test:" : "live:"} +
                             std::to_string(request.job.due_at_ms),
          .payload_json =
              nlohmann::json{{"offer_id", offer_id},
                             {"template", request.definition->slug},
                             {"status", skip_reason ? "skipped" : "open"},
                             {"reason", skip_reason.value_or("")}}
                  .dump(),
      }));
  if (skip_reason) {
    transaction.commit();
    return {.status = HouseWeeklyOfferStatus::skipped,
            .offer_id = offer_id,
            .outbox_created = false};
  }

  schedule_offer_expiry(connection, request, offer_id, event_id, closes_at_ms);

  const auto token_id = next_id(request.next_id);
  auto control = connection.prepare(
      "INSERT INTO tarot_house_control(token_id,offer_id,wager_id,"
      "expected_user_id,expected_revision,action,state,created_at_ms,"
      "expires_at_ms) VALUES(?,?,NULL,NULL,1,'claim','active',?,?)");
  control.bind(1, token_id);
  control.bind(2, offer_id);
  control.bind(3, request.now_ms);
  control.bind(4, closes_at_ms);
  control.execute();

  const auto enqueue = [&](const std::string_view purpose,
                           const std::int64_t available_at_ms,
                           std::string content) {
    const auto outbox_id = next_id(request.next_id);
    const OutboxEnqueue outbox{
        .outbox_id = outbox_id,
        .kind = std::string{public_discord_outbox_kind},
        .aggregate_type = "tarot_house_offer",
        .aggregate_id = offer_id,
        .target_guild_id = request.scope.guild_id,
        .target_channel_id = request.scope.primary_channel_id,
        .target_user_id = std::nullopt,
        .available_at_ms = available_at_ms,
        .max_attempts = 5,
        .idempotency_key =
            "outbox:tarot-house-offer:" + offer_id + ":" + std::string{purpose},
        .provider_nonce = discord_nonce_from_uuid(outbox_id),
        .created_at_ms = request.now_ms,
    };
    auto message = text_message(std::move(content));
    message.allowed_user_mentions.clear();
    message.buttons.push_back(
        {.custom_id = std::string{tarot_house_component_prefix} + token_id,
         .label = "Open sealed offer",
         .disabled = false,
         .style = ButtonStyle::primary});
    static_cast<void>(detail::insert_outbox_uncommitted(
        connection, outbox,
        detail::encode_public_payload(
            PublicOutboxPayload{
                .request =
                    PublicMessageRequest{.guild_id = request.scope.guild_id,
                                         .channel_id =
                                             request.scope.primary_channel_id,
                                         .message = std::move(message)}},
            request.job.correlation_id, event_id)));
    return outbox_id;
  };
  const auto prefix = request.is_test ? "[TEST] " : "";
  const auto create_outbox = enqueue("create", request.now_ms,
                                     std::string{prefix} +
                                         "The Last Standard is raised. Will "
                                         "game-night play remain underway at "
                                         "midnight Saturday? Claim privately "
                                         "with `/tarot house play` using offer "
                                         "reference `" +
                                         offer_id + "`.");
  const auto reminder_outbox = enqueue(
      "reminder", request.job.due_at_ms + 24LL * 60 * 60 * 1'000,
      std::string{prefix} +
          "The Last Standard remains open. Use its offer reference with "
          "`/tarot house play`; all Fate terms remain private.");
  auto card = connection.prepare(
      "INSERT INTO tarot_house_public_card(offer_id,create_outbox_id,"
      "reminder_outbox_id,terminal_edit_outbox_id,outcome_outbox_id,"
      "created_revision) VALUES(?,?,?,NULL,NULL,1)");
  card.bind(1, offer_id);
  card.bind(2, create_outbox);
  card.bind(3, reminder_outbox);
  card.execute();
  transaction.commit();
  return {.status = HouseWeeklyOfferStatus::created,
          .offer_id = offer_id,
          .outbox_created = true};
}

HouseOfferExpiryResult SqliteTarotHouseRepository::handle_offer_expiry(
    const HouseOfferExpiryRequest &request) {
  const auto *payload =
      std::get_if<HouseOfferExpiryJobPayload>(&request.job.payload);
  if (payload == nullptr ||
      request.job.job_type != tarot_house_offer_expiry_job_type ||
      !request.next_id)
    throw std::invalid_argument{"House offer-expiry payload is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto offer = connection.prepare(
      "SELECT offer.state,offer.closes_at_ms,offer.guild_id,offer.channel_id,"
      "offer.is_test,deadline.due_at_ms FROM tarot_house_offer offer JOIN "
      "tarot_house_offer_deadline deadline ON deadline.offer_id=offer.offer_id "
      "WHERE offer.offer_id=? AND deadline.job_id=?");
  offer.bind(1, payload->offer_id);
  offer.bind(2, request.job.job_id);
  if (!offer.step())
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "House offer-expiry link is invalid."};
  const auto closes_at_ms = offer.column_int64(1);
  const auto due_at_ms = offer.column_int64(5);
  if (due_at_ms != closes_at_ms || request.now_ms < due_at_ms)
    throw std::invalid_argument{"House offer expiry was claimed before due."};

  auto status = HouseOfferExpiryStatus::replay;
  if (offer.column_text(0) == "open") {
    auto close = connection.prepare(
        "UPDATE tarot_house_offer SET state='closed' WHERE offer_id=? AND "
        "state='open'");
    close.bind(1, payload->offer_id);
    close.execute();
    if (connection.changes() != 1)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT, "House offer expiry raced."};
    auto cancel_control = connection.prepare(
        "UPDATE tarot_house_control SET state='cancelled' WHERE offer_id=? "
        "AND state='active'");
    cancel_control.bind(1, payload->offer_id);
    cancel_control.execute();
    auto cancel_reminder = connection.prepare(
        "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_at_ms=max(?,created_at_"
        "ms),updated_at_ms=max(?,updated_at_ms),last_error_code='house_offer_"
        "expired' WHERE outbox_id=(SELECT reminder_outbox_id FROM "
        "tarot_house_public_card WHERE offer_id=?) AND (state='pending' OR "
        "(state='claimed' AND submission_started_at_ms IS NULL))");
    cancel_reminder.bind(1, request.now_ms);
    cancel_reminder.bind(2, request.now_ms);
    cancel_reminder.bind(3, payload->offer_id);
    cancel_reminder.execute();
    const auto event_id = next_id(request.next_id);
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        EventJournalEntry{
            .event_id = event_id,
            .event_type = "tarot.house_offer_expired.v1",
            .aggregate_type = "tarot_house_offer",
            .aggregate_id = payload->offer_id,
            .actor_user_id = std::nullopt,
            .guild_id = DiscordSnowflake::parse(offer.column_text(2)),
            .channel_id = DiscordSnowflake::parse(offer.column_text(3)),
            .source_message_id = std::nullopt,
            .occurred_at_ms = request.now_ms,
            .recorded_at_ms = request.now_ms,
            .correlation_id = request.job.correlation_id,
            .causation_id = request.job.causation_event_id,
            .idempotency_key = "tarot.house.offer.expired:" + payload->offer_id,
            .payload_json =
                nlohmann::json{{"offer_id", payload->offer_id},
                               {"status", "expired"},
                               {"is_test", offer.column_int64(4) != 0}}
                    .dump(),
        }));
    retire_public_offer_card(connection, payload->offer_id,
                             DiscordSnowflake::parse(offer.column_text(2)),
                             DiscordSnowflake::parse(offer.column_text(3)),
                             request.now_ms, request.job.correlation_id,
                             event_id, offer.column_int64(4) != 0, "expired",
                             request.next_id);
    status = HouseOfferExpiryStatus::expired;
  }
  if (status == HouseOfferExpiryStatus::expired) {
    complete_job_claim(connection, request.job, request.now_ms);
  } else {
    auto current_claim = connection.prepare(
        "SELECT 1 FROM scheduled_job WHERE job_id=? AND state='claimed' AND "
        "lease_owner=? AND lease_token=?");
    current_claim.bind(1, request.job.job_id);
    current_claim.bind(2, request.job.lease_owner);
    current_claim.bind(3, request.job.lease_token);
    if (current_claim.step())
      complete_job_claim(connection, request.job, request.now_ms);
  }
  transaction.commit();
  return {.status = status, .offer_id = payload->offer_id};
}

HouseControlResult SqliteTarotHouseRepository::inspect_control(
    const HouseControlRequest &request) {
  if (!valid_uuid_v4(request.token_id) || request.invocation.now_ms < 0)
    throw std::invalid_argument{"House control request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT control.expected_user_id,control.expected_revision,"
      "control.action,control.state,control.expires_at_ms,offer.offer_id,"
      "offer.template_slug,offer.guild_id,offer.channel_id,offer.state,"
      "offer.closes_at_ms FROM tarot_house_control control JOIN "
      "tarot_house_offer offer ON offer.offer_id=control.offer_id WHERE "
      "control.token_id=?");
  query.bind(1, request.token_id);
  if (!query.step()) {
    transaction.commit();
    return {.status = HouseControlStatus::invalid_token,
            .offer_id = std::nullopt,
            .template_slug = std::nullopt,
            .closes_at_ms = 0};
  }
  const auto offer_id = query.column_text(5);
  const auto template_slug = query.column_text(6);
  const auto closes_at_ms = query.column_int64(10);
  if (query.column_text(7) != request.invocation.guild_id.str() ||
      query.column_text(8) != request.invocation.channel_id.str()) {
    transaction.commit();
    return {.status = HouseControlStatus::wrong_scope,
            .offer_id = offer_id,
            .template_slug = template_slug,
            .closes_at_ms = closes_at_ms};
  }
  if (!query.column_is_null(0) &&
      query.column_text(0) != request.invocation.user_id.str()) {
    transaction.commit();
    return {.status = HouseControlStatus::wrong_user,
            .offer_id = offer_id,
            .template_slug = template_slug,
            .closes_at_ms = closes_at_ms};
  }
  if (query.column_int64(1) != 1 || query.column_text(2) != "claim") {
    transaction.commit();
    return {.status = HouseControlStatus::invalid_token,
            .offer_id = std::nullopt,
            .template_slug = std::nullopt,
            .closes_at_ms = 0};
  }
  if (query.column_text(3) != "active") {
    transaction.commit();
    return {.status = HouseControlStatus::unavailable,
            .offer_id = offer_id,
            .template_slug = template_slug,
            .closes_at_ms = closes_at_ms};
  }
  if (request.invocation.now_ms >= query.column_int64(4) ||
      request.invocation.now_ms >= closes_at_ms) {
    auto cancel = connection.prepare(
        "UPDATE tarot_house_control SET state='cancelled' WHERE token_id=? "
        "AND state='active'");
    cancel.bind(1, request.token_id);
    cancel.execute();
    transaction.commit();
    return {.status = HouseControlStatus::expired,
            .offer_id = offer_id,
            .template_slug = template_slug,
            .closes_at_ms = closes_at_ms};
  }
  if (query.column_text(9) != "open") {
    transaction.commit();
    return {.status = HouseControlStatus::unavailable,
            .offer_id = offer_id,
            .template_slug = template_slug,
            .closes_at_ms = closes_at_ms};
  }
  transaction.commit();
  return {.status = HouseControlStatus::available,
          .offer_id = offer_id,
          .template_slug = template_slug,
          .closes_at_ms = closes_at_ms};
}

HouseAvailability SqliteTarotHouseRepository::availability(
    const TarotInvocation &invocation,
    const HouseTemplateDefinition &definition, const bool is_test,
    const std::int64_t starting_fate) {
  if (invocation.now_ms < 0 || starting_fate < 0)
    throw std::invalid_argument{"House availability request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();

  HouseAvailability result;
  auto outcome_due_at_ms = invocation.now_ms + definition.outcome_window_ms;
  if (definition.scheduled) {
    auto offer = connection.prepare(
        "SELECT offer.offer_id,offer.resolution_due_at_ms FROM "
        "tarot_house_offer offer JOIN tarot_house_public_card card ON "
        "card.offer_id=offer.offer_id JOIN outbox_message source ON "
        "source.outbox_id=card.create_outbox_id WHERE offer.guild_id=? AND "
        "offer.channel_id=? AND offer.template_slug=? AND offer.is_test=? AND "
        "offer.state='open' AND source.state='delivered' AND "
        "offer.opens_at_ms<=? AND offer.closes_at_ms>? ORDER BY "
        "offer.opens_at_ms DESC LIMIT 1");
    offer.bind(1, invocation.guild_id.str());
    offer.bind(2, invocation.channel_id.str());
    offer.bind(3, definition.slug);
    offer.bind(4, is_test ? 1LL : 0LL);
    offer.bind(5, invocation.now_ms);
    offer.bind(6, invocation.now_ms);
    if (!offer.step()) {
      result.status = HouseAvailabilityStatus::no_scheduled_offer;
      return result;
    }
    result.offer_id = offer.column_text(0);
    outcome_due_at_ms = offer.column_int64(1);
  }

  std::int64_t balance = starting_fate;
  auto account = connection.prepare(
      "SELECT account_id FROM tarot_account WHERE account_kind='HUMAN' AND "
      "user_id=?");
  account.bind(1, invocation.user_id.str());
  if (account.step())
    balance = account_balance(connection, account.column_text(0));
  if (definition.eligibility_balance_below > 0 &&
      balance >= definition.eligibility_balance_below) {
    result.status = HouseAvailabilityStatus::ineligible;
    return result;
  }

  auto cooldown = connection.prepare(
      "SELECT max(CASE WHEN state='accepted_funded' THEN outcome_due_at_ms "
      "ELSE "
      "cooldown_until_ms END) FROM tarot_house_wager WHERE user_id=? AND "
      "template_slug=? AND is_test=? AND (state='accepted_funded' OR "
      "cooldown_until_ms>?)");
  cooldown.bind(1, invocation.user_id.str());
  cooldown.bind(2, definition.slug);
  cooldown.bind(3, is_test ? 1LL : 0LL);
  cooldown.bind(4, invocation.now_ms);
  if (cooldown.step() && !cooldown.column_is_null(0)) {
    result.status = HouseAvailabilityStatus::cooldown;
    result.cooldown_until_ms = cooldown.column_int64(0);
    return result;
  }
  if (!is_test && definition.authority == HouseResolutionAuthority::draw) {
    auto draw_cooldown = connection.prepare(
        "SELECT max(cooldown_until_ms) FROM tarot_card_draw WHERE user_id=? "
        "AND is_test=0 AND cooldown_until_ms>?");
    draw_cooldown.bind(1, invocation.user_id.str());
    draw_cooldown.bind(2, invocation.now_ms);
    if (draw_cooldown.step() && !draw_cooldown.column_is_null(0) &&
        draw_cooldown.column_int64(0) >= outcome_due_at_ms) {
      result.status = HouseAvailabilityStatus::cooldown;
      result.cooldown_until_ms = draw_cooldown.column_int64(0);
    }
  }
  return result;
}

HouseMutationResult
SqliteTarotHouseRepository::play(const HousePlayRequest &request) {
  if (request.definition == nullptr || !request.next_id ||
      request.exposure_cap < 1 || request.profit_cap < 1)
    throw std::invalid_argument{"House play request is invalid."};
  const auto choice =
      std::ranges::find(request.definition->choices, request.choice_slug,
                        &HouseChoiceDefinition::slug);
  if (choice == request.definition->choices.end() ||
      std::ranges::find(request.definition->allowed_stakes, request.stake) ==
          request.definition->allowed_stakes.end())
    throw std::invalid_argument{"House choice or stake is not in the catalog."};
  if (request.stake > 0 &&
      request.stake >
          std::numeric_limits<std::int64_t>::max() / choice->profit_numerator)
    throw std::overflow_error{"House payout overflowed."};
  const auto scaled = request.stake * choice->profit_numerator;
  if (scaled % choice->profit_denominator != 0)
    throw std::invalid_argument{"House payout must be integral."};
  const auto profit = request.definition->recovery
                          ? request.definition->recovery_reward
                          : scaled / choice->profit_denominator;
  if (profit > request.profit_cap)
    throw std::invalid_argument{"House profit exceeds the configured cap."};
  const auto fingerprint = play_fingerprint(request);

  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto receipt = connection.prepare(
      "SELECT operation,request_fingerprint,status,wager_id FROM "
      "tarot_house_receipt WHERE idempotency_key=?");
  receipt.bind(1, request.invocation.interaction_idempotency_key);
  if (receipt.step()) {
    if (receipt.column_text(0) != "play" || receipt.column_is_null(1) ||
        receipt.column_text(1) != fingerprint)
      throw std::invalid_argument{"House play idempotency key was reused."};
    const auto status = receipt_status(receipt.column_text(2));
    const auto record = receipt.column_is_null(3)
                            ? std::nullopt
                            : load_wager(connection, receipt.column_text(3));
    transaction.commit();
    return {.status = status, .wager = record, .event_types = {}};
  }

  std::int64_t offer_reserved_profit{};
  std::optional<std::int64_t> offer_resolution_due_at_ms;
  if (request.definition->scheduled) {
    if (!request.offer_id || !valid_uuid_v4(*request.offer_id))
      throw std::invalid_argument{"Scheduled House play requires an offer."};
    auto offer = connection.prepare(
        "SELECT offer.reserved_profit,offer.resolution_due_at_ms FROM "
        "tarot_house_offer offer JOIN tarot_house_public_card card ON "
        "card.offer_id=offer.offer_id JOIN outbox_message source ON "
        "source.outbox_id=card.create_outbox_id WHERE offer.offer_id=? AND "
        "offer.catalog_version=? AND offer.template_slug=? AND offer.is_test=? "
        "AND offer.state='open' AND source.state='delivered' AND "
        "offer.opens_at_ms<=? AND offer.closes_at_ms>?");
    offer.bind(1, *request.offer_id);
    offer.bind(2, request.catalog_version);
    offer.bind(3, request.definition->slug);
    offer.bind(4, request.is_test ? 1LL : 0LL);
    offer.bind(5, request.invocation.now_ms);
    offer.bind(6, request.invocation.now_ms);
    if (!offer.step()) {
      save_receipt(connection, request, HouseMutationStatus::invalid_state,
                   std::nullopt, fingerprint);
      transaction.commit();
      return {.status = HouseMutationStatus::invalid_state,
              .wager = std::nullopt,
              .event_types = {}};
    }
    offer_reserved_profit = offer.column_int64(0);
    offer_resolution_due_at_ms = offer.column_int64(1);
  } else if (request.offer_id) {
    throw std::invalid_argument{
        "On-demand House play cannot be linked to an offer."};
  }

  const auto due = offer_resolution_due_at_ms.value_or(
      request.invocation.now_ms + request.definition->outcome_window_ms);

  const auto human = human_account(connection, request.invocation.user_id);
  const auto balance = account_balance(connection, human);
  if (request.definition->eligibility_balance_below > 0 &&
      balance >= request.definition->eligibility_balance_below) {
    save_receipt(connection, request, HouseMutationStatus::ineligible,
                 std::nullopt, fingerprint);
    transaction.commit();
    return {.status = HouseMutationStatus::ineligible,
            .wager = std::nullopt,
            .event_types = {}};
  }
  auto cooldown = connection.prepare(
      "SELECT 1 FROM tarot_house_wager WHERE user_id=? AND template_slug=? "
      "AND is_test=? AND (state='accepted_funded' OR cooldown_until_ms>?) "
      "LIMIT 1");
  cooldown.bind(1, request.invocation.user_id.str());
  cooldown.bind(2, request.definition->slug);
  cooldown.bind(3, request.is_test ? 1LL : 0LL);
  cooldown.bind(4, request.invocation.now_ms);
  if (cooldown.step()) {
    save_receipt(connection, request, HouseMutationStatus::cooldown,
                 std::nullopt, fingerprint);
    transaction.commit();
    return {.status = HouseMutationStatus::cooldown,
            .wager = std::nullopt,
            .event_types = {}};
  }
  if (!request.is_test &&
      request.definition->authority == HouseResolutionAuthority::draw) {
    auto draw_cooldown = connection.prepare(
        "SELECT max(cooldown_until_ms) FROM tarot_card_draw WHERE user_id=? "
        "AND is_test=0 AND cooldown_until_ms>?");
    draw_cooldown.bind(1, request.invocation.user_id.str());
    draw_cooldown.bind(2, request.invocation.now_ms);
    if (draw_cooldown.step() && !draw_cooldown.column_is_null(0) &&
        draw_cooldown.column_int64(0) >= due) {
      save_receipt(connection, request, HouseMutationStatus::cooldown,
                   std::nullopt, fingerprint);
      transaction.commit();
      return {.status = HouseMutationStatus::cooldown,
              .wager = std::nullopt,
              .event_types = {}};
    }
  }
  if (balance < request.stake) {
    save_receipt(connection, request, HouseMutationStatus::insufficient_funds,
                 std::nullopt, fingerprint);
    transaction.commit();
    return {.status = HouseMutationStatus::insufficient_funds,
            .wager = std::nullopt,
            .event_types = {}};
  }
  auto exposure_query = connection.prepare(
      "SELECT COALESCE((SELECT sum(profit) FROM tarot_house_wager WHERE "
      "is_test=? "
      "AND state='accepted_funded' AND recovery=0),0)+COALESCE((SELECT "
      "sum(reserved_profit) "
      "FROM tarot_house_offer WHERE is_test=? AND state='open'),0)");
  exposure_query.bind(1, request.is_test ? 1LL : 0LL);
  exposure_query.bind(2, request.is_test ? 1LL : 0LL);
  if (!exposure_query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House exposure query failed."};
  const auto current_exposure =
      exposure_query.column_int64(0) - offer_reserved_profit;
  if (!request.definition->recovery &&
      current_exposure > request.exposure_cap - profit) {
    save_receipt(connection, request, HouseMutationStatus::exposure_blocked,
                 std::nullopt, fingerprint);
    transaction.commit();
    return {.status = HouseMutationStatus::exposure_blocked,
            .wager = std::nullopt,
            .event_types = {}};
  }

  const auto wager_id = next_id(request.next_id);
  const auto event_id = next_id(request.next_id);
  const auto transaction_id = next_id(request.next_id);
  const auto transfer_id = next_id(request.next_id);
  const auto action_id = next_id(request.next_id);
  insert_event(connection, event_id, "tarot.house_funded.v1", wager_id,
               request.invocation,
               request.invocation.interaction_idempotency_key + ":event",
               {{"wager_id", wager_id},
                {"template", request.definition->slug},
                {"visibility", visibility_name(request.visibility)},
                {"is_test", request.is_test}});
  auto wager = connection.prepare(
      "INSERT INTO "
      "tarot_house_wager(wager_id,offer_id,user_id,guild_id,channel_id,"
      "catalog_version,template_slug,proposition,choice_slug,choice_label,"
      "odds_numerator,odds_denominator,stake,profit,visibility,authority,state,"
      "result,terminal_reason,accepted_at_ms,outcome_due_at_ms,terminal_"
      "cooldown_ms,cooldown_until_ms,"
      "terminal_at_ms,recovery,is_test,accepted_event_id,terminal_event_id,"
      "revision) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'accepted_funded',NULL,NULL,"
      "?,?,?,?,NULL,?,?,?,NULL,1)");
  wager.bind(1, wager_id);
  if (request.offer_id)
    wager.bind(2, *request.offer_id);
  else
    wager.bind_null(2);
  wager.bind(3, request.invocation.user_id.str());
  wager.bind(4, request.invocation.guild_id.str());
  wager.bind(5, request.invocation.channel_id.str());
  wager.bind(6, request.catalog_version);
  wager.bind(7, request.definition->slug);
  wager.bind(8, request.definition->proposition);
  wager.bind(9, choice->slug);
  wager.bind(10, choice->label);
  wager.bind(11, choice->profit_numerator);
  wager.bind(12, choice->profit_denominator);
  wager.bind(13, request.stake);
  wager.bind(14, profit);
  wager.bind(15, visibility_name(request.visibility));
  wager.bind(16, authority_name(request.definition->authority));
  wager.bind(17, request.invocation.now_ms);
  wager.bind(18, due);
  wager.bind(19, request.definition->terminal_cooldown_ms);
  wager.bind(20, request.invocation.now_ms);
  wager.bind(21, request.definition->recovery ? 1LL : 0LL);
  wager.bind(22, request.is_test ? 1LL : 0LL);
  wager.bind(23, event_id);
  wager.execute();
  if (request.offer_id) {
    auto close = connection.prepare(
        "UPDATE tarot_house_offer SET state='closed' WHERE offer_id=? AND "
        "state='open'");
    close.bind(1, *request.offer_id);
    close.execute();
    if (connection.changes() != 1)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT, "House offer claim raced."};
    auto cancel_control = connection.prepare(
        "UPDATE tarot_house_control SET state='cancelled' WHERE offer_id=? "
        "AND state='active'");
    cancel_control.bind(1, *request.offer_id);
    cancel_control.execute();
    auto cancel_expiry = connection.prepare(
        "UPDATE scheduled_job SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,updated_at_ms=max(?,updated_at_"
        "ms),terminal_at_ms=max(?,created_at_ms),last_error_code='house_offer_"
        "claimed' WHERE job_id=(SELECT job_id FROM "
        "tarot_house_offer_deadline WHERE offer_id=?) AND state IN "
        "('pending','claimed')");
    cancel_expiry.bind(1, request.invocation.now_ms);
    cancel_expiry.bind(2, request.invocation.now_ms);
    cancel_expiry.bind(3, *request.offer_id);
    cancel_expiry.execute();
    auto cancel_reminder = connection.prepare(
        "UPDATE outbox_message SET state='cancelled',lease_owner=NULL,"
        "lease_token=NULL,lease_until_ms=NULL,terminal_at_ms=max(?,created_at_"
        "ms),"
        "updated_at_ms=max(?,updated_at_ms),last_error_code='house_offer_"
        "claimed' "
        "WHERE outbox_id=(SELECT reminder_outbox_id FROM "
        "tarot_house_public_card WHERE offer_id=?) AND (state='pending' OR "
        "(state='claimed' AND submission_started_at_ms IS NULL))");
    cancel_reminder.bind(1, request.invocation.now_ms);
    cancel_reminder.bind(2, request.invocation.now_ms);
    cancel_reminder.bind(3, *request.offer_id);
    cancel_reminder.execute();
    retire_public_offer_card(
        connection, *request.offer_id, request.invocation.guild_id,
        request.invocation.channel_id, request.invocation.now_ms,
        request.invocation.correlation_id, event_id, request.is_test, "claimed",
        request.next_id);
  }

  const auto posting_count = request.definition->recovery ? 2LL : 3LL;
  insert_transaction(
      connection, transaction_id, "WAGER_ESCROW_FUND", posting_count, event_id,
      request.invocation.interaction_idempotency_key + ":ledger",
      request.invocation, request.is_test, "House wager funding");
  if (request.definition->recovery) {
    insert_posting(connection, next_id(request.next_id), transaction_id,
                   system_account(connection, "MINT"), -profit,
                   request.invocation.now_ms);
  } else {
    insert_posting(connection, next_id(request.next_id), transaction_id, human,
                   -request.stake, request.invocation.now_ms);
    insert_posting(connection, next_id(request.next_id), transaction_id,
                   system_account(connection, "HOUSE"), -profit,
                   request.invocation.now_ms);
  }
  insert_posting(connection, next_id(request.next_id), transaction_id,
                 system_account(connection, "ESCROW"), request.stake + profit,
                 request.invocation.now_ms);
  auto transfer = connection.prepare(
      "INSERT INTO tarot_house_transfer(transfer_id,wager_id,transfer_kind,"
      "transaction_id,created_at_ms) VALUES(?,?,'fund',?,?)");
  transfer.bind(1, transfer_id);
  transfer.bind(2, wager_id);
  transfer.bind(3, transaction_id);
  transfer.bind(4, request.invocation.now_ms);
  transfer.execute();
  commit_transaction(connection, transaction_id, request.invocation.now_ms);
  auto action = connection.prepare(
      "INSERT INTO tarot_house_action(action_id,wager_id,action_kind,"
      "actor_user_id,expected_revision,reason,created_at_ms) "
      "VALUES(?,?,'accepted',?,1,NULL,?)");
  action.bind(1, action_id);
  action.bind(2, wager_id);
  action.bind(3, request.invocation.user_id.str());
  action.bind(4, request.invocation.now_ms);
  action.execute();
  schedule_deadline(connection, request, wager_id, due);
  if (const auto stored = load_wager(connection, wager_id))
    static_cast<void>(enqueue_public_flavor(connection, *stored, event_id,
                                            request.invocation, "funded", "",
                                            request.next_id));
  save_receipt(connection, request, HouseMutationStatus::applied, wager_id,
               fingerprint);
  transaction.commit();
  return {.status = HouseMutationStatus::applied,
          .wager = load_wager(connection, wager_id),
          .event_types = {"tarot.house_funded.v1"}};
}

HouseMutationResult
SqliteTarotHouseRepository::resolve(const HouseResolveRequest &request) {
  if (!valid_uuid_v4(request.wager_id) || !request.next_id ||
      blank(request.reason) || request.reason.size() > 200 || request.automatic)
    throw std::invalid_argument{"House resolution request is invalid."};
  if (!request.observed_choice ||
      (*request.observed_choice != "yes" && *request.observed_choice != "no" &&
       *request.observed_choice != "void"))
    throw std::invalid_argument{
        "Manual House outcome must be yes, no, or void."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto fingerprint = resolution_fingerprint(request);
  auto receipt = connection.prepare(
      "SELECT wager_id,operation,status,request_fingerprint FROM "
      "tarot_house_receipt WHERE idempotency_key=?");
  receipt.bind(1, request.invocation.interaction_idempotency_key);
  if (receipt.step()) {
    const auto stored_status = receipt.column_text(2);
    const auto expected_null_wager = stored_status == "not_found";
    if (receipt.column_is_null(0) != expected_null_wager ||
        (!receipt.column_is_null(0) &&
         receipt.column_text(0) != request.wager_id) ||
        receipt.column_text(1) != "resolve" || receipt.column_is_null(3) ||
        receipt.column_text(3) != fingerprint)
      throw std::invalid_argument{
          "House resolution idempotency key was reused."};
    auto prior = receipt.column_is_null(0)
                     ? std::nullopt
                     : load_wager(connection, receipt.column_text(0));
    const auto status = receipt_status(stored_status);
    transaction.commit();
    return {.status = status,
            .wager = std::move(prior),
            .event_types = {}};
  }
  const auto save_resolution_receipt =
      [&](const HouseMutationStatus status,
          const std::optional<std::string_view> wager_id) {
        auto save = connection.prepare(
            "INSERT INTO tarot_house_receipt(idempotency_key,wager_id,"
            "operation,status,request_fingerprint,created_at_ms) "
            "VALUES(?,?,'resolve',?,?,?)");
        save.bind(1, request.invocation.interaction_idempotency_key);
        if (wager_id)
          save.bind(2, *wager_id);
        else
          save.bind_null(2);
        save.bind(3, receipt_status_name(status));
        save.bind(4, fingerprint);
        save.bind(5, request.invocation.now_ms);
        save.execute();
      };
  auto wager = load_wager(connection, request.wager_id);
  if (!wager) {
    save_resolution_receipt(HouseMutationStatus::not_found, std::nullopt);
    transaction.commit();
    return {.status = HouseMutationStatus::not_found,
            .wager = std::nullopt,
            .event_types = {}};
  }
  if (wager->authority != HouseResolutionAuthority::owner ||
      (wager->is_test && !request.test_mode)) {
    save_resolution_receipt(HouseMutationStatus::forbidden, wager->wager_id);
    transaction.commit();
    return {.status = HouseMutationStatus::forbidden,
            .wager = wager,
            .event_types = {}};
  }
  HouseResult resolved_result = request.result;
  auto owner = connection.prepare(
      "SELECT config.owner_user_id FROM tarot_house_wager wager JOIN "
      "guild_config config ON config.guild_id=wager.guild_id WHERE "
      "wager.wager_id=?");
  owner.bind(1, request.wager_id);
  if (!owner.step() ||
      owner.column_text(0) != request.invocation.user_id.str()) {
    save_resolution_receipt(HouseMutationStatus::forbidden, wager->wager_id);
    transaction.commit();
    return {.status = HouseMutationStatus::forbidden,
            .wager = wager,
            .event_types = {}};
  }
  auto window = connection.prepare(
      "SELECT offer.closes_at_ms,offer.resolution_due_at_ms FROM "
      "tarot_house_wager wager JOIN tarot_house_offer offer ON "
      "offer.offer_id=wager.offer_id WHERE wager.wager_id=?");
  window.bind(1, request.wager_id);
  if (!window.step() || request.invocation.now_ms > window.column_int64(1) ||
      (*request.observed_choice != "void" && !wager->is_test &&
       request.invocation.now_ms < window.column_int64(0))) {
    save_resolution_receipt(HouseMutationStatus::invalid_state,
                            wager->wager_id);
    transaction.commit();
    return {.status = HouseMutationStatus::invalid_state,
            .wager = wager,
            .event_types = {}};
  }
  if (*request.observed_choice == "void") {
    resolved_result = HouseResult::void_wager;
  } else {
    if (wager->choice_slug != "yes" && wager->choice_slug != "no")
      throw std::invalid_argument{
          "Manual House outcome does not match the accepted template."};
    resolved_result = wager->choice_slug == *request.observed_choice
                          ? HouseResult::win
                          : HouseResult::loss;
  }
  auto result = settle_uncommitted(
      connection, *wager, request.invocation, resolved_result, request.reason,
      SettlementCause::owner_resolution, std::nullopt, request.next_id);
  save_resolution_receipt(result.status, request.wager_id);
  transaction.commit();
  return result;
}

std::vector<HouseMutationResult>
SqliteTarotHouseRepository::observe_draw(const TarotDrawRecord &draw,
                                         const std::int64_t now_ms,
                                         std::function<std::string()> factory) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto persisted = connection.prepare(
      "SELECT user_id,visibility,drawn_at_ms,is_test FROM tarot_card_draw "
      "WHERE draw_id=?");
  persisted.bind(1, draw.draw_id);
  if (!persisted.step()) {
    transaction.commit();
    return {};
  }
  const auto persisted_user = persisted.column_text(0);
  const auto persisted_visibility = persisted.column_text(1);
  const auto persisted_drawn_at_ms = persisted.column_int64(2);
  const auto persisted_is_test = persisted.column_int64(3) != 0;
  if (persisted_user != draw.user_id.str() ||
      persisted_visibility != visibility_name(draw.visibility) ||
      persisted_drawn_at_ms != draw.drawn_at_ms ||
      persisted_is_test != draw.is_test)
    throw std::invalid_argument{
        "Observed Tarot draw does not match its persisted source."};
  auto query = connection.prepare(
      "SELECT wager.wager_id,wager.user_id,wager.template_slug,"
      "wager.catalog_version,wager.proposition,wager.choice_slug,"
      "wager.choice_label,wager.odds_numerator,wager.odds_denominator,"
      "wager.stake,wager.profit,wager.visibility,wager.authority,wager.state,"
      "wager.result,wager.accepted_at_ms,wager.outcome_due_at_ms,"
      "wager.terminal_cooldown_ms,wager.recovery,wager.is_test FROM "
      "tarot_house_wager wager "
      "JOIN tarot_event_order accepted_order ON accepted_order.event_id="
      "wager.accepted_event_id JOIN tarot_card_draw draw ON draw.draw_id=? "
      "JOIN discord_user drawer ON drawer.user_id=draw.user_id "
      "JOIN tarot_event_order draw_order ON draw_order.event_id=draw.event_id "
      "WHERE wager.state='accepted_funded' AND draw_order.sequence_id>"
      "accepted_order.sequence_id AND "
      "wager.outcome_due_at_ms>=draw.drawn_at_ms "
      "AND wager.is_test=draw.is_test AND draw.guild_id=wager.guild_id AND "
      "draw.channel_id=wager.channel_id AND ((wager.authority='draw' AND "
      "wager.user_id=draw.user_id) OR (wager.authority='public_draw' AND "
      "wager.user_id<>draw.user_id AND drawer.is_bot=0 AND "
      "draw.visibility='public')) ORDER BY "
      "accepted_order.sequence_id,wager.wager_id");
  query.bind(1, draw.draw_id);
  std::vector<HouseWagerRecord> wagers;
  while (query.step())
    wagers.push_back(record_from(query));
  std::vector<HouseMutationResult> results;
  for (auto &wager : wagers) {
    auto result = HouseResult::win;
    if (wager.authority == HouseResolutionAuthority::public_draw &&
        wager.choice_slug == "no")
      result = HouseResult::loss;
    TarotInvocation call{
        .user_id = wager.user_id,
        .guild_id = DiscordSnowflake::parse("1"),
        .channel_id = DiscordSnowflake::parse("1"),
        .display_name = "House observer",
        .interaction_idempotency_key =
            "tarot.house.draw:" + draw.draw_id + ":" + wager.wager_id,
        .correlation_id = "tarot-house-draw-observer",
        .now_ms =
            std::max({now_ms, persisted_drawn_at_ms, wager.accepted_at_ms})};
    auto scope = connection.prepare(
        "SELECT guild_id,channel_id FROM tarot_house_wager WHERE wager_id=?");
    scope.bind(1, wager.wager_id);
    if (scope.step()) {
      call.guild_id = DiscordSnowflake::parse(scope.column_text(0));
      call.channel_id = DiscordSnowflake::parse(scope.column_text(1));
    }
    results.push_back(settle_uncommitted(
        connection, wager, call, result, "Observed qualifying Tarot draw",
        SettlementCause::observed_draw, draw.draw_id, factory));
  }
  transaction.commit();
  return results;
}

std::vector<HouseMutationResult> SqliteTarotHouseRepository::reconcile_draws(
    const std::int64_t now_ms, std::function<std::string()> factory) {
  if (now_ms < 0 || !factory)
    throw std::invalid_argument{"House draw reconciliation is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT wager_id,user_id,template_slug,catalog_version,proposition,"
      "choice_slug,choice_label,odds_numerator,odds_denominator,stake,profit,"
      "visibility,authority,state,result,accepted_at_ms,outcome_due_at_ms,"
      "terminal_cooldown_ms,recovery,is_test FROM tarot_house_wager WHERE "
      "state='accepted_funded' AND authority IN ('draw','public_draw') "
      "ORDER BY accepted_at_ms,wager_id");
  std::vector<HouseWagerRecord> wagers;
  while (query.step())
    wagers.push_back(record_from(query));
  std::vector<HouseMutationResult> results;
  for (const auto &wager : wagers) {
    const auto draw = qualifying_draw(connection, wager);
    if (draw)
      results.push_back(settle_qualifying_draw_uncommitted(
          connection, wager, *draw, now_ms, factory));
  }
  transaction.commit();
  return results;
}

std::vector<HouseMutationResult>
SqliteTarotHouseRepository::resolve_due(const std::int64_t now_ms,
                                        const bool test_only,
                                        std::function<std::string()> factory) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT wager_id,user_id,template_slug,catalog_version,proposition,"
      "choice_slug,choice_label,odds_numerator,odds_denominator,stake,profit,"
      "visibility,authority,state,result,accepted_at_ms,outcome_due_at_ms,"
      "terminal_cooldown_ms,recovery,is_test FROM tarot_house_wager WHERE "
      "state='accepted_funded' "
      "AND outcome_due_at_ms<=? AND (?=0 OR is_test=1) "
      "ORDER BY outcome_due_at_ms,wager_id");
  query.bind(1, now_ms);
  query.bind(2, test_only ? 1LL : 0LL);
  std::vector<HouseWagerRecord> wagers;
  while (query.step())
    wagers.push_back(record_from(query));
  std::vector<HouseMutationResult> results;
  for (auto &wager : wagers) {
    const auto draw = qualifying_draw(connection, wager);
    const auto result = draw ? observed_draw_outcome(wager)
                             : deadline_outcome(connection, wager);
    auto scope = connection.prepare(
        "SELECT guild_id,channel_id FROM tarot_house_wager WHERE wager_id=?");
    scope.bind(1, wager.wager_id);
    if (!scope.step())
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA, "House wager scope is missing."};
    TarotInvocation call{
        .user_id = wager.user_id,
        .guild_id = DiscordSnowflake::parse(scope.column_text(0)),
        .channel_id = DiscordSnowflake::parse(scope.column_text(1)),
        .display_name = "House deadline",
        .interaction_idempotency_key = "tarot.house.deadline:" + wager.wager_id,
        .correlation_id = "tarot-house-deadline",
        .now_ms = now_ms};
    results.push_back(settle_uncommitted(
        connection, wager, call, result,
        draw ? "Observed qualifying Tarot draw during deadline reconciliation"
        : result == HouseResult::void_wager ? "House deadline void"
                                            : "Observable deadline result",
        draw ? SettlementCause::observed_draw : SettlementCause::deadline,
        draw ? std::optional<std::string_view>{draw->draw_id} : std::nullopt,
        factory));
  }
  transaction.commit();
  return results;
}

HouseMutationResult SqliteTarotHouseRepository::handle_deadline(
    const HouseDeadlineRequest &request) {
  const auto *payload =
      std::get_if<HouseDeadlineJobPayload>(&request.job.payload);
  if (payload == nullptr || payload->expected_revision == 0 || !request.next_id)
    throw std::invalid_argument{"House deadline payload is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto link = connection.prepare(
      "SELECT expected_revision,due_at_ms FROM tarot_house_deadline WHERE "
      "wager_id=? AND job_id=?");
  link.bind(1, payload->wager_id);
  link.bind(2, request.job.job_id);
  if (!link.step() || link.column_int64(0) !=
                          static_cast<std::int64_t>(payload->expected_revision))
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "House deadline link is invalid."};
  const auto due_at_ms = link.column_int64(1);
  if (request.now_ms < due_at_ms)
    throw std::invalid_argument{
        "House deadline was claimed before it was due."};
  auto wager = load_wager(connection, payload->wager_id);
  HouseMutationResult result{.status = HouseMutationStatus::not_found,
                             .wager = std::nullopt,
                             .event_types = {}};
  if (wager && wager->state == HouseWagerState::accepted_funded &&
      payload->expected_revision == 1) {
    const auto draw = qualifying_draw(connection, *wager);
    const auto outcome = draw ? observed_draw_outcome(*wager)
                              : deadline_outcome(connection, *wager);
    auto scope = connection.prepare(
        "SELECT guild_id,channel_id FROM tarot_house_wager WHERE wager_id=?");
    scope.bind(1, wager->wager_id);
    if (!scope.step())
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA, "House wager scope is missing."};
    const TarotInvocation call{
        .user_id = wager->user_id,
        .guild_id = DiscordSnowflake::parse(scope.column_text(0)),
        .channel_id = DiscordSnowflake::parse(scope.column_text(1)),
        .display_name = "House deadline",
        .interaction_idempotency_key =
            "tarot.house.deadline:" + wager->wager_id,
        .correlation_id = request.job.correlation_id,
        .now_ms = request.now_ms};
    result = settle_uncommitted(
        connection, *wager, call, outcome,
        draw ? "Observed qualifying Tarot draw during deadline reconciliation"
        : outcome == HouseResult::void_wager ? "House deadline void"
                                             : "Observable deadline result",
        draw ? SettlementCause::observed_draw : SettlementCause::deadline,
        draw ? std::optional<std::string_view>{draw->draw_id} : std::nullopt,
        request.next_id);
  } else if (wager) {
    result = {.status = HouseMutationStatus::invalid_state,
              .wager = wager,
              .event_types = {}};
  }
  complete_job_claim(connection, request.job, request.now_ms);
  transaction.commit();
  return result;
}

HouseMutationResult SqliteTarotHouseRepository::cleanup_test_wager(
    const HouseTestCleanupRequest &request) {
  if (!request.owner || !request.test_mode || !request.next_id ||
      !valid_uuid_v4(request.wager_id) || blank(request.reason) ||
      request.reason.size() > 200)
    throw std::invalid_argument{"Test House cleanup request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  const auto fingerprint = cleanup_fingerprint(request);

  auto prior = connection.prepare(
      "SELECT wager_id,operation,status,request_fingerprint FROM "
      "tarot_house_receipt WHERE idempotency_key=?");
  prior.bind(1, request.invocation.interaction_idempotency_key);
  if (prior.step()) {
    const auto stored_status = prior.column_text(2);
    const auto expected_null_wager = stored_status == "not_found";
    if (prior.column_is_null(0) != expected_null_wager ||
        (!prior.column_is_null(0) && prior.column_text(0) != request.wager_id) ||
        prior.column_text(1) != "test_cleanup" || prior.column_is_null(3) ||
        prior.column_text(3) != fingerprint)
      throw std::invalid_argument{
          "House test cleanup idempotency key was reused."};
    const auto status = receipt_status(stored_status);
    const auto wager = prior.column_is_null(0)
                           ? std::nullopt
                           : load_wager(connection, prior.column_text(0));
    transaction.commit();
    return {.status = status, .wager = wager, .event_types = {}};
  }
  const auto save_cleanup_receipt =
      [&](const HouseMutationStatus status,
          const std::optional<std::string_view> wager_id) {
        const auto value = [&] {
          switch (status) {
          case HouseMutationStatus::applied:
            return "applied";
          case HouseMutationStatus::not_found:
            return "not_found";
          case HouseMutationStatus::forbidden:
            return "forbidden";
          default:
            return "replay";
          }
        }();
        auto insert = connection.prepare(
            "INSERT INTO tarot_house_receipt(idempotency_key,wager_id,"
            "operation,status,request_fingerprint,created_at_ms) "
            "VALUES(?,?,'test_cleanup',?,?,?)");
        insert.bind(1, request.invocation.interaction_idempotency_key);
        if (wager_id)
          insert.bind(2, *wager_id);
        else
          insert.bind_null(2);
        insert.bind(3, value);
        insert.bind(4, fingerprint);
        insert.bind(5, request.invocation.now_ms);
        insert.execute();
      };

  auto wager = load_wager(connection, request.wager_id);
  if (!wager) {
    save_cleanup_receipt(HouseMutationStatus::not_found, std::nullopt);
    transaction.commit();
    return {.status = HouseMutationStatus::not_found,
            .wager = std::nullopt,
            .event_types = {}};
  }
  const auto terminal = wager->state == HouseWagerState::resolved ||
                        wager->state == HouseWagerState::void_refunded;
  if (!wager->is_test || !terminal ||
      wager->user_id != request.invocation.user_id) {
    save_cleanup_receipt(HouseMutationStatus::forbidden, wager->wager_id);
    transaction.commit();
    return {.status = HouseMutationStatus::forbidden,
            .wager = wager,
            .event_types = {}};
  }

  struct OriginalTransaction {
    std::string transaction_id;
    std::int64_t posting_count{};
  };
  std::vector<OriginalTransaction> originals;
  auto originals_query = connection.prepare(
      "SELECT tx.transaction_id,tx.expected_posting_count FROM "
      "tarot_house_transfer transfer JOIN tarot_transaction tx ON "
      "tx.transaction_id=transfer.transaction_id WHERE transfer.wager_id=? "
      "AND tx.is_test=1 AND tx.state='committed' AND tx.transaction_type IN "
      "('WAGER_ESCROW_FUND','WAGER_PAYOUT','WAGER_REFUND') AND NOT EXISTS("
      "SELECT 1 FROM tarot_house_test_cleanup cleanup WHERE "
      "cleanup.wager_id=transfer.wager_id AND cleanup.original_transaction_id="
      "tx.transaction_id) ORDER BY tx.ledger_sequence DESC");
  originals_query.bind(1, wager->wager_id);
  while (originals_query.step())
    originals.push_back(
        {originals_query.column_text(0), originals_query.column_int64(1)});
  if (originals.empty()) {
    save_cleanup_receipt(HouseMutationStatus::replay, wager->wager_id);
    transaction.commit();
    return {.status = HouseMutationStatus::replay,
            .wager = wager,
            .event_types = {}};
  }

  for (const auto &original : originals) {
    const auto reversal_id = next_id(request.next_id);
    const auto event_id = next_id(request.next_id);
    const auto event_inserted = detail::insert_event_uncommitted(
        connection,
        EventJournalEntry{
            .event_id = event_id,
            .event_type = "tarot.transaction_reversed.v1",
            .aggregate_type = "tarot_transaction",
            .aggregate_id = reversal_id,
            .actor_user_id = request.invocation.user_id,
            .guild_id = request.invocation.guild_id,
            .channel_id = request.invocation.channel_id,
            .source_message_id = std::nullopt,
            .occurred_at_ms = request.invocation.now_ms,
            .recorded_at_ms = request.invocation.now_ms,
            .correlation_id = request.invocation.correlation_id,
            .causation_id = std::nullopt,
            .idempotency_key = "event:house-cleanup:" + wager->wager_id + ":" +
                               original.transaction_id,
            .payload_json = nlohmann::json{
                {"original_transaction_id", original.transaction_id},
                {"reason", request.reason},
                {"is_test",
                 true}}.dump()});
    if (!event_inserted)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "House cleanup event was not fresh."};
    insert_transaction(
        connection, reversal_id, "TEST_REVERSAL", original.posting_count,
        event_id,
        "tx:house-cleanup:" + wager->wager_id + ":" + original.transaction_id,
        request.invocation, true, request.reason, original.transaction_id);
    auto cleanup = connection.prepare(
        "INSERT INTO tarot_house_test_cleanup(wager_id,"
        "original_transaction_id,reversal_transaction_id,actor_user_id,reason,"
        "created_at_ms) VALUES(?,?,?,?,?,?)");
    cleanup.bind(1, wager->wager_id);
    cleanup.bind(2, original.transaction_id);
    cleanup.bind(3, reversal_id);
    cleanup.bind(4, request.invocation.user_id.str());
    cleanup.bind(5, request.reason);
    cleanup.bind(6, request.invocation.now_ms);
    cleanup.execute();

    auto postings = connection.prepare(
        "SELECT account_id,amount FROM tarot_posting WHERE transaction_id=? "
        "ORDER BY posting_id");
    postings.bind(1, original.transaction_id);
    std::int64_t posting_count{};
    while (postings.step()) {
      insert_posting(connection, next_id(request.next_id), reversal_id,
                     postings.column_text(0), -postings.column_int64(1),
                     request.invocation.now_ms);
      ++posting_count;
    }
    if (posting_count != original.posting_count)
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "House cleanup posting count changed."};
    commit_transaction(connection, reversal_id, request.invocation.now_ms);
  }

  const auto cleaned_event_id = next_id(request.next_id);
  insert_event(connection, cleaned_event_id, "tarot.house_test_cleaned.v1",
               wager->wager_id, request.invocation,
               "event:house-cleaned:" + wager->wager_id,
               {{"reversal_count", originals.size()}, {"is_test", true}});
  auto action = connection.prepare(
      "INSERT INTO tarot_house_action(action_id,wager_id,action_kind,"
      "actor_user_id,expected_revision,reason,created_at_ms) "
      "VALUES(?,?,'cleanup',?,2,?,?)");
  action.bind(1, next_id(request.next_id));
  action.bind(2, wager->wager_id);
  action.bind(3, request.invocation.user_id.str());
  action.bind(4, request.reason);
  action.bind(5, request.invocation.now_ms);
  action.execute();
  save_cleanup_receipt(HouseMutationStatus::applied, wager->wager_id);
  transaction.commit();
  return {.status = HouseMutationStatus::applied,
          .wager = wager,
          .event_types = {"tarot.transaction_reversed.v1",
                          "tarot.house_test_cleaned.v1"}};
}

std::vector<HouseWagerRecord> SqliteTarotHouseRepository::history(
    const DiscordSnowflake &user_id,
    const std::optional<std::string_view> reference) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto query =
      reference
          ? connection.prepare(
                "SELECT wager_id,user_id,template_slug,catalog_version,"
                "proposition,choice_slug,choice_label,odds_numerator,"
                "odds_denominator,stake,profit,visibility,authority,state,"
                "result,accepted_at_ms,outcome_due_at_ms,terminal_cooldown_ms,"
                "recovery,is_test "
                "FROM tarot_house_wager WHERE user_id=? AND wager_id=?")
          : connection.prepare(
                "SELECT wager_id,user_id,template_slug,catalog_version,"
                "proposition,choice_slug,choice_label,odds_numerator,"
                "odds_denominator,stake,profit,visibility,authority,state,"
                "result,accepted_at_ms,outcome_due_at_ms,terminal_cooldown_ms,"
                "recovery,is_test "
                "FROM tarot_house_wager WHERE user_id=? ORDER BY "
                "accepted_at_ms DESC,wager_id DESC LIMIT 20");
  query.bind(1, user_id.str());
  if (reference)
    query.bind(2, *reference);
  std::vector<HouseWagerRecord> result;
  while (query.step())
    result.push_back(record_from(query));
  return result;
}

TarotPlayerRecord
SqliteTarotHouseRepository::record(const DiscordSnowflake &user_id) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  TarotPlayerRecord result;
  auto query = connection.prepare(
      "SELECT wins,losses,current_win_streak,current_loss_streak,"
      "settled_house_wagers FROM tarot_player_stats WHERE user_id=?");
  query.bind(1, user_id.str());
  if (query.step()) {
    result.wins = static_cast<std::size_t>(query.column_int64(0));
    result.losses = static_cast<std::size_t>(query.column_int64(1));
    result.current_win_streak = static_cast<std::size_t>(query.column_int64(2));
    result.current_loss_streak =
        static_cast<std::size_t>(query.column_int64(3));
    result.settled_house_wagers =
        static_cast<std::size_t>(query.column_int64(4));
  }
  auto titles = connection.prepare(
      "SELECT DISTINCT title_name FROM tarot_title_source WHERE user_id=? "
      "AND state='proposed' ORDER BY title_name");
  titles.bind(1, user_id.str());
  while (titles.step())
    result.pending_titles.push_back(titles.column_text(0));
  return result;
}

HouseEconomyReport SqliteTarotHouseRepository::economy() {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  HouseEconomyReport result;
  const auto scalar_amount = [&](const std::string_view sql) {
    auto query = connection.prepare(sql);
    if (!query.step())
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA, "House economy query failed."};
    return query.column_int64(0);
  };
  result.account_total = scalar_amount(
      "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id WHERE "
      "tx.state='committed'");
  result.issued_fate = scalar_amount(
      "SELECT -COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id JOIN "
      "tarot_account account ON account.account_id=post.account_id WHERE "
      "tx.state='committed' AND tx.is_test=0 AND account.account_kind='MINT'");
  result.human_fate = scalar_amount(
      "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id JOIN "
      "tarot_account account ON account.account_id=post.account_id WHERE "
      "tx.state='committed' AND tx.is_test=0 AND account.account_kind='HUMAN'");
  result.house_fate = scalar_amount(
      "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id JOIN "
      "tarot_account account ON account.account_id=post.account_id WHERE "
      "tx.state='committed' AND tx.is_test=0 AND account.account_kind='HOUSE'");
  result.recovery_issuance = scalar_amount(
      "SELECT -COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id JOIN "
      "tarot_account account ON account.account_id=post.account_id WHERE "
      "tx.state='committed' AND tx.is_test=0 AND account.account_kind='MINT' "
      "AND (tx.transaction_type IN ('GRACE','TRIAL') OR EXISTS(SELECT 1 FROM "
      "tarot_house_transfer transfer JOIN tarot_house_wager wager ON "
      "wager.wager_id=transfer.wager_id WHERE transfer.transaction_id="
      "tx.transaction_id AND wager.recovery=1))");
  auto exposure = connection.prepare(
      "SELECT COALESCE((SELECT sum(profit) FROM tarot_house_wager WHERE "
      "is_test=0 AND state='accepted_funded' AND "
      "recovery=0),0)+COALESCE((SELECT "
      "sum(reserved_profit) FROM tarot_house_offer WHERE is_test=0 AND "
      "state='open'),0),COALESCE((SELECT sum(profit) FROM tarot_house_wager "
      "WHERE is_test=1 AND state='accepted_funded' AND "
      "recovery=0),0)+COALESCE((SELECT sum(reserved_profit) FROM "
      "tarot_house_offer WHERE is_test=1 AND state='open'),0),(SELECT count(*) "
      "FROM tarot_house_wager WHERE "
      "state='accepted_funded')");
  if (!exposure.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House exposure report failed."};
  result.non_test_exposure = exposure.column_int64(0);
  result.test_exposure = exposure.column_int64(1);
  result.open_house_wagers = static_cast<std::size_t>(exposure.column_int64(2));
  auto expected = connection.prepare(
      "SELECT COALESCE(sum(stake+profit),0) FROM tarot_house_wager "
      "WHERE state='accepted_funded'");
  if (!expected.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House escrow report failed."};
  result.expected_house_escrow = expected.column_int64(0);
  result.escrow_balance = scalar_amount(
      "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
      "tarot_transaction tx ON tx.transaction_id=post.transaction_id JOIN "
      "tarot_account account ON account.account_id=post.account_id WHERE "
      "tx.state='committed' AND account.account_kind='ESCROW'");
  auto peer = connection.prepare(
      "SELECT COALESCE(sum(2*stake),0) FROM tarot_wager WHERE state IN "
      "('accepted_funded','awaiting_resolution','disputed')");
  if (!peer.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Peer escrow report failed."};
  auto malformed = connection.prepare(
      "SELECT count(*) FROM tarot_house_wager wager WHERE "
      "NOT EXISTS(SELECT 1 FROM tarot_house_transfer transfer WHERE "
      "transfer.wager_id=wager.wager_id AND transfer.transfer_kind='fund') "
      "OR (wager.state<>'accepted_funded' AND NOT EXISTS(SELECT 1 FROM "
      "tarot_house_transfer transfer WHERE transfer.wager_id=wager.wager_id "
      "AND transfer.transfer_kind IN ('payout','refund')))");
  if (!malformed.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House linkage report failed."};
  result.malformed_transfer_count =
      static_cast<std::size_t>(malformed.column_int64(0));
  auto malformed_offer_deadlines = connection.prepare(
      "SELECT count(*) FROM tarot_house_offer offer WHERE "
      "offer.state<>'skipped' AND NOT EXISTS("
      "SELECT 1 FROM tarot_house_offer_deadline deadline JOIN scheduled_job "
      "job ON job.job_id=deadline.job_id WHERE deadline.offer_id=offer.offer_"
      "id AND deadline.due_at_ms=offer.closes_at_ms AND "
      "job.job_type='tarot.house-offer-expiry.v1' AND "
      "job.aggregate_type='tarot_house_offer' AND job.aggregate_id=offer."
      "offer_id AND job.due_at_ms=offer.closes_at_ms)");
  if (!malformed_offer_deadlines.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "House offer-deadline report failed."};
  result.malformed_offer_deadline_count =
      static_cast<std::size_t>(malformed_offer_deadlines.column_int64(0));
  result.valid = result.escrow_balance ==
                     result.expected_house_escrow + peer.column_int64(0) &&
                 result.malformed_transfer_count == 0 &&
                 result.malformed_offer_deadline_count == 0 &&
                 result.account_total == 0;
  return result;
}

TarotPlayerProjectionReport
SqliteTarotHouseRepository::check_player_projection() {
  std::scoped_lock lock{context_->mutex()};
  return check_player_projection_unlocked(context_->connection());
}

TarotPlayerProjectionReport
SqliteTarotHouseRepository::rebuild_player_projection() {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto expected = expected_player_projection(connection);
  connection.execute("DELETE FROM tarot_player_stats");
  auto insert = connection.prepare(
      "INSERT INTO tarot_player_stats(user_id,wins,losses,current_win_streak,"
      "current_loss_streak,settled_house_wagers,last_event_id,rebuilt_at_ms) "
      "VALUES(?,?,?,?,?,?,?,?)");
  for (const auto &[user_id, value] : expected.users) {
    insert.bind(1, user_id);
    insert.bind(2, value.wins);
    insert.bind(3, value.losses);
    insert.bind(4, value.win_streak);
    insert.bind(5, value.loss_streak);
    insert.bind(6, value.settled_house);
    insert.bind(7, value.last_event_id);
    insert.bind(8, value.rebuilt_at_ms);
    insert.execute();
    insert.reset();
  }
  transaction.commit();
  return check_player_projection_unlocked(connection);
}

} // namespace sanguinius::persistence
