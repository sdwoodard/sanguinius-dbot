#include "sanguinius/persistence/sqlite_wager_repository.hpp"

#include "sanguinius/durable_work.hpp"
#include "sanguinius/pending_notice.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::int64_t checked_add(const std::int64_t left,
                                       const std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    throw std::overflow_error{"Wager arithmetic overflowed."};
  return left + right;
}

[[nodiscard]] std::int64_t checked_twice(const std::int64_t value) {
  if (value <= 0 || value > std::numeric_limits<std::int64_t>::max() / 2)
    throw std::overflow_error{"Wager stake overflowed."};
  return value * 2;
}

void require_id_factory(const WagerIdFactory &next_id) {
  if (!next_id)
    throw std::invalid_argument{"A wager ID factory is required."};
}

[[nodiscard]] std::string next_id(const WagerIdFactory &factory) {
  const auto value = factory();
  if (!valid_uuid_v4(value))
    throw std::invalid_argument{"Wager IDs must be UUIDv4 values."};
  return value;
}

[[nodiscard]] bool blank(const std::string_view value) {
  return value.empty() ||
         std::ranges::all_of(value, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

[[nodiscard]] std::string state_name(const WagerState state) {
  switch (state) {
  case WagerState::draft:
    return "draft";
  case WagerState::offered:
    return "offered";
  case WagerState::accepted_funded:
    return "accepted_funded";
  case WagerState::awaiting_resolution:
    return "awaiting_resolution";
  case WagerState::disputed:
    return "disputed";
  case WagerState::resolved:
    return "resolved";
  case WagerState::void_refunded:
    return "void_refunded";
  case WagerState::cancelled:
    return "cancelled";
  case WagerState::declined:
    return "declined";
  case WagerState::expired:
    return "expired";
  }
  throw std::invalid_argument{"Wager state is invalid."};
}

[[nodiscard]] WagerState state_value(const std::string_view state) {
  if (state == "draft")
    return WagerState::draft;
  if (state == "offered")
    return WagerState::offered;
  if (state == "accepted_funded")
    return WagerState::accepted_funded;
  if (state == "awaiting_resolution")
    return WagerState::awaiting_resolution;
  if (state == "disputed")
    return WagerState::disputed;
  if (state == "resolved")
    return WagerState::resolved;
  if (state == "void_refunded")
    return WagerState::void_refunded;
  if (state == "cancelled")
    return WagerState::cancelled;
  if (state == "declined")
    return WagerState::declined;
  if (state == "expired")
    return WagerState::expired;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored wager state is incompatible."};
}

[[nodiscard]] std::string visibility_name(const WagerVisibility visibility) {
  return visibility == WagerVisibility::sealed ? "sealed" : "public";
}

[[nodiscard]] WagerVisibility visibility_value(const std::string_view value) {
  if (value == "public")
    return WagerVisibility::public_offer;
  if (value == "sealed")
    return WagerVisibility::sealed;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA,
                      "Stored wager visibility is incompatible."};
}

[[nodiscard]] std::string resolution_name(const WagerResolutionPolicy policy) {
  return policy == WagerResolutionPolicy::designated ? "designated" : "mutual";
}

[[nodiscard]] WagerResolutionPolicy
resolution_value(const std::string_view value) {
  if (value == "mutual")
    return WagerResolutionPolicy::mutual;
  if (value == "designated")
    return WagerResolutionPolicy::designated;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA,
                      "Stored wager resolution is incompatible."};
}

[[nodiscard]] std::optional<std::string>
optional_text(SqliteStatement &statement, const int column) {
  return statement.column_is_null(column)
             ? std::nullopt
             : std::optional<std::string>{statement.column_text(column)};
}

[[nodiscard]] std::optional<std::int64_t>
optional_int(SqliteStatement &statement, const int column) {
  return statement.column_is_null(column)
             ? std::nullopt
             : std::optional<std::int64_t>{statement.column_int64(column)};
}

[[nodiscard]] std::optional<DiscordSnowflake>
optional_snowflake(SqliteStatement &statement, const int column) {
  const auto value = optional_text(statement, column);
  return value
             ? std::optional<DiscordSnowflake>{DiscordSnowflake::parse(*value)}
             : std::nullopt;
}

[[nodiscard]] WagerRecord record_from(SqliteStatement &query) {
  const auto winner = optional_text(query, 24);
  std::optional<WagerRole> winner_role;
  if (winner == std::optional<std::string>{"creator"})
    winner_role = WagerRole::creator;
  else if (winner == std::optional<std::string>{"target"})
    winner_role = WagerRole::target;
  return WagerRecord{
      .wager_id = query.column_text(0),
      .state = state_value(query.column_text(1)),
      .revision = static_cast<std::size_t>(query.column_int64(2)),
      .guild_id = DiscordSnowflake::parse(query.column_text(3)),
      .channel_id = DiscordSnowflake::parse(query.column_text(4)),
      .creator_user_id = DiscordSnowflake::parse(query.column_text(5)),
      .target_user_id = DiscordSnowflake::parse(query.column_text(6)),
      .judge_user_id = optional_snowflake(query, 7),
      .creator_display_name = query.column_text(8),
      .target_display_name = query.column_text(9),
      .judge_display_name = optional_text(query, 10),
      .visibility = visibility_value(query.column_text(11)),
      .resolution_policy = resolution_value(query.column_text(12)),
      .proposition = optional_text(query, 13),
      .stake = optional_int(query, 14),
      .evidence_instructions = optional_text(query, 15),
      .outcome_window_ms = query.column_int64(16),
      .resolution_grace_ms = query.column_int64(17),
      .offer_duration_ms = optional_int(query, 18),
      .offer_expires_at_ms = optional_int(query, 19),
      .outcome_due_at_ms = optional_int(query, 20),
      .resolution_grace_until_ms = optional_int(query, 21),
      .winner = winner_role,
      .terminal_reason = optional_text(query, 23),
      .is_test = query.column_int64(25) != 0,
      .created_at_ms = query.column_int64(26),
      .updated_at_ms = query.column_int64(27),
  };
}

[[nodiscard]] std::optional<WagerRecord>
load_wager(SqliteConnection &connection, const std::string_view wager_id) {
  auto query = connection.prepare(
      "SELECT wager.wager_id, wager.state, wager.revision, "
      "wager.guild_id, wager.channel_id, wager.creator_user_id, "
      "wager.target_user_id, wager.judge_user_id, "
      "COALESCE(creator.display_name_cache, creator.username_cache, 'member'), "
      "COALESCE(target.display_name_cache, target.username_cache, 'member'), "
      "COALESCE(judge.display_name_cache, judge.username_cache), "
      "wager.visibility, wager.resolution_policy, wager.proposition, "
      "wager.stake, "
      "wager.evidence_instructions, wager.outcome_window_ms, "
      "wager.resolution_grace_ms, "
      "wager.offer_duration_ms, wager.offer_expires_at_ms, "
      "wager.outcome_due_at_ms, "
      "wager.resolution_grace_until_ms, wager.judged_by_user_id, "
      "wager.terminal_reason, wager.winner_role, wager.is_test, "
      "wager.created_at_ms, wager.updated_at_ms FROM tarot_wager wager "
      "JOIN discord_user creator ON creator.user_id = wager.creator_user_id "
      "JOIN discord_user target ON target.user_id = wager.target_user_id "
      "LEFT JOIN discord_user judge ON judge.user_id = wager.judge_user_id "
      "WHERE wager.wager_id = ?");
  query.bind(1, wager_id);
  if (!query.step())
    return std::nullopt;
  auto result = record_from(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate wager identity."};
  return result;
}

[[nodiscard]] EventJournalEntry
event_for(const WagerInvocation &call, std::string event_id,
          std::string event_type, const std::string_view wager_id,
          std::string idempotency_key, Json payload = Json::object(),
          std::optional<DiscordSnowflake> actor = std::nullopt) {
  return EventJournalEntry{
      .event_id = std::move(event_id),
      .event_type = std::move(event_type),
      .aggregate_type = "tarot_wager",
      .aggregate_id = std::string{wager_id},
      .actor_user_id = actor.has_value() ? actor : std::optional{call.user_id},
      .guild_id = call.guild_id,
      .channel_id = call.channel_id,
      .source_message_id = std::nullopt,
      .occurred_at_ms = call.now_ms,
      .recorded_at_ms = call.now_ms,
      .correlation_id = call.correlation_id,
      .causation_id = std::nullopt,
      .idempotency_key = std::move(idempotency_key),
      .payload_json = payload.dump(),
  };
}

void insert_action(SqliteConnection &connection, const WagerIdFactory &ids,
                   const WagerInvocation &call, const WagerRecord &wager,
                   const WagerRole role, const std::string_view action,
                   const std::string_view event_id,
                   const std::string_view idempotency_key,
                   const std::optional<std::string> &reason = std::nullopt) {
  auto insert =
      connection.prepare("INSERT INTO tarot_wager_action "
                         "(action_id, wager_id, wager_revision, actor_user_id, "
                         "actor_role, action, "
                         "event_id, reason, idempotency_key, occurred_at_ms) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, next_id(ids));
  insert.bind(2, wager.wager_id);
  insert.bind(3, static_cast<std::int64_t>(wager.revision));
  insert.bind(4, call.user_id.str());
  insert.bind(5, wager_role_name(role));
  insert.bind(6, action);
  insert.bind(7, event_id);
  if (reason)
    insert.bind(8, *reason);
  else
    insert.bind_null(8);
  insert.bind(9, idempotency_key);
  insert.bind(10, call.now_ms);
  insert.execute();
}

void insert_receipt(SqliteConnection &connection, const WagerInvocation &call,
                    const std::optional<std::string> &wager_id,
                    const std::string_view operation,
                    const std::string_view fingerprint,
                    const WagerMutationStatus status,
                    const Json &result = Json::object()) {
  const auto status_text = [status] {
    switch (status) {
    case WagerMutationStatus::applied:
      return "applied";
    case WagerMutationStatus::unchanged:
      return "unchanged";
    case WagerMutationStatus::forbidden:
      return "forbidden";
    case WagerMutationStatus::invalid_state:
      return "invalid_state";
    case WagerMutationStatus::expired:
      return "expired";
    case WagerMutationStatus::insufficient_funds:
      return "insufficient_funds";
    case WagerMutationStatus::not_found:
      return "not_found";
    case WagerMutationStatus::stale:
      return "stale";
    }
    return "invalid_state";
  }();
  auto insert = connection.prepare(
      "INSERT INTO tarot_wager_receipt "
      "(idempotency_key, wager_id, operation, request_fingerprint, status, "
      "result_json, created_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, call.interaction_idempotency_key);
  if (wager_id)
    insert.bind(2, *wager_id);
  else
    insert.bind_null(2);
  insert.bind(3, operation);
  insert.bind(4, fingerprint);
  insert.bind(5, status_text);
  insert.bind(6, result.dump());
  insert.bind(7, call.now_ms);
  insert.execute();
}

[[nodiscard]] WagerMutationResult
empty_mutation_result(const WagerMutationStatus status,
                      std::optional<WagerRecord> wager = std::nullopt) {
  return {.status = status,
          .wager = std::move(wager),
          .controls = {},
          .committed_event_types = {},
          .public_delivery_created = false};
}

[[nodiscard]] WagerHistoryResult
empty_history_result(const WagerMutationStatus status) {
  return {.status = status,
          .wagers = {},
          .outcomes = {},
          .evidence = {},
          .evidence_total_count = 0,
          .next_cursor_id = std::nullopt,
          .controls = {},
          .exact = false};
}

[[nodiscard]] std::optional<WagerMutationResult>
replay_receipt(SqliteConnection &connection, const WagerInvocation &call,
               const std::string_view operation,
               const std::string_view fingerprint) {
  auto query = connection.prepare(
      "SELECT operation, request_fingerprint, status, wager_id "
      "FROM tarot_wager_receipt WHERE idempotency_key = ?");
  query.bind(1, call.interaction_idempotency_key);
  if (!query.step())
    return std::nullopt;
  if (query.column_text(0) != operation || query.column_text(1) != fingerprint)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Wager interaction idempotency conflict."};
  const auto stored_status = query.column_text(2);
  WagerMutationStatus status = WagerMutationStatus::unchanged;
  if (stored_status == "forbidden")
    status = WagerMutationStatus::forbidden;
  else if (stored_status == "invalid_state")
    status = WagerMutationStatus::invalid_state;
  else if (stored_status == "expired")
    status = WagerMutationStatus::expired;
  else if (stored_status == "insufficient_funds")
    status = WagerMutationStatus::insufficient_funds;
  else if (stored_status == "not_found")
    status = WagerMutationStatus::not_found;
  else if (stored_status == "stale")
    status = WagerMutationStatus::stale;
  const auto wager_id = optional_text(query, 3);
  return WagerMutationResult{
      .status =
          stored_status == "applied" ? WagerMutationStatus::unchanged : status,
      .wager = wager_id ? load_wager(connection, *wager_id) : std::nullopt,
      .controls = {},
      .committed_event_types = {},
      .public_delivery_created = false,
  };
}

[[nodiscard]] std::string account_for_user(SqliteConnection &connection,
                                           const DiscordSnowflake &user_id) {
  auto query = connection.prepare("SELECT account_id FROM tarot_account WHERE "
                                  "account_kind = 'HUMAN' AND user_id = ?");
  query.bind(1, user_id.str());
  if (!query.step())
    return {};
  return query.column_text(0);
}

[[nodiscard]] std::string system_account(SqliteConnection &connection,
                                         const std::string_view kind) {
  auto query = connection.prepare(
      "SELECT account_id FROM tarot_account WHERE account_kind = ?");
  query.bind(1, kind);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot system account is missing."};
  return query.column_text(0);
}

[[nodiscard]] std::int64_t account_balance(SqliteConnection &connection,
                                           const std::string_view account_id) {
  auto query = connection.prepare(
      "SELECT posting.amount FROM tarot_posting posting "
      "JOIN tarot_transaction tx ON tx.transaction_id = posting.transaction_id "
      "WHERE posting.account_id = ? AND tx.state = 'committed' "
      "ORDER BY tx.ledger_sequence, posting.posting_id");
  query.bind(1, account_id);
  std::int64_t balance{};
  while (query.step())
    balance = checked_add(balance, query.column_int64(0));
  return balance;
}

void insert_transaction(
    SqliteConnection &connection, const std::string_view transaction_id,
    const std::string_view transaction_type, const std::size_t posting_count,
    const std::string_view event_id, const std::string_view idempotency_key,
    const DiscordSnowflake &actor, const std::optional<std::string> &reason,
    const bool is_test, const std::optional<std::string> &reversal_of,
    const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_transaction "
      "(transaction_id, transaction_type, state, expected_posting_count, "
      "event_id, idempotency_key, actor_user_id, reason, is_test, "
      "reversal_of_transaction_id, created_at_ms) "
      "VALUES (?, ?, 'prepared', ?, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, transaction_id);
  insert.bind(2, transaction_type);
  insert.bind(3, static_cast<std::int64_t>(posting_count));
  insert.bind(4, event_id);
  insert.bind(5, idempotency_key);
  insert.bind(6, actor.str());
  if (reason)
    insert.bind(7, *reason);
  else
    insert.bind_null(7);
  insert.bind(8, is_test ? 1 : 0);
  if (reversal_of)
    insert.bind(9, *reversal_of);
  else
    insert.bind_null(9);
  insert.bind(10, now_ms);
  insert.execute();
}

void insert_posting(SqliteConnection &connection, const WagerIdFactory &ids,
                    const std::string_view transaction_id,
                    const std::string_view account_id,
                    const std::int64_t amount, const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_posting "
      "(posting_id, transaction_id, account_id, amount, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, next_id(ids));
  insert.bind(2, transaction_id);
  insert.bind(3, account_id);
  insert.bind(4, amount);
  insert.bind(5, now_ms);
  insert.execute();
}

void seal_transaction(SqliteConnection &connection,
                      const std::string_view transaction_id,
                      const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE tarot_transaction SET state = 'committed', committed_at_ms = ? "
      "WHERE transaction_id = ? AND state = 'prepared'");
  update.bind(1, now_ms);
  update.bind(2, transaction_id);
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "Wager transaction could not seal."};
}

[[nodiscard]] std::string ensure_account(SqliteConnection &connection,
                                         const WagerInvocation &call,
                                         const DiscordSnowflake &user_id,
                                         const std::int64_t starting_fate,
                                         const WagerIdFactory &ids) {
  if (auto existing = account_for_user(connection, user_id); !existing.empty())
    return existing;
  const auto account_id = next_id(ids);
  auto account = connection.prepare("INSERT INTO tarot_account "
                                    "(account_id, account_kind, user_id, "
                                    "created_at_ms) VALUES (?, 'HUMAN', ?, ?)");
  account.bind(1, account_id);
  account.bind(2, user_id.str());
  account.bind(3, call.now_ms);
  account.execute();

  const auto event_id = next_id(ids);
  const auto idempotency = "tarot.starting_grant:" + user_id.str();
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      EventJournalEntry{
          .event_id = event_id,
          .event_type = "tarot.starting_grant.v1",
          .aggregate_type = "tarot_account",
          .aggregate_id = account_id,
          .actor_user_id = user_id,
          .guild_id = call.guild_id,
          .channel_id = call.channel_id,
          .source_message_id = std::nullopt,
          .occurred_at_ms = call.now_ms,
          .recorded_at_ms = call.now_ms,
          .correlation_id = call.correlation_id,
          .causation_id = std::nullopt,
          .idempotency_key = idempotency,
          .payload_json =
              Json{{"amount", starting_fate}, {"currency", "Fate"}}.dump(),
      }));
  const auto transaction_id = next_id(ids);
  insert_transaction(connection, transaction_id, "STARTING_GRANT", 2, event_id,
                     idempotency, user_id, std::nullopt, false, std::nullopt,
                     call.now_ms);
  insert_posting(connection, ids, transaction_id,
                 system_account(connection, "MINT"), -starting_fate,
                 call.now_ms);
  insert_posting(connection, ids, transaction_id, account_id, starting_fate,
                 call.now_ms);
  seal_transaction(connection, transaction_id, call.now_ms);
  return account_id;
}

[[nodiscard]] bool test_access_allowed(const WagerInvocation &call,
                                       const WagerRecord &wager) {
  return !wager.is_test || (call.owner && call.test_mode);
}

[[nodiscard]] WagerRole
role_for(SqliteConnection &connection, const WagerInvocation &call,
         const WagerRecord &wager,
         const std::optional<std::string> &control_token) {
  if (!test_access_allowed(call, wager))
    return WagerRole::scheduler;
  if (control_token && wager.is_test && call.owner && call.test_mode) {
    auto query = connection.prepare(
        "SELECT simulated_role FROM tarot_wager_control WHERE token_id = ?");
    query.bind(1, *control_token);
    if (query.step() && !query.column_is_null(0)) {
      const auto role = query.column_text(0);
      if (role == "creator")
        return WagerRole::creator;
      if (role == "target")
        return WagerRole::target;
      if (role == "judge")
        return WagerRole::judge;
      if (role == "owner")
        return WagerRole::owner;
    }
  }
  if (wager.is_test && call.owner && call.test_mode) {
    auto query = connection.prepare("SELECT role FROM tarot_wager_test_role "
                                    "WHERE wager_id = ? AND owner_user_id = ?");
    query.bind(1, wager.wager_id);
    query.bind(2, call.user_id.str());
    if (query.step()) {
      const auto role = query.column_text(0);
      if (role == "creator")
        return WagerRole::creator;
      if (role == "target")
        return WagerRole::target;
      if (role == "judge")
        return WagerRole::judge;
      if (role == "owner")
        return WagerRole::owner;
    }
  }
  if (call.user_id == wager.creator_user_id)
    return WagerRole::creator;
  if (call.user_id == wager.target_user_id)
    return WagerRole::target;
  if (wager.judge_user_id && call.user_id == *wager.judge_user_id)
    return WagerRole::judge;
  if (call.owner)
    return WagerRole::owner;
  return WagerRole::scheduler;
}

void insert_control(
    SqliteConnection &connection, const WagerIdFactory &ids,
    const WagerRecord &wager, const std::string_view action,
    const DiscordSnowflake &user, const WagerRole role,
    const std::int64_t expires_at_ms,
    std::vector<WagerMutationResult::Control> &result,
    const std::string_view label,
    const std::string_view custom_prefix = wager_component_prefix,
    const std::optional<std::int64_t> created_at_ms = std::nullopt) {
  const auto token = next_id(ids);
  auto insert = connection.prepare(
      "INSERT INTO tarot_wager_control "
      "(token_id, wager_id, action, expected_user_id, simulated_role, "
      "expected_revision, state, expires_at_ms, idempotency_key, "
      "created_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, 'active', ?, ?, ?)");
  insert.bind(1, token);
  insert.bind(2, wager.wager_id);
  insert.bind(3, action);
  insert.bind(4, user.str());
  if (wager.is_test)
    insert.bind(5, wager_role_name(role));
  else
    insert.bind_null(5);
  insert.bind(6, static_cast<std::int64_t>(wager.revision));
  insert.bind(7, expires_at_ms);
  insert.bind(8, "wager:control:" + wager.wager_id + ":" +
                     std::to_string(wager.revision) + ":" +
                     std::string{action} + ":" +
                     std::string{wager_role_name(role)} + ":" + token);
  insert.bind(9, created_at_ms.value_or(wager.updated_at_ms));
  insert.execute();
  result.push_back({std::string{custom_prefix} + token, std::string{label}});
}

void cancel_controls(SqliteConnection &connection,
                     const std::string_view wager_id) {
  auto update =
      connection.prepare("UPDATE tarot_wager_control SET state = 'cancelled' "
                         "WHERE wager_id = ? AND state = 'active'");
  update.bind(1, wager_id);
  update.execute();
}

void cancel_sealed_offer_notice(SqliteConnection &connection,
                                const std::string_view wager_id) {
  auto tokens = connection.prepare(
      "UPDATE interaction_token SET state = 'cancelled' "
      "WHERE entity_type = 'pending_notice' AND state = 'active' "
      "AND entity_id IN (SELECT notice_id FROM tarot_wager_notice "
      "WHERE wager_id = ? AND purpose = 'sealed_offer')");
  tokens.bind(1, wager_id);
  tokens.execute();

  auto notices = connection.prepare(
      "UPDATE pending_notice SET state = 'cancelled' "
      "WHERE notice_id IN (SELECT notice_id FROM tarot_wager_notice "
      "WHERE wager_id = ? AND purpose = 'sealed_offer') "
      "AND state IN ('pending', 'opened')");
  notices.bind(1, wager_id);
  notices.execute();
}

void schedule_job(SqliteConnection &connection, const WagerIdFactory &ids,
                  const WagerInvocation &call, const WagerRecord &wager,
                  const WagerDeadlinePhase phase,
                  const std::int64_t due_at_ms) {
  const auto job_id = next_id(ids);
  const auto phase_name = wager_deadline_phase_name(phase);
  const ScheduledJobEnqueue job{
      .job_id = job_id,
      .job_type = std::string{wager_deadline_job_type},
      .aggregate_type = "tarot_wager",
      .aggregate_id = wager.wager_id,
      .due_at_ms = due_at_ms,
      .max_attempts = 10,
      .idempotency_key = "job:wager:" + wager.wager_id + ":" + phase_name,
      .created_at_ms = call.now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_wager_deadline_payload(
          WagerDeadlineJobPayload{.wager_id = wager.wager_id,
                                  .phase = phase_name,
                                  .expected_revision = wager.revision},
          call.correlation_id, std::nullopt)));
  auto link = connection.prepare(
      "INSERT INTO tarot_wager_job "
      "(wager_id, phase, job_id, expected_revision) VALUES (?, ?, ?, ?)");
  link.bind(1, wager.wager_id);
  link.bind(2, phase_name);
  link.bind(3, job_id);
  link.bind(4, static_cast<std::int64_t>(wager.revision));
  link.execute();
}

[[nodiscard]] PublicMessageRequest
public_card(const WagerRecord &wager,
            const std::vector<WagerMutationResult::Control> &controls) {
  const bool sealed = wager.visibility == WagerVisibility::sealed;
  const auto public_state = [&wager, sealed] {
    if (!sealed)
      return state_name(wager.state);
    switch (wager.state) {
    case WagerState::offered:
      return std::string{"Offered"};
    case WagerState::accepted_funded:
    case WagerState::awaiting_resolution:
      return std::string{"Funded"};
    case WagerState::disputed:
      return std::string{"Disputed"};
    case WagerState::resolved:
    case WagerState::void_refunded:
    case WagerState::cancelled:
    case WagerState::declined:
    case WagerState::expired:
      return std::string{"Closed"};
    case WagerState::draft:
      return std::string{"Draft"};
    }
    return std::string{"Closed"};
  }();
  std::string description = "Status: " + public_state + ".";
  std::string content;
  if (sealed) {
    content = (wager.is_test ? "[TEST] " : "") + std::string{"<@"} +
              wager.target_user_id.str() + ">, a sealed Fate wager awaits.";
    description += " The terms remain sealed to the addressed participant.";
  } else {
    content = wager.is_test ? "[TEST] Peer Fate wager" : "Peer Fate wager";
    if (wager.proposition)
      description += "\nProposition: " + *wager.proposition;
    if (wager.stake)
      description += "\nStake: " + std::to_string(*wager.stake) + " Fate each.";
    description += "\nCreator: <@" + wager.creator_user_id.str() +
                   ">\nTarget: <@" + wager.target_user_id.str() + ">";
    description += wager.resolution_policy == WagerResolutionPolicy::mutual
                       ? "\nResolution: mutual agreement."
                       : "\nResolution: designated judge <@" +
                             wager.judge_user_id->str() + ">.";
    if (wager.offer_expires_at_ms)
      description +=
          "\nOffer deadline: <t:" +
          std::to_string(*wager.offer_expires_at_ms / 1'000) +
          ":F> (<t:" + std::to_string(*wager.offer_expires_at_ms / 1'000) +
          ":R>).";
    if (!wager.outcome_due_at_ms) {
      description += "\nOutcome window: " +
                     std::to_string(wager.outcome_window_ms / 3'600'000) +
                     " hours after acceptance.";
      description += "\nOwner escalation: " +
                     std::to_string(wager.resolution_grace_ms / 3'600'000) +
                     " hours after the outcome deadline.";
    }
    if (wager.outcome_due_at_ms)
      description +=
          "\nOutcome deadline: <t:" +
          std::to_string(*wager.outcome_due_at_ms / 1'000) +
          ":F> (<t:" + std::to_string(*wager.outcome_due_at_ms / 1'000) +
          ":R>).";
    if (wager.resolution_grace_until_ms)
      description += "\nOwner escalation after: <t:" +
                     std::to_string(*wager.resolution_grace_until_ms / 1'000) +
                     ":F> (<t:" +
                     std::to_string(*wager.resolution_grace_until_ms / 1'000) +
                     ":R>).";
    if (wager.state == WagerState::resolved && wager.winner) {
      const auto winner = *wager.winner == WagerRole::creator
                              ? wager.creator_user_id
                              : wager.target_user_id;
      description += "\nWinner: <@" + winner.str() + ">";
    } else if (wager.state == WagerState::void_refunded) {
      description += "\nBoth equal stakes were refunded.";
    }
  }
  InteractionMessage message{
      .content = std::move(content),
      .embed =
          EmbedPayload{.color = 0x8B0000U,
                       .title = sealed ? "A sealed wager awaits"
                                       : "The Emperor's Tarot — Peer Wager",
                       .description = std::move(description)},
      .buttons = {},
      .allowed_user_mentions =
          sealed ? std::vector<DiscordId>{wager.target_user_id}
                 : std::vector<DiscordId>{},
  };
  if (!sealed) {
    for (const auto &control : controls)
      message.buttons.push_back(
          {.custom_id = control.custom_id, .label = control.action});
  }
  return PublicMessageRequest{
      .guild_id = {}, .channel_id = {}, .message = std::move(message)};
}

void insert_pending_notice(
    SqliteConnection &connection, const WagerIdFactory &ids,
    const WagerInvocation &call, const WagerRecord &wager,
    const DiscordSnowflake &target, const std::string_view purpose,
    const std::string_view title, const std::string &body,
    const std::vector<WagerMutationResult::Control> &controls,
    const std::int64_t expires_at_ms,
    std::optional<std::string> *open_custom_id = nullptr) {
  const auto notice_id = next_id(ids);
  Json actions = Json::array();
  for (const auto &control : controls)
    actions.push_back(
        {{"custom_id", control.custom_id}, {"label", control.action}});
  const auto payload =
      Json{{"title", title}, {"body", body}, {"actions", actions}}.dump();
  auto notice = connection.prepare(
      "INSERT INTO pending_notice "
      "(notice_id, target_user_id, notice_type, payload_json, "
      "source_aggregate_type, source_aggregate_id, state, expires_at_ms, "
      "idempotency_key, created_at_ms) "
      "VALUES (?, ?, 'tarot_wager', ?, 'tarot_wager', ?, 'pending', ?, ?, ?)");
  notice.bind(1, notice_id);
  notice.bind(2, target.str());
  notice.bind(3, payload);
  notice.bind(4, wager.wager_id);
  notice.bind(5, expires_at_ms);
  notice.bind(6, "notice:wager:" + wager.wager_id + ":" + std::string{purpose} +
                     ":" + target.str());
  notice.bind(7, call.now_ms);
  notice.execute();

  const auto token_id = next_id(ids);
  auto token = connection.prepare(
      "INSERT INTO interaction_token "
      "(token_id, token_version, interaction_kind, action, entity_type, "
      "entity_id, "
      "expected_user_id, guild_id, channel_id, state, expires_at_ms, "
      "idempotency_key, created_at_ms) "
      "VALUES (?, 1, 'button', 'notice.open', 'pending_notice', ?, ?, ?, ?, "
      "'active', ?, ?, ?)");
  token.bind(1, token_id);
  token.bind(2, notice_id);
  token.bind(3, target.str());
  token.bind(4, call.guild_id.str());
  token.bind(5, call.channel_id.str());
  token.bind(6, expires_at_ms);
  token.bind(7, "token:wager-notice:" + notice_id);
  token.bind(8, call.now_ms);
  token.execute();

  auto link = connection.prepare(
      "INSERT INTO tarot_wager_notice "
      "(wager_id, purpose, target_user_id, notice_id) VALUES (?, ?, ?, ?)");
  link.bind(1, wager.wager_id);
  link.bind(2, purpose);
  link.bind(3, target.str());
  link.bind(4, notice_id);
  link.execute();
  if (open_custom_id)
    *open_custom_id = make_component_id(token_id);
}

[[nodiscard]] DiscordSnowflake guild_owner(SqliteConnection &connection,
                                           const WagerRecord &wager) {
  auto query = connection.prepare(
      "SELECT owner_user_id FROM guild_config WHERE guild_id = ?");
  query.bind(1, wager.guild_id.str());
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Wager guild owner is unavailable."};
  return DiscordSnowflake::parse(query.column_text(0));
}

void insert_lifecycle_notices(
    SqliteConnection &connection, const WagerIdFactory &ids,
    const WagerInvocation &call, const WagerRecord &wager,
    const std::string_view purpose, const std::string_view title,
    const std::string &body, std::vector<DiscordSnowflake> recipients,
    const std::int64_t expires_at_ms) {
  std::ranges::sort(recipients, {}, &DiscordSnowflake::value);
  const auto unique = std::ranges::unique(recipients);
  recipients.erase(unique.begin(), unique.end());
  const std::vector<WagerMutationResult::Control> no_controls;
  for (const auto &recipient : recipients) {
    insert_pending_notice(connection, ids, call, wager, recipient, purpose,
                          title, body, no_controls, expires_at_ms);
  }
}

void insert_dispute_notices(SqliteConnection &connection,
                            const WagerIdFactory &ids,
                            const WagerInvocation &call,
                            const WagerRecord &wager) {
  insert_lifecycle_notices(
      connection, ids, call, wager, "disputed", "Wager escalated",
      "Wager `" + wager.wager_id +
          "` is disputed. Escrow remains locked until agreement, mutual void, "
          "or reasoned owner judgment.",
      {wager.creator_user_id, wager.target_user_id,
       guild_owner(connection, wager)},
      checked_add(call.now_ms, 30LL * 24 * 60 * 60 * 1'000));
}

[[nodiscard]] std::string nonce(const std::string_view uuid) {
  return discord_nonce_from_uuid(uuid);
}

void enqueue_initial_card(
    SqliteConnection &connection, const WagerIdFactory &ids,
    const WagerInvocation &call, const WagerRecord &wager,
    std::vector<WagerMutationResult::Control> controls,
    const std::optional<std::string> &sealed_open_control) {
  if (sealed_open_control) {
    controls = {{*sealed_open_control, "Open sealed offer"}};
  }
  auto request = public_card(wager, controls);
  request.guild_id = call.guild_id;
  request.channel_id = call.channel_id;
  if (wager.visibility == WagerVisibility::sealed && sealed_open_control) {
    request.message.buttons = {
        {.custom_id = *sealed_open_control, .label = "Open sealed offer"}};
  }
  const auto outbox_id = next_id(ids);
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "tarot_wager",
      .aggregate_id = wager.wager_id,
      .target_guild_id = call.guild_id,
      .target_channel_id = call.channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = call.now_ms,
      .max_attempts = 5,
      .idempotency_key = "outbox:wager-card:" + wager.wager_id,
      .provider_nonce = nonce(outbox_id),
      .created_at_ms = call.now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_payload(
          PublicOutboxPayload{.request = std::move(request)},
          call.correlation_id, std::nullopt)));
  auto link = connection.prepare(
      "INSERT INTO tarot_wager_public_card "
      "(wager_id, create_outbox_id, created_revision) VALUES (?, ?, ?)");
  link.bind(1, wager.wager_id);
  link.bind(2, outbox_id);
  link.bind(3, static_cast<std::int64_t>(wager.revision));
  link.execute();
}

void complete_job_claim(SqliteConnection &connection,
                        const ClaimedScheduledJob &job,
                        const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state = 'completed', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, updated_at_ms = max(?, "
      "updated_at_ms), "
      "completed_at_ms = max(?, created_at_ms), terminal_at_ms = max(?, "
      "created_at_ms) "
      "WHERE job_id = ? AND state = 'claimed' AND lease_owner = ? AND "
      "lease_token = ?");
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
                        "Wager deadline claim became stale."};
}

[[nodiscard]] std::uint64_t fingerprint_hash(const std::string_view value,
                                             std::uint64_t hash) {
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  for (const auto character : value) {
    hash ^= static_cast<unsigned char>(character);
    hash *= prime;
  }
  return hash;
}

[[nodiscard]] std::string fingerprint_hex(std::uint64_t value) {
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result(16, '0');
  for (auto index = result.size(); index > 0; --index) {
    result[index - 1] = digits[static_cast<std::size_t>(value & 0x0fU)];
    value >>= 4U;
  }
  return result;
}

[[nodiscard]] std::string fingerprint(const std::string_view operation,
                                      const std::string_view wager_id,
                                      const std::string_view detail = {}) {
  const auto canonical =
      std::to_string(operation.size()) + ":" + std::string{operation} +
      std::to_string(wager_id.size()) + ":" + std::string{wager_id} +
      std::to_string(detail.size()) + ":" + std::string{detail};
  const auto first = fingerprint_hash(canonical, 14'695'981'039'346'656'037ULL);
  const auto second = fingerprint_hash(canonical, 7'803'984'560'294'236'053ULL);
  return "v1:" + fingerprint_hex(first) + fingerprint_hex(second);
}

[[nodiscard]] bool participant(const WagerRole role) {
  return role == WagerRole::creator || role == WagerRole::target;
}

[[nodiscard]] bool funded_open(const WagerState state) {
  return state == WagerState::accepted_funded ||
         state == WagerState::awaiting_resolution ||
         state == WagerState::disputed;
}

[[nodiscard]] bool sealed_offer_delivered(SqliteConnection &connection,
                                          const WagerRecord &wager) {
  if (wager.visibility != WagerVisibility::sealed)
    return true;
  auto query = connection.prepare(
      "SELECT 1 FROM tarot_wager_notice link "
      "JOIN pending_notice notice ON notice.notice_id = link.notice_id "
      "WHERE link.wager_id = ? AND link.purpose = 'sealed_offer' "
      "AND link.target_user_id = ? AND notice.opened_at_ms IS NOT NULL "
      "LIMIT 1");
  query.bind(1, wager.wager_id);
  query.bind(2, wager.target_user_id.str());
  return query.step();
}

struct HistoryControlSpec {
  std::string_view action;
  std::string_view label;
  std::string_view prefix;
};

[[nodiscard]] std::optional<std::int64_t>
deadline_due_at(SqliteConnection &connection, const std::string_view wager_id,
                const std::string_view phase) {
  auto query =
      connection.prepare("SELECT job.due_at_ms FROM tarot_wager_job link "
                         "JOIN scheduled_job job ON job.job_id = link.job_id "
                         "WHERE link.wager_id = ? AND link.phase = ?");
  query.bind(1, wager_id);
  query.bind(2, phase);
  return query.step() ? std::optional<std::int64_t>{query.column_int64(0)}
                      : std::nullopt;
}

[[nodiscard]] std::vector<WagerMutationResult::Control>
current_controls(SqliteConnection &connection, const WagerIdFactory &ids,
                 const WagerInvocation &call, const WagerRecord &wager,
                 const WagerRole role) {
  if (wager.is_test && !(call.owner && call.test_mode))
    return {};
  std::vector<HistoryControlSpec> specs;
  if (wager.state == WagerState::draft && role == WagerRole::creator) {
    if (wager.proposition && wager.stake) {
      specs = {{"confirm", "Confirm offer", wager_component_prefix},
               {"discard", "Discard", wager_component_prefix}};
    } else {
      specs = {{"open_form", "Open wager form", wager_form_prefix}};
    }
  } else if (wager.state == WagerState::offered) {
    if (role == WagerRole::creator)
      specs = {{"cancel", "Cancel", wager_component_prefix}};
    else if (role == WagerRole::target)
      specs = {{"accept", "Accept and fund", wager_component_prefix},
               {"decline", "Decline", wager_component_prefix}};
  } else if (funded_open(wager.state) && participant(role)) {
    const bool outcome_allowed =
        wager.resolution_policy == WagerResolutionPolicy::mutual ||
        wager.state == WagerState::disputed;
    if (outcome_allowed) {
      auto submitted = connection.prepare(
          "SELECT 1 FROM tarot_wager_outcome WHERE wager_id = ? AND "
          "actor_role = ?");
      submitted.bind(1, wager.wager_id);
      submitted.bind(2, wager_role_name(role));
      if (!submitted.step())
        specs.push_back({"outcome", "Submit outcome", wager_outcome_prefix});
    }
    if (wager.state != WagerState::disputed)
      specs.push_back({"dispute", "Dispute", wager_component_prefix});
    specs.push_back({"void", "Consent to void", wager_component_prefix});
    specs.push_back(
        {"evidence", "Add private evidence", wager_evidence_prefix});
    if (wager.state == WagerState::disputed) {
      const auto other_role = role == WagerRole::creator
                                  ? std::string_view{"target"}
                                  : std::string_view{"creator"};
      auto outcome =
          connection.prepare("SELECT 1 FROM tarot_wager_outcome WHERE wager_id "
                             "= ? AND actor_role = ?");
      outcome.bind(1, wager.wager_id);
      outcome.bind(2, other_role);
      if (outcome.step())
        specs.push_back(
            {"agree", "Adopt other outcome", wager_component_prefix});
    }
  }

  std::vector<WagerMutationResult::Control> controls;
  if (specs.empty())
    return controls;
  const auto ordinary_expiry =
      checked_add(call.now_ms, wager_control_lifetime_ms);
  auto expiry = ordinary_expiry;
  if (wager.state == WagerState::offered && wager.offer_expires_at_ms)
    expiry = std::min(expiry, *wager.offer_expires_at_ms);
  if (wager.state == WagerState::draft) {
    const auto draft_expiry =
        deadline_due_at(connection, wager.wager_id, "draft_expiry");
    if (!draft_expiry)
      return {};
    expiry = std::min(expiry, *draft_expiry);
  }
  if (expiry <= call.now_ms)
    return controls;

  for (const auto &spec : specs) {
    auto existing = connection.prepare(
        "SELECT token_id FROM tarot_wager_control WHERE wager_id = ? "
        "AND action = ? AND expected_user_id = ? AND expected_revision = ? "
        "AND state = 'active' AND expires_at_ms > ? "
        "AND ((? = 0 AND simulated_role IS NULL) OR simulated_role = ?) "
        "ORDER BY created_at_ms DESC, token_id DESC LIMIT 1");
    existing.bind(1, wager.wager_id);
    existing.bind(2, spec.action);
    existing.bind(3, call.user_id.str());
    existing.bind(4, static_cast<std::int64_t>(wager.revision));
    existing.bind(5, call.now_ms);
    existing.bind(6, wager.is_test ? 1 : 0);
    existing.bind(7, wager_role_name(role));
    if (existing.step()) {
      controls.push_back({std::string{spec.prefix} + existing.column_text(0),
                          std::string{spec.label}});
      continue;
    }
    insert_control(connection, ids, wager, spec.action, call.user_id, role,
                   expiry, controls, spec.label, spec.prefix, call.now_ms);
  }
  return controls;
}

void insert_transfer_link(SqliteConnection &connection,
                          const WagerIdFactory &ids, const WagerRecord &wager,
                          const std::string_view kind,
                          const std::string_view transaction_id,
                          const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_wager_transfer "
      "(transfer_id, wager_id, transfer_kind, transaction_id, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, next_id(ids));
  insert.bind(2, wager.wager_id);
  insert.bind(3, kind);
  insert.bind(4, transaction_id);
  insert.bind(5, now_ms);
  insert.execute();
}

void cancel_job(SqliteConnection &connection, const std::string_view wager_id,
                const std::string_view phase, const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE scheduled_job SET state = 'cancelled', terminal_at_ms = max(?, "
      "created_at_ms), "
      "updated_at_ms = max(?, updated_at_ms), lease_owner = NULL, lease_token "
      "= NULL, "
      "lease_until_ms = NULL WHERE job_id = (SELECT job_id FROM "
      "tarot_wager_job "
      "WHERE wager_id = ? AND phase = ?) AND state = 'pending'");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, wager_id);
  update.bind(4, phase);
  update.execute();
}

void cancel_resolution_reminders(SqliteConnection &connection,
                                 const std::string_view wager_id,
                                 const std::int64_t now_ms) {
  auto outbox = connection.prepare(
      "UPDATE outbox_message SET state = 'cancelled', lease_owner = NULL, "
      "lease_token = NULL, lease_until_ms = NULL, "
      "updated_at_ms = max(?, updated_at_ms), "
      "terminal_at_ms = max(?, created_at_ms) "
      "WHERE aggregate_type = 'tarot_wager' AND aggregate_id = ? "
      "AND idempotency_key = 'outbox:wager-reminder:' || ? "
      "AND state = 'pending'");
  outbox.bind(1, now_ms);
  outbox.bind(2, now_ms);
  outbox.bind(3, wager_id);
  outbox.bind(4, wager_id);
  outbox.execute();

  auto tokens = connection.prepare(
      "UPDATE interaction_token SET state = 'cancelled' "
      "WHERE entity_type = 'pending_notice' AND state = 'active' "
      "AND entity_id IN (SELECT notice_id FROM tarot_wager_notice "
      "WHERE wager_id = ? AND purpose = 'reminder')");
  tokens.bind(1, wager_id);
  tokens.execute();

  auto notices = connection.prepare(
      "UPDATE pending_notice SET state = 'cancelled' "
      "WHERE notice_id IN (SELECT notice_id FROM tarot_wager_notice "
      "WHERE wager_id = ? AND purpose = 'reminder') "
      "AND state IN ('pending', 'opened')");
  notices.bind(1, wager_id);
  notices.execute();
}

[[nodiscard]] std::optional<std::string>
control_action(SqliteConnection &connection, const WagerActionRequest &request,
               std::string &wager_id, WagerMutationStatus &error) {
  if (!request.token_id)
    return std::nullopt;
  auto query = connection.prepare(
      "SELECT control.wager_id, control.action, control.expected_user_id, "
      "control.expected_revision, control.state, control.expires_at_ms, "
      "wager.guild_id, wager.channel_id, wager.revision "
      "FROM tarot_wager_control control JOIN tarot_wager wager "
      "ON wager.wager_id = control.wager_id WHERE control.token_id = ?");
  query.bind(1, *request.token_id);
  if (!query.step()) {
    error = WagerMutationStatus::not_found;
    return std::nullopt;
  }
  wager_id = query.column_text(0);
  const auto action = query.column_text(1);
  if (query.column_text(2) != request.invocation.user_id.str()) {
    error = WagerMutationStatus::forbidden;
    return action;
  }
  if (query.column_text(6) != request.invocation.guild_id.str() ||
      query.column_text(7) != request.invocation.channel_id.str()) {
    error = WagerMutationStatus::forbidden;
    return action;
  }
  if (query.column_text(4) != "active") {
    error = WagerMutationStatus::stale;
    return action;
  }
  if (query.column_int64(5) <= request.invocation.now_ms) {
    error = WagerMutationStatus::expired;
    return action;
  }
  if (query.column_int64(3) != query.column_int64(8)) {
    error = WagerMutationStatus::stale;
    return action;
  }
  error = WagerMutationStatus::applied;
  return action;
}

void mark_control_used(SqliteConnection &connection,
                       const std::optional<std::string> &token,
                       const std::int64_t now_ms) {
  if (!token)
    return;
  auto update =
      connection.prepare("UPDATE tarot_wager_control SET state = 'used', "
                         "used_at_ms = max(?, created_at_ms) "
                         "WHERE token_id = ? AND state = 'active'");
  update.bind(1, now_ms);
  update.bind(2, *token);
  update.execute();
}

[[nodiscard]] WagerAction action_from_name(const std::string_view action) {
  if (action == "confirm")
    return WagerAction::confirm;
  if (action == "discard")
    return WagerAction::discard;
  if (action == "accept")
    return WagerAction::accept;
  if (action == "decline")
    return WagerAction::decline;
  if (action == "cancel")
    return WagerAction::cancel;
  if (action == "dispute")
    return WagerAction::dispute;
  if (action == "agree")
    return WagerAction::agree;
  if (action == "void")
    return WagerAction::void_wager;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Wager control action is incompatible."};
}

[[nodiscard]] std::string action_name(const WagerAction action) {
  switch (action) {
  case WagerAction::confirm:
    return "confirm";
  case WagerAction::discard:
    return "discard";
  case WagerAction::accept:
    return "accept";
  case WagerAction::decline:
    return "decline";
  case WagerAction::cancel:
    return "cancel";
  case WagerAction::dispute:
    return "dispute";
  case WagerAction::agree:
    return "agree";
  case WagerAction::void_wager:
    return "void";
  }
  throw std::invalid_argument{"Wager action is invalid."};
}

} // namespace

SqliteWagerRepository::SqliteWagerRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite wager repository context is required."};
}

WagerMutationResult
SqliteWagerRepository::create_draft(const WagerCreateRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!request.invocation.user_id.is_set() ||
      !request.target_user_id.is_set() ||
      request.outcome_window_ms < 3'600'000 ||
      request.resolution_grace_ms < 3'600'000 ||
      request.draft_expires_at_ms <= request.invocation.now_ms ||
      (request.resolution_policy == WagerResolutionPolicy::designated) !=
          request.judge_user_id.has_value() ||
      (request.is_test &&
       (!(request.invocation.owner && request.invocation.test_mode) ||
        request.target_user_id != request.invocation.user_id ||
        (request.judge_user_id &&
         *request.judge_user_id != request.invocation.user_id))) ||
      (!request.is_test &&
       request.invocation.user_id == request.target_user_id))
    throw std::invalid_argument{"Wager draft request is invalid."};
  const auto fp = fingerprint(
      "create", request.target_user_id.str(),
      Json{{"judge", request.judge_user_id ? Json{request.judge_user_id->str()}
                                           : Json{nullptr}},
           {"visibility", visibility_name(request.visibility)},
           {"resolution", resolution_name(request.resolution_policy)},
           {"outcome_window_ms", request.outcome_window_ms},
           {"resolution_grace_ms", request.resolution_grace_ms},
           {"is_test", request.is_test}}
          .dump());
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "create", fp)) {
    transaction.commit();
    return *replay;
  }
  const auto wager_id = next_id(request.next_id);
  auto insert = context_->connection().prepare(
      "INSERT INTO tarot_wager "
      "(wager_id, state, revision, guild_id, channel_id, creator_user_id, "
      "target_user_id, judge_user_id, visibility, resolution_policy, "
      "outcome_window_ms, resolution_grace_ms, is_test, created_at_ms, "
      "updated_at_ms) "
      "VALUES (?, 'draft', 1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, wager_id);
  insert.bind(2, request.invocation.guild_id.str());
  insert.bind(3, request.invocation.channel_id.str());
  insert.bind(4, request.invocation.user_id.str());
  insert.bind(5, request.target_user_id.str());
  if (request.judge_user_id)
    insert.bind(6, request.judge_user_id->str());
  else
    insert.bind_null(6);
  insert.bind(7, visibility_name(request.visibility));
  insert.bind(8, resolution_name(request.resolution_policy));
  insert.bind(9, request.outcome_window_ms);
  insert.bind(10, request.resolution_grace_ms);
  insert.bind(11, request.is_test ? 1 : 0);
  insert.bind(12, request.invocation.now_ms);
  insert.bind(13, request.invocation.now_ms);
  insert.execute();
  auto wager = *load_wager(context_->connection(), wager_id);
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, event_id, "tarot.wager_drafted.v1",
                wager_id, "event:wager-drafted:" + wager_id,
                {{"visibility", visibility_name(request.visibility)},
                 {"resolution", resolution_name(request.resolution_policy)},
                 {"is_test", request.is_test}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                wager, WagerRole::creator, "drafted", event_id,
                "action:wager-drafted:" + wager_id);

  WagerMutationResult result{
      .status = WagerMutationStatus::applied,
      .wager = wager,
      .controls = {},
      .committed_event_types = {"tarot.wager_drafted.v1"},
      .public_delivery_created = false};
  const auto token = next_id(request.next_id);
  auto control = context_->connection().prepare(
      "INSERT INTO tarot_wager_control "
      "(token_id, wager_id, action, expected_user_id, simulated_role, "
      "expected_revision, state, expires_at_ms, idempotency_key, "
      "created_at_ms) "
      "VALUES (?, ?, 'open_form', ?, ?, 1, 'active', ?, ?, ?)");
  control.bind(1, token);
  control.bind(2, wager_id);
  control.bind(3, request.invocation.user_id.str());
  if (request.is_test)
    control.bind(4, "creator");
  else
    control.bind_null(4);
  control.bind(5, request.draft_expires_at_ms);
  control.bind(6, "wager:control:" + wager_id + ":open_form");
  control.bind(7, request.invocation.now_ms);
  control.execute();
  result.controls.push_back(
      {std::string{wager_form_prefix} + token, "Open wager form"});
  schedule_job(context_->connection(), request.next_id, request.invocation,
               wager, WagerDeadlinePhase::draft_expiry,
               request.draft_expires_at_ms);
  insert_receipt(context_->connection(), request.invocation, wager_id, "create",
                 fp, WagerMutationStatus::applied, {{"wager_id", wager_id}});
  transaction.commit();
  return result;
}

WagerMutationResult
SqliteWagerRepository::preview(const WagerPreviewRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!valid_uuid_v4(request.token_id) || blank(request.proposition) ||
      request.proposition.size() > 500 || request.stake < 1 ||
      request.stake > 100 || request.offer_expiry_ms <= 0 ||
      (request.evidence_instructions &&
       (blank(*request.evidence_instructions) ||
        request.evidence_instructions->size() > 500)))
    throw std::invalid_argument{"Wager preview request is invalid."};
  const auto fp = fingerprint(
      "preview", request.token_id,
      Json{{"proposition", request.proposition},
           {"stake", request.stake},
           {"evidence_instructions", request.evidence_instructions
                                         ? Json{*request.evidence_instructions}
                                         : Json{nullptr}},
           {"offer_expiry_ms", request.offer_expiry_ms}}
          .dump());
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "preview", fp)) {
    transaction.commit();
    return *replay;
  }
  auto token = context_->connection().prepare(
      "SELECT wager_id, expected_user_id, expected_revision, state, "
      "expires_at_ms "
      "FROM tarot_wager_control WHERE token_id = ? AND action = 'open_form'");
  token.bind(1, request.token_id);
  if (!token.step()) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "preview", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  const auto wager_id = token.column_text(0);
  auto wager = *load_wager(context_->connection(), wager_id);
  WagerMutationStatus denied = WagerMutationStatus::applied;
  if (!test_access_allowed(request.invocation, wager))
    denied = WagerMutationStatus::forbidden;
  else if (token.column_text(1) != request.invocation.user_id.str())
    denied = WagerMutationStatus::forbidden;
  else if (token.column_text(3) != "active" ||
           token.column_int64(2) != static_cast<std::int64_t>(wager.revision))
    denied = WagerMutationStatus::stale;
  else if (token.column_int64(4) <= request.invocation.now_ms)
    denied = WagerMutationStatus::expired;
  else if (wager.state != WagerState::draft)
    denied = WagerMutationStatus::invalid_state;
  if (denied != WagerMutationStatus::applied) {
    insert_receipt(context_->connection(), request.invocation, wager_id,
                   "preview", fp, denied);
    transaction.commit();
    return empty_mutation_result(denied, wager);
  }
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, event_id, "tarot.wager_previewed.v1",
                wager_id, "event:wager-previewed:" + wager_id,
                {{"revision", wager.revision + 1}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                wager, WagerRole::creator, "previewed", event_id,
                "action:wager-previewed:" + wager_id);
  auto update = context_->connection().prepare(
      "UPDATE tarot_wager SET proposition = ?, stake = ?, "
      "evidence_instructions = ?, "
      "offer_duration_ms = ?, revision = revision + 1, "
      "updated_at_ms = max(?, updated_at_ms) "
      "WHERE wager_id = ? AND state = 'draft' AND revision = ?");
  update.bind(1, request.proposition);
  update.bind(2, request.stake);
  if (request.evidence_instructions)
    update.bind(3, *request.evidence_instructions);
  else
    update.bind_null(3);
  update.bind(4, request.offer_expiry_ms);
  update.bind(5, request.invocation.now_ms);
  update.bind(6, wager_id);
  update.bind(7, static_cast<std::int64_t>(wager.revision));
  update.execute();
  if (context_->connection().changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Wager preview revision became stale."};
  mark_control_used(context_->connection(), request.token_id,
                    request.invocation.now_ms);
  wager = *load_wager(context_->connection(), wager_id);
  WagerMutationResult result{
      .status = WagerMutationStatus::applied,
      .wager = wager,
      .controls = {},
      .committed_event_types = {"tarot.wager_previewed.v1"},
      .public_delivery_created = false};
  const auto controls_expire_at =
      checked_add(wager.updated_at_ms, wager_control_lifetime_ms);
  insert_control(context_->connection(), request.next_id, wager, "confirm",
                 wager.creator_user_id, WagerRole::creator, controls_expire_at,
                 result.controls, "Confirm offer");
  insert_control(context_->connection(), request.next_id, wager, "discard",
                 wager.creator_user_id, WagerRole::creator, controls_expire_at,
                 result.controls, "Discard");
  insert_receipt(context_->connection(), request.invocation, wager_id,
                 "preview", fp, WagerMutationStatus::applied,
                 {{"wager_id", wager_id}});
  transaction.commit();
  return result;
}

namespace {

void enqueue_card_edit(SqliteConnection &connection, const WagerIdFactory &ids,
                       const WagerInvocation &call, const WagerRecord &wager) {
  auto source =
      connection.prepare("SELECT create_outbox_id FROM tarot_wager_public_card "
                         "WHERE wager_id = ?");
  source.bind(1, wager.wager_id);
  if (!source.step())
    return;
  auto replacement = public_card(wager, {});
  replacement.guild_id = call.guild_id;
  replacement.channel_id = call.channel_id;
  replacement.message.buttons.clear();
  if (wager.visibility == WagerVisibility::sealed &&
      wager.state != WagerState::offered) {
    replacement.message.content =
        wager.is_test ? "[TEST] Sealed Fate wager" : "Sealed Fate wager";
    replacement.message.allowed_user_mentions.clear();
  }
  const auto outbox_id = next_id(ids);
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{wager_public_edit_outbox_kind},
      .aggregate_type = "tarot_wager",
      .aggregate_id = wager.wager_id,
      .target_guild_id = call.guild_id,
      .target_channel_id = call.channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = call.now_ms + 1,
      .max_attempts = 20,
      .idempotency_key = "outbox:wager-edit:" + wager.wager_id + ":" +
                         std::to_string(wager.revision),
      .provider_nonce = nonce(outbox_id),
      .created_at_ms = call.now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_edit_payload(
          PublicEditOutboxPayload{.replacement = std::move(replacement),
                                  .source_outbox_id = source.column_text(0),
                                  .wager_revision = wager.revision},
          call.correlation_id, std::nullopt)));
  auto link = connection.prepare(
      "INSERT INTO tarot_wager_card_revision "
      "(wager_id, wager_revision, outbox_id) VALUES (?, ?, ?)");
  link.bind(1, wager.wager_id);
  link.bind(2, static_cast<std::int64_t>(wager.revision));
  link.bind(3, outbox_id);
  link.execute();
}

[[nodiscard]] WagerMutationResult
rejected_action(SqliteConnection &connection, const WagerActionRequest &request,
                const std::optional<WagerRecord> &wager,
                const WagerMutationStatus status, const std::string_view fp) {
  insert_receipt(connection, request.invocation,
                 wager ? std::optional<std::string>{wager->wager_id}
                       : std::nullopt,
                 "action", fp, status);
  return empty_mutation_result(status, wager);
}

[[nodiscard]] WagerMutationResult transition_unfunded_terminal(
    SqliteConnection &connection, const WagerActionRequest &request,
    WagerRecord wager, const WagerState terminal_state, const WagerRole role,
    const std::string_view action, const std::string_view event_type,
    const std::string_view fp) {
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, event_id, std::string{event_type},
                wager.wager_id,
                "event:" + std::string{action} + ":" + wager.wager_id,
                {{"is_test", wager.is_test}})));
  insert_action(connection, request.next_id, request.invocation, wager, role,
                action, event_id,
                "action:" + std::string{action} + ":" + wager.wager_id);
  auto update = connection.prepare(
      "UPDATE tarot_wager SET state = ?, revision = revision + 1, "
      "offer_expires_at_ms = COALESCE(offer_expires_at_ms, ?), "
      "updated_at_ms = max(?, updated_at_ms), terminal_at_ms = max(?, "
      "created_at_ms) "
      "WHERE wager_id = ? AND state = ? AND revision = ?");
  update.bind(1, state_name(terminal_state));
  update.bind(2, request.invocation.now_ms);
  update.bind(3, request.invocation.now_ms);
  update.bind(4, request.invocation.now_ms);
  update.bind(5, wager.wager_id);
  update.bind(6, state_name(wager.state));
  update.bind(7, static_cast<std::int64_t>(wager.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Wager terminal transition became stale."};
  wager = *load_wager(connection, wager.wager_id);
  cancel_controls(connection, wager.wager_id);
  cancel_sealed_offer_notice(connection, wager.wager_id);
  cancel_job(connection, wager.wager_id, "draft_expiry",
             request.invocation.now_ms);
  cancel_job(connection, wager.wager_id, "offer_expiry",
             request.invocation.now_ms);
  enqueue_card_edit(connection, request.next_id, request.invocation, wager);
  insert_receipt(connection, request.invocation, wager.wager_id, "action", fp,
                 WagerMutationStatus::applied);
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = {},
          .committed_event_types = {std::string{event_type}},
          .public_delivery_created = true};
}

[[nodiscard]] WagerMutationResult fund_wager(SqliteConnection &connection,
                                             const WagerActionRequest &request,
                                             WagerRecord wager,
                                             const std::string_view fp) {
  if (!wager.stake)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Offered wager has no stake."};
  const auto creator_existing =
      account_for_user(connection, wager.creator_user_id);
  const auto target_existing =
      account_for_user(connection, wager.target_user_id);
  const auto creator_balance =
      creator_existing.empty() ? request.starting_fate
                               : account_balance(connection, creator_existing);
  const auto target_balance =
      wager.creator_user_id == wager.target_user_id ? creator_balance
      : target_existing.empty()                     ? request.starting_fate
                                : account_balance(connection, target_existing);
  const auto required_creator = wager.creator_user_id == wager.target_user_id
                                    ? checked_twice(*wager.stake)
                                    : *wager.stake;
  if (creator_balance < required_creator || target_balance < *wager.stake) {
    insert_receipt(connection, request.invocation, wager.wager_id, "action", fp,
                   WagerMutationStatus::insufficient_funds);
    return empty_mutation_result(WagerMutationStatus::insufficient_funds,
                                 wager);
  }
  const auto creator_account =
      ensure_account(connection, request.invocation, wager.creator_user_id,
                     request.starting_fate, request.next_id);
  const auto target_account =
      wager.creator_user_id == wager.target_user_id
          ? creator_account
          : ensure_account(connection, request.invocation, wager.target_user_id,
                           request.starting_fate, request.next_id);
  const auto escrow = system_account(connection, "ESCROW");
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, event_id, "tarot.wager_funded.v1",
                wager.wager_id, "event:wager-funded:" + wager.wager_id,
                {{"is_test", wager.is_test}})));
  const auto transaction_id = next_id(request.next_id);
  const auto same_account = creator_account == target_account;
  insert_transaction(
      connection, transaction_id, "WAGER_ESCROW_FUND", same_account ? 2 : 3,
      event_id, "tx:wager-funded:" + wager.wager_id, request.invocation.user_id,
      std::nullopt, wager.is_test, std::nullopt, request.invocation.now_ms);
  insert_transfer_link(connection, request.next_id, wager, "fund",
                       transaction_id, request.invocation.now_ms);
  if (same_account) {
    insert_posting(connection, request.next_id, transaction_id, creator_account,
                   -checked_twice(*wager.stake), request.invocation.now_ms);
  } else {
    insert_posting(connection, request.next_id, transaction_id, creator_account,
                   -*wager.stake, request.invocation.now_ms);
    insert_posting(connection, request.next_id, transaction_id, target_account,
                   -*wager.stake, request.invocation.now_ms);
  }
  insert_posting(connection, request.next_id, transaction_id, escrow,
                 checked_twice(*wager.stake), request.invocation.now_ms);
  seal_transaction(connection, transaction_id, request.invocation.now_ms);
  insert_action(connection, request.next_id, request.invocation, wager,
                WagerRole::target, "accepted", event_id,
                "action:wager-accepted:" + wager.wager_id);

  const auto funded_at =
      std::max(request.invocation.now_ms, wager.updated_at_ms);
  const auto outcome_due = checked_add(funded_at, wager.outcome_window_ms);
  const auto grace = checked_add(outcome_due, wager.resolution_grace_ms);
  auto update = connection.prepare(
      "UPDATE tarot_wager SET state = 'accepted_funded', revision = revision + "
      "1, "
      "fund_transaction_id = ?, outcome_due_at_ms = ?, "
      "resolution_grace_until_ms = ?, updated_at_ms = ? "
      "WHERE wager_id = ? AND state = 'offered' AND revision = ?");
  update.bind(1, transaction_id);
  update.bind(2, outcome_due);
  update.bind(3, grace);
  update.bind(4, funded_at);
  update.bind(5, wager.wager_id);
  update.bind(6, static_cast<std::int64_t>(wager.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "Wager acceptance became stale."};
  wager = *load_wager(connection, wager.wager_id);
  cancel_controls(connection, wager.wager_id);
  cancel_job(connection, wager.wager_id, "offer_expiry",
             request.invocation.now_ms);
  const auto midpoint = checked_add(funded_at, wager.outcome_window_ms / 2);
  schedule_job(connection, request.next_id, request.invocation, wager,
               WagerDeadlinePhase::reminder, midpoint);
  schedule_job(connection, request.next_id, request.invocation, wager,
               WagerDeadlinePhase::outcome_due, outcome_due);
  schedule_job(connection, request.next_id, request.invocation, wager,
               WagerDeadlinePhase::grace, grace);
  std::vector<DiscordSnowflake> accepted_recipients{wager.creator_user_id,
                                                    wager.target_user_id};
  if (wager.judge_user_id)
    accepted_recipients.push_back(*wager.judge_user_id);
  std::string accepted_body =
      "Wager `" + wager.wager_id +
      "` is funded.\nProposition: " + *wager.proposition +
      "\nEqual stake: " + std::to_string(*wager.stake) +
      " Fate each.\nOutcome deadline: <t:" +
      std::to_string(outcome_due / 1'000) +
      ":F>.\nOwner escalation after: <t:" + std::to_string(grace / 1'000) +
      ":F>.";
  if (wager.evidence_instructions)
    accepted_body += "\nEvidence instructions: " + *wager.evidence_instructions;
  if (wager.resolution_policy == WagerResolutionPolicy::designated)
    accepted_body += "\nThe designated judge <@" + wager.judge_user_id->str() +
                     "> may now submit a reasoned result unless a participant "
                     "disputes first.";
  insert_lifecycle_notices(connection, request.next_id, request.invocation,
                           wager, "accepted", "Wager funded", accepted_body,
                           std::move(accepted_recipients), grace);
  std::vector<WagerMutationResult::Control> response_controls;
  if (wager.resolution_policy == WagerResolutionPolicy::mutual)
    insert_control(connection, request.next_id, wager, "outcome",
                   wager.target_user_id, WagerRole::target, grace,
                   response_controls, "Submit outcome", wager_outcome_prefix);
  insert_control(connection, request.next_id, wager, "evidence",
                 wager.target_user_id, WagerRole::target, grace,
                 response_controls, "Add private evidence",
                 wager_evidence_prefix);
  enqueue_card_edit(connection, request.next_id, request.invocation, wager);
  insert_receipt(connection, request.invocation, wager.wager_id, "action", fp,
                 WagerMutationStatus::applied);
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = std::move(response_controls),
          .committed_event_types = {"tarot.wager_funded.v1"},
          .public_delivery_created = true};
}

[[nodiscard]] WagerMutationResult
settle_wager(SqliteConnection &connection, const WagerIdFactory &ids,
             const WagerInvocation &call, WagerRecord wager,
             const std::optional<WagerRole> winner,
             const std::string_view authority,
             const WagerRole settlement_actor_role,
             const std::optional<std::string> &reason,
             const std::string_view receipt_operation,
             const std::string_view receipt_fingerprint) {
  if (!wager.stake || !funded_open(wager.state))
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Wager cannot be settled from this state."};
  const bool is_void = !winner.has_value();
  const auto event_type =
      is_void ? "tarot.wager_voided.v1" : "tarot.wager_resolved.v1";
  const auto event_id = next_id(ids);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(call, event_id, event_type, wager.wager_id,
                "event:" + std::string{event_type} + ":" + wager.wager_id,
                {{"authority", authority}, {"is_test", wager.is_test}})));
  insert_action(connection, ids, call, wager, settlement_actor_role,
                authority == "mutual" ? "agreed" : "judged", event_id,
                "action:wager-settled:" + wager.wager_id, reason);
  const auto transaction_id = next_id(ids);
  const auto creator_account =
      account_for_user(connection, wager.creator_user_id);
  const auto target_account =
      account_for_user(connection, wager.target_user_id);
  const auto same_account = creator_account == target_account;
  const auto posting_count = is_void && !same_account ? 3U : 2U;
  insert_transaction(
      connection, transaction_id, is_void ? "WAGER_REFUND" : "WAGER_PAYOUT",
      posting_count, event_id,
      std::string{"tx:"} + (is_void ? "wager-refund:" : "wager-payout:") +
          wager.wager_id,
      call.user_id, reason, wager.is_test, std::nullopt, call.now_ms);
  insert_transfer_link(connection, ids, wager, is_void ? "refund" : "payout",
                       transaction_id, call.now_ms);
  insert_posting(connection, ids, transaction_id,
                 system_account(connection, "ESCROW"),
                 -checked_twice(*wager.stake), call.now_ms);
  if (is_void) {
    if (same_account) {
      insert_posting(connection, ids, transaction_id, creator_account,
                     checked_twice(*wager.stake), call.now_ms);
    } else {
      insert_posting(connection, ids, transaction_id, creator_account,
                     *wager.stake, call.now_ms);
      insert_posting(connection, ids, transaction_id, target_account,
                     *wager.stake, call.now_ms);
    }
  } else {
    const auto winner_account =
        *winner == WagerRole::creator ? creator_account : target_account;
    insert_posting(connection, ids, transaction_id, winner_account,
                   checked_twice(*wager.stake), call.now_ms);
  }
  seal_transaction(connection, transaction_id, call.now_ms);

  const auto resolution_id = next_id(ids);
  auto resolution = connection.prepare(
      "INSERT INTO tarot_wager_resolution "
      "(resolution_id, wager_id, result, authority, actor_user_id, reason, "
      "transaction_id, event_id, created_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, "
      "?, ?)");
  resolution.bind(1, resolution_id);
  resolution.bind(2, wager.wager_id);
  resolution.bind(3, is_void ? "void" : wager_role_name(*winner));
  resolution.bind(4, authority);
  if (authority == "mutual")
    resolution.bind_null(5);
  else
    resolution.bind(5, call.user_id.str());
  if (reason)
    resolution.bind(6, *reason);
  else
    resolution.bind_null(6);
  resolution.bind(7, transaction_id);
  resolution.bind(8, event_id);
  resolution.bind(9, call.now_ms);
  resolution.execute();

  auto update =
      connection.prepare("UPDATE tarot_wager SET state = ?, revision = "
                         "revision + 1, winner_role = ?, "
                         "terminal_reason = ?, judged_by_user_id = ?, "
                         "settlement_transaction_id = ?, "
                         "updated_at_ms = max(?, updated_at_ms), "
                         "terminal_at_ms = max(?, created_at_ms) "
                         "WHERE wager_id = ? AND state IN ('accepted_funded', "
                         "'awaiting_resolution', 'disputed') "
                         "AND revision = ?");
  update.bind(1, is_void ? "void_refunded" : "resolved");
  if (winner)
    update.bind(2, wager_role_name(*winner));
  else
    update.bind_null(2);
  if (reason)
    update.bind(3, *reason);
  else
    update.bind_null(3);
  if (authority == "mutual")
    update.bind_null(4);
  else
    update.bind(4, call.user_id.str());
  update.bind(5, transaction_id);
  update.bind(6, call.now_ms);
  update.bind(7, call.now_ms);
  update.bind(8, wager.wager_id);
  update.bind(9, static_cast<std::int64_t>(wager.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "Wager settlement became stale."};
  wager = *load_wager(connection, wager.wager_id);
  const auto winner_text =
      winner
          ? std::string{*winner == WagerRole::creator ? "Creator" : "Target"} +
                " received the complete escrow."
          : "Both original equal stakes were refunded.";
  insert_lifecycle_notices(
      connection, ids, call, wager, "resolved", "Wager closed",
      "Wager `" + wager.wager_id + "` is closed. " + winner_text,
      {wager.creator_user_id, wager.target_user_id},
      checked_add(call.now_ms, 30LL * 24 * 60 * 60 * 1'000));
  cancel_controls(connection, wager.wager_id);
  cancel_job(connection, wager.wager_id, "reminder", call.now_ms);
  cancel_job(connection, wager.wager_id, "outcome_due", call.now_ms);
  cancel_job(connection, wager.wager_id, "grace", call.now_ms);
  cancel_resolution_reminders(connection, wager.wager_id, call.now_ms);
  enqueue_card_edit(connection, ids, call, wager);
  insert_receipt(connection, call, wager.wager_id, receipt_operation,
                 receipt_fingerprint, WagerMutationStatus::applied);
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = {},
          .committed_event_types = {event_type},
          .public_delivery_created = true};
}

} // namespace

WagerMutationResult
SqliteWagerRepository::act(const WagerActionRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (request.starting_fate < 1 || request.offer_expiry_ms <= 0 ||
      request.resolution_grace_ms <= 0 ||
      (!request.token_id && !valid_uuid_v4(request.wager_id)))
    throw std::invalid_argument{"Wager action request is invalid."};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  std::string wager_id = request.wager_id;
  WagerMutationStatus control_error = WagerMutationStatus::applied;
  const auto stored_action =
      control_action(context_->connection(), request, wager_id, control_error);
  const auto effective_action =
      stored_action ? action_from_name(*stored_action) : request.action;
  const auto fp = fingerprint("action", wager_id,
                              action_name(effective_action) + ":" +
                                  request.token_id.value_or("slash"));
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "action", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), wager_id);
  if (!wager) {
    auto result = rejected_action(context_->connection(), request, std::nullopt,
                                  control_error == WagerMutationStatus::applied
                                      ? WagerMutationStatus::not_found
                                      : control_error,
                                  fp);
    transaction.commit();
    return result;
  }
  if (control_error != WagerMutationStatus::applied &&
      control_error != WagerMutationStatus::expired) {
    auto result = rejected_action(context_->connection(), request, wager,
                                  control_error, fp);
    transaction.commit();
    return result;
  }
  const auto role = role_for(context_->connection(), request.invocation, *wager,
                             request.token_id);
  if (wager->state == WagerState::draft) {
    const auto draft_expiry = deadline_due_at(context_->connection(),
                                              wager->wager_id, "draft_expiry");
    if (draft_expiry && request.invocation.now_ms >= *draft_expiry) {
      if (role != WagerRole::creator) {
        auto result = rejected_action(context_->connection(), request, wager,
                                      WagerMutationStatus::forbidden, fp);
        transaction.commit();
        return result;
      }
      auto result = transition_unfunded_terminal(
          context_->connection(), request, *wager, WagerState::expired, role,
          "expired", "tarot.wager_expired.v1", fp);
      result.status = WagerMutationStatus::expired;
      transaction.commit();
      return result;
    }
  }
  if (wager->state == WagerState::offered && wager->offer_expires_at_ms &&
      request.invocation.now_ms >= *wager->offer_expires_at_ms) {
    if (!participant(role)) {
      auto result = rejected_action(context_->connection(), request, wager,
                                    WagerMutationStatus::forbidden, fp);
      transaction.commit();
      return result;
    }
    auto result = transition_unfunded_terminal(
        context_->connection(), request, *wager, WagerState::expired, role,
        "expired", "tarot.wager_expired.v1", fp);
    result.status = WagerMutationStatus::expired;
    transaction.commit();
    return result;
  }
  if (control_error == WagerMutationStatus::expired) {
    auto result = rejected_action(context_->connection(), request, wager,
                                  control_error, fp);
    transaction.commit();
    return result;
  }

  WagerMutationResult result;
  switch (effective_action) {
  case WagerAction::confirm: {
    if (role != WagerRole::creator) {
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
      break;
    }
    if (wager->state != WagerState::draft || !wager->stake ||
        !wager->proposition || !wager->offer_duration_ms) {
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
      break;
    }
    const auto confirmed_at =
        std::max(request.invocation.now_ms, wager->updated_at_ms);
    const auto expiry = checked_add(confirmed_at, *wager->offer_duration_ms);
    const auto event_id = next_id(request.next_id);
    static_cast<void>(detail::insert_event_uncommitted(
        context_->connection(),
        event_for(request.invocation, event_id, "tarot.wager_offered.v1",
                  wager->wager_id, "event:wager-offered:" + wager->wager_id,
                  {{"visibility", visibility_name(wager->visibility)},
                   {"is_test", wager->is_test}})));
    insert_action(context_->connection(), request.next_id, request.invocation,
                  *wager, WagerRole::creator, "confirmed", event_id,
                  "action:wager-confirmed:" + wager->wager_id);
    auto update = context_->connection().prepare(
        "UPDATE tarot_wager SET state = 'offered', revision = revision + 1, "
        "offer_expires_at_ms = ?, updated_at_ms = ? "
        "WHERE wager_id = ? AND state = 'draft' AND revision = ?");
    update.bind(1, expiry);
    update.bind(2, confirmed_at);
    update.bind(3, wager->wager_id);
    update.bind(4, static_cast<std::int64_t>(wager->revision));
    update.execute();
    if (context_->connection().changes() != 1)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Wager confirmation became stale."};
    mark_control_used(context_->connection(), request.token_id,
                      request.invocation.now_ms);
    cancel_controls(context_->connection(), wager->wager_id);
    cancel_job(context_->connection(), wager->wager_id, "draft_expiry",
               request.invocation.now_ms);
    wager = load_wager(context_->connection(), wager->wager_id);
    std::vector<WagerMutationResult::Control> target_controls;
    std::vector<WagerMutationResult::Control> creator_controls;
    insert_control(context_->connection(), request.next_id, *wager, "accept",
                   wager->target_user_id, WagerRole::target, expiry,
                   target_controls, "Accept and fund");
    insert_control(context_->connection(), request.next_id, *wager, "decline",
                   wager->target_user_id, WagerRole::target, expiry,
                   target_controls, "Decline");
    insert_control(context_->connection(), request.next_id, *wager, "cancel",
                   wager->creator_user_id, WagerRole::creator, expiry,
                   creator_controls, "Cancel");
    std::optional<std::string> sealed_open;
    if (wager->visibility == WagerVisibility::sealed) {
      std::string body =
          "Reference: `" + wager->wager_id + "`\nCreator: <@" +
          wager->creator_user_id.str() +
          ">\nProposition: " + *wager->proposition +
          "\nEqual stake: " + std::to_string(*wager->stake) +
          " Fate each.\nAccepting funds both sides atomically." +
          "\nOffer deadline: <t:" + std::to_string(expiry / 1'000) + ":F>." +
          "\nOutcome window: " +
          std::to_string(wager->outcome_window_ms / 3'600'000) +
          " hours after acceptance." + "\nOwner escalation: " +
          std::to_string(wager->resolution_grace_ms / 3'600'000) +
          " hours after the outcome deadline.";
      if (wager->resolution_policy == WagerResolutionPolicy::designated)
        body += "\nResolution: designated judge <@" +
                wager->judge_user_id->str() + "> before any dispute.";
      else
        body += "\nResolution: matching participant submissions.";
      if (wager->evidence_instructions)
        body += "\nEvidence: " + *wager->evidence_instructions;
      insert_pending_notice(context_->connection(), request.next_id,
                            request.invocation, *wager, wager->target_user_id,
                            "sealed_offer", "A sealed Fate wager", body,
                            target_controls, expiry, &sealed_open);
    }
    auto public_controls = target_controls;
    public_controls.insert(public_controls.end(), creator_controls.begin(),
                           creator_controls.end());
    enqueue_initial_card(context_->connection(), request.next_id,
                         request.invocation, *wager, std::move(public_controls),
                         sealed_open);
    schedule_job(context_->connection(), request.next_id, request.invocation,
                 *wager, WagerDeadlinePhase::offer_expiry, expiry);
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "action", fp, WagerMutationStatus::applied);
    result = {.status = WagerMutationStatus::applied,
              .wager = wager,
              .controls = std::move(creator_controls),
              .committed_event_types = {"tarot.wager_offered.v1"},
              .public_delivery_created = true};
    break;
  }
  case WagerAction::discard:
    if (role != WagerRole::creator)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (wager->state != WagerState::draft)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else
      result = transition_unfunded_terminal(
          context_->connection(), request, *wager, WagerState::cancelled, role,
          "discarded", "tarot.wager_cancelled.v1", fp);
    break;
  case WagerAction::accept:
    if (role != WagerRole::target)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (wager->state != WagerState::offered)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else if (!sealed_offer_delivered(context_->connection(), *wager))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else
      result = fund_wager(context_->connection(), request, *wager, fp);
    break;
  case WagerAction::decline:
    if (role != WagerRole::target)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (wager->state != WagerState::offered)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else if (!sealed_offer_delivered(context_->connection(), *wager))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else
      result = transition_unfunded_terminal(
          context_->connection(), request, *wager, WagerState::declined, role,
          "declined", "tarot.wager_declined.v1", fp);
    break;
  case WagerAction::cancel:
    if (role != WagerRole::creator)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (wager->state != WagerState::offered)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else
      result = transition_unfunded_terminal(
          context_->connection(), request, *wager, WagerState::cancelled, role,
          "cancelled", "tarot.wager_cancelled.v1", fp);
    break;
  case WagerAction::dispute: {
    if (!participant(role))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (!funded_open(wager->state))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else if (wager->state == WagerState::disputed) {
      insert_receipt(context_->connection(), request.invocation,
                     wager->wager_id, "action", fp,
                     WagerMutationStatus::unchanged);
      result = empty_mutation_result(WagerMutationStatus::unchanged, wager);
    } else {
      const auto event_id = next_id(request.next_id);
      static_cast<void>(detail::insert_event_uncommitted(
          context_->connection(),
          event_for(request.invocation, event_id, "tarot.wager_disputed.v1",
                    wager->wager_id, "event:wager-disputed:" + wager->wager_id,
                    {{"is_test", wager->is_test}})));
      insert_action(context_->connection(), request.next_id, request.invocation,
                    *wager, role, "disputed", event_id,
                    "action:wager-disputed:" + wager->wager_id);
      auto update = context_->connection().prepare(
          "UPDATE tarot_wager SET state = 'disputed', revision = revision + 1, "
          "updated_at_ms = max(?, updated_at_ms) WHERE wager_id = ? AND "
          "revision = ? "
          "AND state IN ('accepted_funded', 'awaiting_resolution')");
      update.bind(1, request.invocation.now_ms);
      update.bind(2, wager->wager_id);
      update.bind(3, static_cast<std::int64_t>(wager->revision));
      update.execute();
      wager = load_wager(context_->connection(), wager->wager_id);
      insert_dispute_notices(context_->connection(), request.next_id,
                             request.invocation, *wager);
      cancel_controls(context_->connection(), wager->wager_id);
      cancel_job(context_->connection(), wager->wager_id, "reminder",
                 request.invocation.now_ms);
      cancel_job(context_->connection(), wager->wager_id, "outcome_due",
                 request.invocation.now_ms);
      cancel_job(context_->connection(), wager->wager_id, "grace",
                 request.invocation.now_ms);
      cancel_resolution_reminders(context_->connection(), wager->wager_id,
                                  request.invocation.now_ms);
      enqueue_card_edit(context_->connection(), request.next_id,
                        request.invocation, *wager);
      insert_receipt(context_->connection(), request.invocation,
                     wager->wager_id, "action", fp,
                     WagerMutationStatus::applied);
      result = {.status = WagerMutationStatus::applied,
                .wager = wager,
                .controls = {},
                .committed_event_types = {"tarot.wager_disputed.v1"},
                .public_delivery_created = true};
    }
    break;
  }
  case WagerAction::void_wager: {
    if (!participant(role))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (!funded_open(wager->state))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else {
      bool consent_created = false;
      auto existing = context_->connection().prepare(
          "SELECT 1 FROM tarot_wager_void_consent WHERE wager_id = ? AND "
          "actor_role = ?");
      existing.bind(1, wager->wager_id);
      existing.bind(2, wager_role_name(role));
      if (!existing.step()) {
        auto consent = context_->connection().prepare(
            "INSERT INTO tarot_wager_void_consent "
            "(consent_id, wager_id, actor_user_id, actor_role, "
            "idempotency_key, created_at_ms) "
            "VALUES (?, ?, ?, ?, ?, ?)");
        consent.bind(1, next_id(request.next_id));
        consent.bind(2, wager->wager_id);
        consent.bind(3, request.invocation.user_id.str());
        consent.bind(4, wager_role_name(role));
        consent.bind(5, request.invocation.interaction_idempotency_key);
        consent.bind(6, request.invocation.now_ms);
        consent.execute();
        const auto consent_event_id = next_id(request.next_id);
        static_cast<void>(detail::insert_event_uncommitted(
            context_->connection(),
            event_for(request.invocation, consent_event_id,
                      "tarot.wager_void_consent.v1", wager->wager_id,
                      "event:wager-void-consent:" + wager->wager_id + ":" +
                          wager_role_name(role),
                      {{"actor_role", wager_role_name(role)}})));
        insert_action(context_->connection(), request.next_id,
                      request.invocation, *wager, role, "void_consented",
                      consent_event_id,
                      "action:void-consent:" + wager->wager_id + ":" +
                          wager_role_name(role));
        consent_created = true;
      }
      auto count = context_->connection().prepare(
          "SELECT count(*) FROM tarot_wager_void_consent WHERE wager_id = ?");
      count.bind(1, wager->wager_id);
      static_cast<void>(count.step());
      if (count.column_int64(0) >= 2) {
        result = settle_wager(context_->connection(), request.next_id,
                              request.invocation, *wager, std::nullopt,
                              "mutual", role, std::nullopt, "action", fp);
        if (consent_created)
          result.committed_event_types.insert(
              result.committed_event_types.begin(),
              "tarot.wager_void_consent.v1");
      } else {
        insert_receipt(context_->connection(), request.invocation,
                       wager->wager_id, "action", fp,
                       consent_created ? WagerMutationStatus::applied
                                       : WagerMutationStatus::unchanged);
        result = {
            .status = consent_created ? WagerMutationStatus::applied
                                      : WagerMutationStatus::unchanged,
            .wager = wager,
            .controls = {},
            .committed_event_types =
                consent_created
                    ? std::vector<std::string>{"tarot.wager_void_consent.v1"}
                    : std::vector<std::string>{},
            .public_delivery_created = false};
      }
    }
    break;
  }
  case WagerAction::agree: {
    if (!participant(role))
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::forbidden, fp);
    else if (wager->state != WagerState::disputed)
      result = rejected_action(context_->connection(), request, wager,
                               WagerMutationStatus::invalid_state, fp);
    else {
      const auto other_role = role == WagerRole::creator ? "target" : "creator";
      auto other = context_->connection().prepare(
          "SELECT winner_role FROM tarot_wager_outcome WHERE wager_id = ? AND "
          "actor_role = ?");
      other.bind(1, wager->wager_id);
      other.bind(2, other_role);
      if (!other.step())
        result = rejected_action(context_->connection(), request, wager,
                                 WagerMutationStatus::invalid_state, fp);
      else
        result = settle_wager(context_->connection(), request.next_id,
                              request.invocation, *wager,
                              other.column_text(0) == "creator"
                                  ? std::optional<WagerRole>{WagerRole::creator}
                                  : std::optional<WagerRole>{WagerRole::target},
                              "mutual", role, std::nullopt, "action", fp);
    }
    break;
  }
  }
  transaction.commit();
  return result;
}

WagerMutationResult
SqliteWagerRepository::submit_outcome(const WagerOutcomeRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if ((request.token_id ? !valid_uuid_v4(*request.token_id)
                        : !valid_uuid_v4(request.wager_id)) ||
      (request.winner != WagerRole::creator &&
       request.winner != WagerRole::target))
    throw std::invalid_argument{"Wager outcome request is invalid."};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  std::string wager_id = request.wager_id;
  WagerMutationStatus token_status = WagerMutationStatus::applied;
  if (request.token_id) {
    auto token = context_->connection().prepare(
        "SELECT control.wager_id, control.expected_user_id, "
        "control.expected_revision, control.state, control.expires_at_ms, "
        "wager.revision, wager.guild_id, wager.channel_id "
        "FROM tarot_wager_control control JOIN tarot_wager wager ON "
        "wager.wager_id = control.wager_id "
        "WHERE control.token_id = ? AND control.action = 'outcome'");
    token.bind(1, *request.token_id);
    if (!token.step())
      token_status = WagerMutationStatus::not_found;
    else {
      wager_id = token.column_text(0);
      if (token.column_text(1) != request.invocation.user_id.str() ||
          token.column_text(6) != request.invocation.guild_id.str() ||
          token.column_text(7) != request.invocation.channel_id.str())
        token_status = WagerMutationStatus::forbidden;
      else if (token.column_text(3) != "active" ||
               token.column_int64(2) != token.column_int64(5))
        token_status = WagerMutationStatus::stale;
      else if (token.column_int64(4) <= request.invocation.now_ms)
        token_status = WagerMutationStatus::expired;
    }
  }
  const auto fp = fingerprint("outcome", wager_id,
                              std::string{wager_role_name(request.winner)} +
                                  ":" + request.token_id.value_or("slash"));
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "outcome", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), wager_id);
  if (token_status != WagerMutationStatus::applied) {
    insert_receipt(context_->connection(), request.invocation,
                   wager ? std::optional<std::string>{wager->wager_id}
                         : std::nullopt,
                   "outcome", fp, token_status);
    transaction.commit();
    return empty_mutation_result(token_status, wager);
  }
  if (!wager) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "outcome", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  if (!test_access_allowed(request.invocation, *wager)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden);
  }
  const auto role = role_for(context_->connection(), request.invocation, *wager,
                             request.token_id);
  if (!participant(role)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden, wager);
  }
  if (wager->resolution_policy == WagerResolutionPolicy::designated &&
      wager->state != WagerState::disputed) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::invalid_state);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::invalid_state, wager);
  }
  if (!funded_open(wager->state)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::invalid_state);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::invalid_state, wager);
  }
  auto prior = context_->connection().prepare(
      "SELECT winner_role FROM tarot_wager_outcome WHERE wager_id = ? AND "
      "actor_role = ?");
  prior.bind(1, wager->wager_id);
  prior.bind(2, wager_role_name(role));
  if (prior.step()) {
    const auto status = prior.column_text(0) == wager_role_name(request.winner)
                            ? WagerMutationStatus::unchanged
                            : WagerMutationStatus::invalid_state;
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, status);
    transaction.commit();
    return empty_mutation_result(status, wager);
  }

  auto insert = context_->connection().prepare(
      "INSERT INTO tarot_wager_outcome "
      "(submission_id, wager_id, actor_user_id, actor_role, winner_role, "
      "idempotency_key, created_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, next_id(request.next_id));
  insert.bind(2, wager->wager_id);
  insert.bind(3, request.invocation.user_id.str());
  insert.bind(4, wager_role_name(role));
  insert.bind(5, wager_role_name(request.winner));
  insert.bind(6, request.invocation.interaction_idempotency_key);
  insert.bind(7, request.invocation.now_ms);
  insert.execute();
  mark_control_used(context_->connection(), request.token_id,
                    request.invocation.now_ms);
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, event_id,
                "tarot.wager_outcome_submitted.v1", wager->wager_id,
                "event:wager-outcome:" + wager->wager_id + ":" +
                    wager_role_name(role),
                {{"actor_role", wager_role_name(role)}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                *wager, role, "outcome_submitted", event_id,
                "action:wager-outcome:" + wager->wager_id + ":" +
                    wager_role_name(role));

  bool transitioned_to_awaiting{};
  if (wager->state == WagerState::accepted_funded) {
    auto update = context_->connection().prepare(
        "UPDATE tarot_wager SET state = 'awaiting_resolution', revision = "
        "revision + 1, "
        "updated_at_ms = max(?, updated_at_ms) WHERE wager_id = ? "
        "AND state = 'accepted_funded' AND revision = ?");
    update.bind(1, request.invocation.now_ms);
    update.bind(2, wager->wager_id);
    update.bind(3, static_cast<std::int64_t>(wager->revision));
    update.execute();
    wager = load_wager(context_->connection(), wager->wager_id);
    transitioned_to_awaiting = true;
  }

  auto outcomes = context_->connection().prepare(
      "SELECT count(*), min(winner_role), max(winner_role) "
      "FROM tarot_wager_outcome WHERE wager_id = ?");
  outcomes.bind(1, wager->wager_id);
  static_cast<void>(outcomes.step());
  WagerMutationResult result;
  if (outcomes.column_int64(0) < 2) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::applied);
    if (transitioned_to_awaiting)
      enqueue_card_edit(context_->connection(), request.next_id,
                        request.invocation, *wager);
    result = {.status = WagerMutationStatus::applied,
              .wager = wager,
              .controls = {},
              .committed_event_types = {"tarot.wager_outcome_submitted.v1"},
              .public_delivery_created = transitioned_to_awaiting};
  } else if (outcomes.column_text(1) == outcomes.column_text(2)) {
    const auto winner = outcomes.column_text(1) == "creator"
                            ? WagerRole::creator
                            : WagerRole::target;
    result = settle_wager(context_->connection(), request.next_id,
                          request.invocation, *wager, winner, "mutual", role,
                          std::nullopt, "outcome", fp);
    result.committed_event_types.insert(result.committed_event_types.begin(),
                                        "tarot.wager_outcome_submitted.v1");
  } else if (wager->state != WagerState::disputed) {
    const auto dispute_event = next_id(request.next_id);
    static_cast<void>(detail::insert_event_uncommitted(
        context_->connection(),
        event_for(request.invocation, dispute_event, "tarot.wager_disputed.v1",
                  wager->wager_id, "event:wager-conflict:" + wager->wager_id,
                  {{"reason", "conflicting_outcomes"}})));
    insert_action(context_->connection(), request.next_id, request.invocation,
                  *wager, role, "disputed", dispute_event,
                  "action:wager-conflict:" + wager->wager_id);
    auto update = context_->connection().prepare(
        "UPDATE tarot_wager SET state = 'disputed', revision = revision + 1, "
        "updated_at_ms = max(?, updated_at_ms) WHERE wager_id = ? "
        "AND state = 'awaiting_resolution' AND revision = ?");
    update.bind(1, request.invocation.now_ms);
    update.bind(2, wager->wager_id);
    update.bind(3, static_cast<std::int64_t>(wager->revision));
    update.execute();
    wager = load_wager(context_->connection(), wager->wager_id);
    insert_dispute_notices(context_->connection(), request.next_id,
                           request.invocation, *wager);
    cancel_job(context_->connection(), wager->wager_id, "reminder",
               request.invocation.now_ms);
    cancel_job(context_->connection(), wager->wager_id, "outcome_due",
               request.invocation.now_ms);
    cancel_job(context_->connection(), wager->wager_id, "grace",
               request.invocation.now_ms);
    cancel_resolution_reminders(context_->connection(), wager->wager_id,
                                request.invocation.now_ms);
    enqueue_card_edit(context_->connection(), request.next_id,
                      request.invocation, *wager);
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::applied);
    result = {.status = WagerMutationStatus::applied,
              .wager = wager,
              .controls = {},
              .committed_event_types = {"tarot.wager_outcome_submitted.v1",
                                        "tarot.wager_disputed.v1"},
              .public_delivery_created = true};
  } else {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "outcome", fp, WagerMutationStatus::applied);
    result = {.status = WagerMutationStatus::applied,
              .wager = wager,
              .controls = {},
              .committed_event_types = {"tarot.wager_outcome_submitted.v1"},
              .public_delivery_created = false};
  }
  transaction.commit();
  return result;
}

WagerMutationResult
SqliteWagerRepository::add_evidence(const WagerEvidenceRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (blank(request.body) || request.body.size() > 1000 ||
      (!request.token_id && !valid_uuid_v4(request.wager_id)))
    throw std::invalid_argument{"Wager evidence request is invalid."};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  std::string wager_id = request.wager_id;
  WagerMutationStatus token_status = WagerMutationStatus::applied;
  if (request.token_id) {
    auto token = context_->connection().prepare(
        "SELECT control.wager_id, control.expected_user_id, "
        "control.expected_revision, "
        "control.state, control.expires_at_ms, wager.revision, wager.guild_id, "
        "wager.channel_id "
        "FROM tarot_wager_control control JOIN tarot_wager wager ON "
        "wager.wager_id = control.wager_id "
        "WHERE control.token_id = ? AND control.action = 'evidence'");
    token.bind(1, *request.token_id);
    if (!token.step())
      token_status = WagerMutationStatus::not_found;
    else {
      wager_id = token.column_text(0);
      if (token.column_text(1) != request.invocation.user_id.str() ||
          token.column_text(6) != request.invocation.guild_id.str() ||
          token.column_text(7) != request.invocation.channel_id.str())
        token_status = WagerMutationStatus::forbidden;
      else if (token.column_text(3) != "active" ||
               token.column_int64(2) != token.column_int64(5))
        token_status = WagerMutationStatus::stale;
      else if (token.column_int64(4) <= request.invocation.now_ms)
        token_status = WagerMutationStatus::expired;
    }
  }
  const auto fp = fingerprint("evidence", wager_id, request.body);
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "evidence", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), wager_id);
  if (token_status != WagerMutationStatus::applied || !wager) {
    const auto status = token_status == WagerMutationStatus::applied
                            ? WagerMutationStatus::not_found
                            : token_status;
    insert_receipt(context_->connection(), request.invocation,
                   wager ? std::optional<std::string>{wager->wager_id}
                         : std::nullopt,
                   "evidence", fp, status);
    transaction.commit();
    return empty_mutation_result(status, wager);
  }
  const auto role = role_for(context_->connection(), request.invocation, *wager,
                             request.token_id);
  if (!participant(role) || !funded_open(wager->state)) {
    const auto status = participant(role) ? WagerMutationStatus::invalid_state
                                          : WagerMutationStatus::forbidden;
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "evidence", fp, status);
    transaction.commit();
    return empty_mutation_result(status, wager);
  }
  auto insert = context_->connection().prepare(
      "INSERT INTO tarot_wager_evidence "
      "(evidence_id, wager_id, actor_user_id, actor_role, body, "
      "idempotency_key, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, next_id(request.next_id));
  insert.bind(2, wager->wager_id);
  insert.bind(3, request.invocation.user_id.str());
  insert.bind(4, wager_role_name(role));
  insert.bind(5, request.body);
  insert.bind(6, request.invocation.interaction_idempotency_key);
  insert.bind(7, request.invocation.now_ms);
  insert.execute();
  mark_control_used(context_->connection(), request.token_id,
                    request.invocation.now_ms);
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, event_id, "tarot.wager_evidence_added.v1",
                wager->wager_id,
                "event:wager-evidence:" +
                    request.invocation.interaction_idempotency_key,
                {{"actor_role", wager_role_name(role)}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                *wager, role, "evidence_added", event_id,
                "action:wager-evidence:" +
                    request.invocation.interaction_idempotency_key);
  insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                 "evidence", fp, WagerMutationStatus::applied);
  transaction.commit();
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = {},
          .committed_event_types = {"tarot.wager_evidence_added.v1"},
          .public_delivery_created = false};
}

WagerMutationResult
SqliteWagerRepository::judge(const WagerJudgmentRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!valid_uuid_v4(request.wager_id) || blank(request.reason) ||
      request.reason.size() > 200)
    throw std::invalid_argument{"Wager judgment request is invalid."};
  const auto judgment = request.judgment == WagerJudgment::creator  ? "creator"
                        : request.judgment == WagerJudgment::target ? "target"
                                                                    : "void";
  const auto fp = fingerprint("judgment", request.wager_id,
                              std::string{judgment} + ":" + request.reason);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "judgment", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), request.wager_id);
  if (!wager) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "judgment", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  if (!test_access_allowed(request.invocation, *wager)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "judgment", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden);
  }
  const auto role = role_for(context_->connection(), request.invocation, *wager,
                             std::nullopt);
  const bool judge_authorized =
      role == WagerRole::judge &&
      wager->resolution_policy == WagerResolutionPolicy::designated &&
      wager->resolution_grace_until_ms &&
      request.invocation.now_ms < *wager->resolution_grace_until_ms &&
      (wager->state == WagerState::accepted_funded ||
       wager->state == WagerState::awaiting_resolution);
  const bool owner_authorized =
      request.invocation.owner && wager->state == WagerState::disputed;
  if (!judge_authorized && !owner_authorized) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "judgment", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden);
  }
  const std::optional<WagerRole> winner =
      request.judgment == WagerJudgment::creator
          ? std::optional<WagerRole>{WagerRole::creator}
      : request.judgment == WagerJudgment::target
          ? std::optional<WagerRole>{WagerRole::target}
          : std::nullopt;
  auto result =
      settle_wager(context_->connection(), request.next_id, request.invocation,
                   *wager, winner, judge_authorized ? "judge" : "owner",
                   judge_authorized ? WagerRole::judge : WagerRole::owner,
                   request.reason, "judgment", fp);
  transaction.commit();
  return result;
}

namespace {

[[nodiscard]] bool can_view_evidence(const WagerInvocation &call,
                                     const WagerRecord &wager,
                                     const WagerRole role) {
  if (!test_access_allowed(call, wager))
    return false;
  if (participant(role))
    return true;
  if (role == WagerRole::judge &&
      wager.resolution_policy == WagerResolutionPolicy::designated &&
      (wager.state == WagerState::accepted_funded ||
       wager.state == WagerState::awaiting_resolution))
    return true;
  return call.owner && wager.state == WagerState::disputed;
}

[[nodiscard]] bool sealed_offer_revealed(SqliteConnection &connection,
                                         const WagerRecord &wager,
                                         const DiscordSnowflake &viewer) {
  if (wager.visibility != WagerVisibility::sealed ||
      viewer == wager.creator_user_id || viewer != wager.target_user_id)
    return true;
  return sealed_offer_delivered(connection, wager);
}

struct BoundedEvidence {
  std::vector<std::string> entries;
  std::size_t total_count{};
};

[[nodiscard]] BoundedEvidence load_evidence(SqliteConnection &connection,
                                            const std::string_view wager_id) {
  auto count = connection.prepare(
      "SELECT count(*) FROM tarot_wager_evidence WHERE wager_id = ?");
  count.bind(1, wager_id);
  if (!count.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Wager evidence count is unavailable."};
  const auto stored_count = count.column_int64(0);
  if (stored_count < 0)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Wager evidence count is invalid."};
  auto query = connection.prepare(
      "SELECT actor_role, body FROM tarot_wager_evidence "
      "WHERE wager_id = ? ORDER BY created_at_ms, evidence_id LIMIT ?");
  query.bind(1, wager_id);
  query.bind(2, static_cast<std::int64_t>(wager_history_evidence_limit));
  BoundedEvidence evidence{
      .entries = {}, .total_count = static_cast<std::size_t>(stored_count)};
  evidence.entries.reserve(
      std::min(evidence.total_count, wager_history_evidence_limit));
  while (query.step())
    evidence.entries.push_back(query.column_text(0) + ": " +
                               query.column_text(1));
  return evidence;
}

[[nodiscard]] std::vector<std::string>
load_resolution_activity(SqliteConnection &connection,
                         const std::string_view wager_id) {
  std::vector<std::string> activity;
  auto outcomes = connection.prepare(
      "SELECT actor_role, winner_role FROM tarot_wager_outcome "
      "WHERE wager_id = ? ORDER BY created_at_ms, submission_id");
  outcomes.bind(1, wager_id);
  while (outcomes.step())
    activity.push_back(outcomes.column_text(0) + " submitted " +
                       outcomes.column_text(1) + " as winner.");
  auto voids = connection.prepare(
      "SELECT actor_role FROM tarot_wager_void_consent "
      "WHERE wager_id = ? ORDER BY created_at_ms, consent_id");
  voids.bind(1, wager_id);
  while (voids.step())
    activity.push_back(voids.column_text(0) + " consented to void and refund.");
  return activity;
}

[[nodiscard]] WagerInvocation
scheduler_invocation(const WagerRecord &wager, const std::string &job_id,
                     const std::string &correlation,
                     const std::int64_t now_ms) {
  return WagerInvocation{
      .user_id = wager.creator_user_id,
      .guild_id = wager.guild_id,
      .channel_id = wager.channel_id,
      .interaction_idempotency_key = "wager:job:" + job_id,
      .correlation_id =
          correlation.empty() ? "wager-job:" + job_id : correlation,
      .now_ms = now_ms,
      .owner = false,
      .test_mode = wager.is_test,
  };
}

void enqueue_public_reminder(SqliteConnection &connection,
                             const WagerIdFactory &ids,
                             const WagerInvocation &call,
                             const WagerRecord &wager) {
  InteractionMessage message;
  message.content =
      wager.is_test ? "[TEST] Fate wager reminder" : "Fate wager reminder";
  message.embed = EmbedPayload{
      .color = 0x8B0000U,
      .title = "A peer wager awaits resolution",
      .description =
          wager.visibility == WagerVisibility::sealed
              ? "A sealed funded wager is approaching its outcome deadline. "
                "Participants should consult their private history."
              : "Wager `" + wager.wager_id.substr(0, 8) +
                    "` is approaching its outcome deadline.",
  };
  const auto outbox_id = next_id(ids);
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "tarot_wager",
      .aggregate_id = wager.wager_id,
      .target_guild_id = wager.guild_id,
      .target_channel_id = wager.channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = call.now_ms,
      .max_attempts = 5,
      .idempotency_key = "outbox:wager-reminder:" + wager.wager_id,
      .provider_nonce = nonce(outbox_id),
      .created_at_ms = call.now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_payload(
          PublicOutboxPayload{
              .request = PublicMessageRequest{.guild_id = wager.guild_id,
                                              .channel_id = wager.channel_id,
                                              .message = std::move(message)}},
          call.correlation_id, std::nullopt)));
}

[[nodiscard]] WagerMutationResult apply_deadline(SqliteConnection &connection,
                                                 const WagerIdFactory &ids,
                                                 WagerRecord wager,
                                                 const WagerDeadlinePhase phase,
                                                 const WagerInvocation &call) {
  WagerMutationResult result{.status = WagerMutationStatus::unchanged,
                             .wager = wager,
                             .controls = {},
                             .committed_event_types = {},
                             .public_delivery_created = false};
  if (phase == WagerDeadlinePhase::draft_expiry &&
      wager.state == WagerState::draft) {
    const WagerActionRequest action_request{
        .invocation = call,
        .wager_id = wager.wager_id,
        .token_id = std::nullopt,
        .action = WagerAction::discard,
        .starting_fate = 1,
        .offer_expiry_ms = 1,
        .resolution_grace_ms = 1,
        .next_id = ids,
    };
    return transition_unfunded_terminal(
        connection, action_request, wager, WagerState::expired,
        WagerRole::scheduler, "expired", "tarot.wager_expired.v1",
        fingerprint("deadline", wager.wager_id, "draft_expiry"));
  }
  if (phase == WagerDeadlinePhase::offer_expiry &&
      wager.state == WagerState::offered) {
    const WagerActionRequest action_request{
        .invocation = call,
        .wager_id = wager.wager_id,
        .token_id = std::nullopt,
        .action = WagerAction::cancel,
        .starting_fate = 1,
        .offer_expiry_ms = 1,
        .resolution_grace_ms = 1,
        .next_id = ids,
    };
    return transition_unfunded_terminal(
        connection, action_request, wager, WagerState::expired,
        WagerRole::scheduler, "expired", "tarot.wager_expired.v1",
        fingerprint("deadline", wager.wager_id, "offer_expiry"));
  }
  if (phase == WagerDeadlinePhase::reminder &&
      (wager.state == WagerState::accepted_funded ||
       wager.state == WagerState::awaiting_resolution)) {
    const auto event_id = next_id(ids);
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        event_for(call, event_id, "tarot.wager_reminded.v1", wager.wager_id,
                  "event:wager-reminder:" + wager.wager_id,
                  {{"visibility", visibility_name(wager.visibility)}})));
    insert_action(connection, ids, call, wager, WagerRole::scheduler,
                  "reminded", event_id,
                  "action:wager-reminder:" + wager.wager_id);
    const auto expires = wager.resolution_grace_until_ms.value_or(
        checked_add(call.now_ms, wager_control_lifetime_ms));
    const std::vector<WagerMutationResult::Control> no_controls;
    insert_pending_notice(connection, ids, call, wager, wager.creator_user_id,
                          "reminder", "Wager resolution reminder",
                          "Your funded wager `" + wager.wager_id +
                              "` is approaching its outcome deadline.",
                          no_controls, expires);
    if (wager.target_user_id != wager.creator_user_id)
      insert_pending_notice(connection, ids, call, wager, wager.target_user_id,
                            "reminder", "Wager resolution reminder",
                            "Your funded wager `" + wager.wager_id +
                                "` is approaching its outcome deadline.",
                            no_controls, expires);
    enqueue_public_reminder(connection, ids, call, wager);
    return {.status = WagerMutationStatus::applied,
            .wager = wager,
            .controls = {},
            .committed_event_types = {"tarot.wager_reminded.v1"},
            .public_delivery_created = true};
  }
  if (phase == WagerDeadlinePhase::outcome_due &&
      wager.state == WagerState::accepted_funded) {
    const auto event_id = next_id(ids);
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        event_for(call, event_id, "tarot.wager_outcome_due.v1", wager.wager_id,
                  "event:wager-outcome-due:" + wager.wager_id)));
    insert_action(connection, ids, call, wager, WagerRole::scheduler,
                  "outcome_due", event_id,
                  "action:wager-outcome-due:" + wager.wager_id);
    auto update = connection.prepare(
        "UPDATE tarot_wager SET state = 'awaiting_resolution', revision = "
        "revision + 1, "
        "updated_at_ms = max(?, updated_at_ms) WHERE wager_id = ? "
        "AND state = 'accepted_funded' AND revision = ?");
    update.bind(1, call.now_ms);
    update.bind(2, wager.wager_id);
    update.bind(3, static_cast<std::int64_t>(wager.revision));
    update.execute();
    wager = *load_wager(connection, wager.wager_id);
    cancel_job(connection, wager.wager_id, "reminder", call.now_ms);
    cancel_resolution_reminders(connection, wager.wager_id, call.now_ms);
    enqueue_card_edit(connection, ids, call, wager);
    return {.status = WagerMutationStatus::applied,
            .wager = wager,
            .controls = {},
            .committed_event_types = {"tarot.wager_outcome_due.v1"},
            .public_delivery_created = true};
  }
  if (phase == WagerDeadlinePhase::grace &&
      (wager.state == WagerState::accepted_funded ||
       wager.state == WagerState::awaiting_resolution)) {
    const auto event_id = next_id(ids);
    static_cast<void>(detail::insert_event_uncommitted(
        connection,
        event_for(call, event_id, "tarot.wager_disputed.v1", wager.wager_id,
                  "event:wager-grace:" + wager.wager_id,
                  {{"reason", "resolution_grace_elapsed"}})));
    insert_action(connection, ids, call, wager, WagerRole::scheduler,
                  "grace_elapsed", event_id,
                  "action:wager-grace:" + wager.wager_id);
    auto update = connection.prepare(
        "UPDATE tarot_wager SET state = 'disputed', revision = revision + 1, "
        "updated_at_ms = max(?, updated_at_ms) WHERE wager_id = ? "
        "AND state IN ('accepted_funded', 'awaiting_resolution') AND revision "
        "= ?");
    update.bind(1, call.now_ms);
    update.bind(2, wager.wager_id);
    update.bind(3, static_cast<std::int64_t>(wager.revision));
    update.execute();
    wager = *load_wager(connection, wager.wager_id);
    insert_dispute_notices(connection, ids, call, wager);
    cancel_job(connection, wager.wager_id, "reminder", call.now_ms);
    cancel_job(connection, wager.wager_id, "outcome_due", call.now_ms);
    cancel_resolution_reminders(connection, wager.wager_id, call.now_ms);
    enqueue_card_edit(connection, ids, call, wager);
    return {.status = WagerMutationStatus::applied,
            .wager = wager,
            .controls = {},
            .committed_event_types = {"tarot.wager_disputed.v1"},
            .public_delivery_created = true};
  }
  return result;
}

[[nodiscard]] WagerDeadlinePhase phase_value(const std::string_view value) {
  if (value == "draft_expiry")
    return WagerDeadlinePhase::draft_expiry;
  if (value == "offer_expiry")
    return WagerDeadlinePhase::offer_expiry;
  if (value == "reminder")
    return WagerDeadlinePhase::reminder;
  if (value == "outcome_due")
    return WagerDeadlinePhase::outcome_due;
  if (value == "grace")
    return WagerDeadlinePhase::grace;
  throw std::invalid_argument{"Stored wager deadline phase is invalid."};
}

} // namespace

WagerHistoryResult
SqliteWagerRepository::history(const WagerHistoryRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  if (request.wager_id && !valid_uuid_v4(*request.wager_id))
    throw std::invalid_argument{"Wager history reference is invalid."};
  if (request.cursor_id && !valid_uuid_v4(*request.cursor_id))
    throw std::invalid_argument{"Wager history cursor is invalid."};
  if (request.wager_id && request.cursor_id)
    throw std::invalid_argument{"Wager history request is ambiguous."};
  if (request.wager_id) {
    auto wager = load_wager(context_->connection(), *request.wager_id);
    if (!wager)
      return empty_history_result(WagerMutationStatus::not_found);
    if (!test_access_allowed(request.invocation, *wager))
      return empty_history_result(WagerMutationStatus::forbidden);
    const auto role = role_for(context_->connection(), request.invocation,
                               *wager, std::nullopt);
    if (!participant(role) &&
        !can_view_evidence(request.invocation, *wager, role))
      return empty_history_result(WagerMutationStatus::forbidden);
    if (!sealed_offer_revealed(context_->connection(), *wager,
                               request.invocation.user_id))
      return empty_history_result(WagerMutationStatus::forbidden);
    require_id_factory(request.next_id);
    Transaction transaction{context_->connection(), TransactionMode::immediate};
    auto evidence = can_view_evidence(request.invocation, *wager, role)
                        ? load_evidence(context_->connection(), wager->wager_id)
                        : BoundedEvidence{};
    auto result = WagerHistoryResult{
        .status = WagerMutationStatus::applied,
        .wagers = {*wager},
        .outcomes =
            load_resolution_activity(context_->connection(), wager->wager_id),
        .evidence = std::move(evidence.entries),
        .evidence_total_count = evidence.total_count,
        .next_cursor_id = std::nullopt,
        .controls = current_controls(context_->connection(), request.next_id,
                                     request.invocation, *wager, role),
        .exact = true};
    transaction.commit();
    return result;
  }
  if (request.cursor_id) {
    auto cursor = context_->connection().prepare(
        "SELECT user_id,expires_at_ms,next_cursor_id FROM "
        "tarot_wager_history_cursor WHERE cursor_id = ?");
    cursor.bind(1, *request.cursor_id);
    if (!cursor.step())
      return {.status = WagerMutationStatus::not_found,
              .wagers = {},
              .outcomes = {},
              .evidence = {},
              .evidence_total_count = 0,
              .next_cursor_id = std::nullopt,
              .controls = {},
              .exact = false};
    if (cursor.column_text(0) != request.invocation.user_id.str())
      return {.status = WagerMutationStatus::forbidden,
              .wagers = {},
              .outcomes = {},
              .evidence = {},
              .evidence_total_count = 0,
              .next_cursor_id = std::nullopt,
              .controls = {},
              .exact = false};
    if (cursor.column_int64(1) <= request.invocation.now_ms)
      return {.status = WagerMutationStatus::expired,
              .wagers = {},
              .outcomes = {},
              .evidence = {},
              .evidence_total_count = 0,
              .next_cursor_id = std::nullopt,
              .controls = {},
              .exact = false};
    WagerHistoryResult result{
        .status = WagerMutationStatus::applied,
        .wagers = {},
        .outcomes = {},
        .evidence = {},
        .evidence_total_count = 0,
        .next_cursor_id =
            cursor.column_is_null(2)
                ? std::nullopt
                : std::optional<std::string>{cursor.column_text(2)},
        .controls = {},
        .exact = false};
    auto items = context_->connection().prepare(
        "SELECT wager_id FROM tarot_wager_history_item WHERE cursor_id = ? "
        "ORDER BY position");
    items.bind(1, *request.cursor_id);
    while (items.step()) {
      auto wager = load_wager(context_->connection(), items.column_text(0));
      if (wager && test_access_allowed(request.invocation, *wager))
        result.wagers.push_back(*wager);
    }
    return result;
  }
  require_id_factory(request.next_id);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto purge = context_->connection().prepare(
      "DELETE FROM tarot_wager_history_cursor WHERE expires_at_ms <= ?");
  purge.bind(1, request.invocation.now_ms);
  purge.execute();
  auto query = context_->connection().prepare(
      "SELECT wager.wager_id FROM tarot_wager wager WHERE "
      "(creator_user_id = ? OR target_user_id = ?) "
      "AND (is_test = 0 OR ? = 1) "
      "AND NOT (visibility = 'sealed' "
      "AND creator_user_id <> ? AND target_user_id = ? "
      "AND NOT EXISTS (SELECT 1 FROM tarot_wager_notice link "
      "JOIN pending_notice notice ON notice.notice_id = link.notice_id "
      "WHERE link.wager_id = wager.wager_id "
      "AND link.purpose = 'sealed_offer' AND link.target_user_id = ? "
      "AND notice.opened_at_ms IS NOT NULL)) "
      "ORDER BY updated_at_ms DESC, wager_id DESC LIMIT 50");
  query.bind(1, request.invocation.user_id.str());
  query.bind(2, request.invocation.user_id.str());
  query.bind(3,
             request.invocation.owner && request.invocation.test_mode ? 1 : 0);
  query.bind(4, request.invocation.user_id.str());
  query.bind(5, request.invocation.user_id.str());
  query.bind(6, request.invocation.user_id.str());
  std::vector<std::string> wager_ids;
  while (query.step())
    wager_ids.push_back(query.column_text(0));
  WagerHistoryResult result{.status = WagerMutationStatus::applied,
                            .wagers = {},
                            .outcomes = {},
                            .evidence = {},
                            .evidence_total_count = 0,
                            .next_cursor_id = std::nullopt,
                            .controls = {},
                            .exact = false};
  const auto first_page_size =
      std::min(wager_ids.size(), wager_history_page_size);
  for (std::size_t index = 0; index < first_page_size; ++index)
    result.wagers.push_back(
        *load_wager(context_->connection(), wager_ids[index]));
  std::optional<std::string> next_cursor;
  const auto page_count = (wager_ids.size() + wager_history_page_size - 1) /
                          wager_history_page_size;
  for (std::size_t page = page_count; page > 1; --page) {
    const auto offset = (page - 1) * wager_history_page_size;
    const auto item_count =
        std::min(wager_history_page_size, wager_ids.size() - offset);
    const auto cursor_id = next_id(request.next_id);
    auto insert_cursor =
        context_->connection().prepare("INSERT INTO tarot_wager_history_cursor "
                                       "(cursor_id,user_id,item_count,next_"
                                       "cursor_id,created_at_ms,expires_at_ms) "
                                       "VALUES (?,?,?,?,?,?)");
    insert_cursor.bind(1, cursor_id);
    insert_cursor.bind(2, request.invocation.user_id.str());
    insert_cursor.bind(3, static_cast<std::int64_t>(item_count));
    if (next_cursor)
      insert_cursor.bind(4, *next_cursor);
    else
      insert_cursor.bind_null(4);
    insert_cursor.bind(5, request.invocation.now_ms);
    insert_cursor.bind(
        6, checked_add(request.invocation.now_ms, wager_control_lifetime_ms));
    insert_cursor.execute();
    for (std::size_t position = 0; position < item_count; ++position) {
      auto insert_item = context_->connection().prepare(
          "INSERT INTO tarot_wager_history_item "
          "(cursor_id,position,wager_id) VALUES (?,?,?)");
      insert_item.bind(1, cursor_id);
      insert_item.bind(2, static_cast<std::int64_t>(position));
      insert_item.bind(3, wager_ids[offset + position]);
      insert_item.execute();
    }
    next_cursor = cursor_id;
  }
  result.next_cursor_id = std::move(next_cursor);
  transaction.commit();
  return result;
}

WagerHistoryResult
SqliteWagerRepository::disputes(const WagerHistoryRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  if (request.wager_id && !valid_uuid_v4(*request.wager_id))
    throw std::invalid_argument{"Wager dispute reference is invalid."};
  std::vector<std::string> ids;
  if (request.wager_id) {
    ids.push_back(*request.wager_id);
  } else {
    auto query = context_->connection().prepare(
        "SELECT wager_id FROM tarot_wager WHERE state = 'disputed' "
        "AND (creator_user_id = ? OR target_user_id = ? OR judge_user_id = ? "
        "OR ? = 1) "
        "AND (is_test = 0 OR ? = 1) "
        "ORDER BY updated_at_ms DESC, wager_id DESC LIMIT 5");
    query.bind(1, request.invocation.user_id.str());
    query.bind(2, request.invocation.user_id.str());
    query.bind(3, request.invocation.user_id.str());
    query.bind(4, request.invocation.owner ? 1 : 0);
    query.bind(5, request.invocation.owner && request.invocation.test_mode ? 1
                                                                           : 0);
    while (query.step())
      ids.push_back(query.column_text(0));
  }
  WagerHistoryResult result{.status = WagerMutationStatus::applied,
                            .wagers = {},
                            .outcomes = {},
                            .evidence = {},
                            .evidence_total_count = 0,
                            .next_cursor_id = std::nullopt,
                            .controls = {},
                            .exact = false};
  for (const auto &id : ids) {
    auto wager = load_wager(context_->connection(), id);
    if (!wager || wager->state != WagerState::disputed)
      continue;
    if (!test_access_allowed(request.invocation, *wager)) {
      if (request.wager_id)
        return empty_history_result(WagerMutationStatus::forbidden);
      continue;
    }
    const auto role = role_for(context_->connection(), request.invocation,
                               *wager, std::nullopt);
    const bool visible = participant(role) || request.invocation.owner;
    if (!visible) {
      if (request.wager_id)
        return empty_history_result(WagerMutationStatus::forbidden);
      continue;
    }
    result.wagers.push_back(*wager);
    if (request.wager_id &&
        can_view_evidence(request.invocation, *wager, role)) {
      auto evidence = load_evidence(context_->connection(), wager->wager_id);
      result.evidence = std::move(evidence.entries);
      result.evidence_total_count = evidence.total_count;
      result.outcomes =
          load_resolution_activity(context_->connection(), wager->wager_id);
      result.exact = true;
    }
  }
  if (request.wager_id && result.wagers.empty())
    result.status = WagerMutationStatus::not_found;
  return result;
}

WagerMutationResult
SqliteWagerRepository::handle_deadline(const WagerDeadlineRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  const auto *payload =
      std::get_if<WagerDeadlineJobPayload>(&request.job.payload);
  if (payload == nullptr || payload->wager_id.empty())
    throw std::invalid_argument{"Wager deadline payload is invalid."};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto wager = load_wager(context_->connection(), payload->wager_id);
  if (!wager) {
    complete_job_claim(context_->connection(), request.job, request.now_ms);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  auto link = context_->connection().prepare(
      "SELECT 1 FROM tarot_wager_job WHERE wager_id = ? AND phase = ? AND "
      "job_id = ?");
  link.bind(1, wager->wager_id);
  link.bind(2, payload->phase);
  link.bind(3, request.job.job_id);
  if (!link.step())
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "Wager deadline link is invalid."};
  const auto call = scheduler_invocation(
      *wager, request.job.job_id, request.job.correlation_id, request.now_ms);
  auto result = apply_deadline(context_->connection(), request.next_id, *wager,
                               phase_value(payload->phase), call);
  complete_job_claim(context_->connection(), request.job, request.now_ms);
  transaction.commit();
  return result;
}

WagerMutationResult
SqliteWagerRepository::set_test_role(const WagerTestRoleRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!request.invocation.owner || !request.invocation.test_mode ||
      !valid_uuid_v4(request.wager_id) || request.role == WagerRole::scheduler)
    throw std::invalid_argument{"Test wager role request is invalid."};
  const auto fp =
      fingerprint("test_role", request.wager_id, wager_role_name(request.role));
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "test_role", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), request.wager_id);
  if (!wager) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "test_role", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  if (!wager->is_test ||
      (request.role == WagerRole::judge &&
       wager->resolution_policy != WagerResolutionPolicy::designated)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "test_role", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden, wager);
  }
  auto current = context_->connection().prepare(
      "SELECT role FROM tarot_wager_test_role WHERE wager_id = ? AND "
      "owner_user_id = ?");
  current.bind(1, wager->wager_id);
  current.bind(2, request.invocation.user_id.str());
  if (current.step() &&
      current.column_text(0) == wager_role_name(request.role)) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "test_role", fp, WagerMutationStatus::unchanged);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::unchanged, wager);
  }
  auto upsert = context_->connection().prepare(
      "INSERT INTO tarot_wager_test_role "
      "(wager_id, owner_user_id, role, revision, updated_at_ms) VALUES (?, ?, "
      "?, 1, ?) "
      "ON CONFLICT(wager_id, owner_user_id) DO UPDATE SET role = "
      "excluded.role, "
      "revision = tarot_wager_test_role.revision + 1, updated_at_ms = "
      "excluded.updated_at_ms");
  upsert.bind(1, wager->wager_id);
  upsert.bind(2, request.invocation.user_id.str());
  upsert.bind(3, wager_role_name(request.role));
  upsert.bind(4, request.invocation.now_ms);
  upsert.execute();
  const auto event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, event_id, "tarot.wager_test_role_set.v1",
                wager->wager_id,
                "event:wager-test-role:" +
                    request.invocation.interaction_idempotency_key,
                {{"role", wager_role_name(request.role)}, {"is_test", true}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                *wager, WagerRole::owner, "test_role_set", event_id,
                "action:wager-test-role:" +
                    request.invocation.interaction_idempotency_key,
                std::string{"simulated role: "} +
                    wager_role_name(request.role));
  insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                 "test_role", fp, WagerMutationStatus::applied);
  transaction.commit();
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = {},
          .committed_event_types = {"tarot.wager_test_role_set.v1"},
          .public_delivery_created = false};
}

WagerMutationResult SqliteWagerRepository::force_test_deadline(
    const WagerTestDeadlineRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!request.invocation.owner || !request.invocation.test_mode ||
      !valid_uuid_v4(request.wager_id))
    throw std::invalid_argument{"Forced wager deadline request is invalid."};
  const auto fp = fingerprint("test_deadline", request.wager_id,
                              wager_deadline_phase_name(request.phase));
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "test_deadline", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), request.wager_id);
  if (!wager) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "test_deadline", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  if (!wager->is_test) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "test_deadline", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden, wager);
  }
  if (request.phase == WagerDeadlinePhase::reminder) {
    auto prior = context_->connection().prepare(
        "SELECT 1 FROM event_journal WHERE idempotency_key = ?");
    prior.bind(1, "event:wager-reminder:" + wager->wager_id);
    if (prior.step()) {
      cancel_job(context_->connection(), wager->wager_id, "reminder",
                 request.invocation.now_ms);
      insert_receipt(context_->connection(), request.invocation,
                     wager->wager_id, "test_deadline", fp,
                     WagerMutationStatus::unchanged);
      transaction.commit();
      return empty_mutation_result(WagerMutationStatus::unchanged, wager);
    }
  }
  auto call = request.invocation;
  call.interaction_idempotency_key += ":deadline-action";
  auto result = apply_deadline(context_->connection(), request.next_id, *wager,
                               request.phase, call);
  if (request.phase == WagerDeadlinePhase::reminder &&
      result.status == WagerMutationStatus::applied)
    cancel_job(context_->connection(), wager->wager_id, "reminder",
               request.invocation.now_ms);
  insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                 "test_deadline", fp, result.status);
  transaction.commit();
  return result;
}

WagerMutationResult SqliteWagerRepository::cleanup_test_wager(
    const WagerTestCleanupRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  require_id_factory(request.next_id);
  if (!request.invocation.owner || !request.invocation.test_mode ||
      !valid_uuid_v4(request.wager_id) || blank(request.reason) ||
      request.reason.size() > 200)
    throw std::invalid_argument{"Test wager cleanup request is invalid."};
  const auto fp = fingerprint("test_cleanup", request.wager_id, request.reason);
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  if (auto replay = replay_receipt(context_->connection(), request.invocation,
                                   "test_cleanup", fp)) {
    transaction.commit();
    return *replay;
  }
  auto wager = load_wager(context_->connection(), request.wager_id);
  if (!wager) {
    insert_receipt(context_->connection(), request.invocation, std::nullopt,
                   "test_cleanup", fp, WagerMutationStatus::not_found);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::not_found);
  }
  const bool terminal = wager->state == WagerState::resolved ||
                        wager->state == WagerState::void_refunded ||
                        wager->state == WagerState::cancelled ||
                        wager->state == WagerState::declined ||
                        wager->state == WagerState::expired;
  if (!wager->is_test || !terminal) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "test_cleanup", fp, WagerMutationStatus::forbidden);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::forbidden, wager);
  }
  struct OriginalTransaction {
    std::string transaction_id;
    std::size_t posting_count{};
  };
  std::vector<OriginalTransaction> originals;
  auto query = context_->connection().prepare(
      "SELECT tx.transaction_id, tx.expected_posting_count "
      "FROM tarot_wager_transfer transfer "
      "JOIN tarot_transaction tx ON tx.transaction_id = "
      "transfer.transaction_id "
      "WHERE transfer.wager_id = ? AND tx.is_test = 1 AND tx.state = "
      "'committed' "
      "AND tx.transaction_type IN ('WAGER_ESCROW_FUND', 'WAGER_PAYOUT', "
      "'WAGER_REFUND') "
      "AND NOT EXISTS (SELECT 1 FROM tarot_wager_test_cleanup cleanup "
      "WHERE cleanup.wager_id = transfer.wager_id "
      "AND cleanup.original_transaction_id = tx.transaction_id) "
      "ORDER BY tx.ledger_sequence DESC");
  query.bind(1, wager->wager_id);
  while (query.step())
    originals.push_back({query.column_text(0),
                         static_cast<std::size_t>(query.column_int64(1))});
  if (originals.empty()) {
    insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                   "test_cleanup", fp, WagerMutationStatus::unchanged);
    transaction.commit();
    return empty_mutation_result(WagerMutationStatus::unchanged, wager);
  }
  for (const auto &original : originals) {
    const auto reversal_id = next_id(request.next_id);
    const auto event_id = next_id(request.next_id);
    static_cast<void>(detail::insert_event_uncommitted(
        context_->connection(),
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
            .idempotency_key = "event:wager-cleanup:" + wager->wager_id + ":" +
                               original.transaction_id,
            .payload_json =
                Json{{"original_transaction_id", original.transaction_id},
                     {"reason", request.reason},
                     {"is_test", true}}
                    .dump(),
        }));
    insert_transaction(context_->connection(), reversal_id, "TEST_REVERSAL",
                       original.posting_count, event_id,
                       "tx:wager-cleanup:" + wager->wager_id + ":" +
                           original.transaction_id,
                       request.invocation.user_id, request.reason, true,
                       original.transaction_id, request.invocation.now_ms);
    auto cleanup = context_->connection().prepare(
        "INSERT INTO tarot_wager_test_cleanup "
        "(wager_id, original_transaction_id, reversal_transaction_id, "
        "actor_user_id, reason, created_at_ms) VALUES (?, ?, ?, ?, ?, ?)");
    cleanup.bind(1, wager->wager_id);
    cleanup.bind(2, original.transaction_id);
    cleanup.bind(3, reversal_id);
    cleanup.bind(4, request.invocation.user_id.str());
    cleanup.bind(5, request.reason);
    cleanup.bind(6, request.invocation.now_ms);
    cleanup.execute();
    auto postings = context_->connection().prepare(
        "SELECT account_id, amount FROM tarot_posting "
        "WHERE transaction_id = ? ORDER BY posting_id");
    postings.bind(1, original.transaction_id);
    std::size_t count{};
    while (postings.step()) {
      insert_posting(context_->connection(), request.next_id, reversal_id,
                     postings.column_text(0), -postings.column_int64(1),
                     request.invocation.now_ms);
      ++count;
    }
    if (count != original.posting_count)
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "Test wager cleanup posting count changed."};
    seal_transaction(context_->connection(), reversal_id,
                     request.invocation.now_ms);
  }
  const auto cleanup_event_id = next_id(request.next_id);
  static_cast<void>(detail::insert_event_uncommitted(
      context_->connection(),
      event_for(request.invocation, cleanup_event_id,
                "tarot.wager_test_cleaned.v1", wager->wager_id,
                "event:wager-cleaned:" + wager->wager_id,
                {{"reversal_count", originals.size()}, {"is_test", true}})));
  insert_action(context_->connection(), request.next_id, request.invocation,
                *wager, WagerRole::owner, "test_cleaned", cleanup_event_id,
                "action:wager-cleanup:" + wager->wager_id, request.reason);
  insert_receipt(context_->connection(), request.invocation, wager->wager_id,
                 "test_cleanup", fp, WagerMutationStatus::applied);
  transaction.commit();
  return {.status = WagerMutationStatus::applied,
          .wager = wager,
          .controls = {},
          .committed_event_types = {"tarot.transaction_reversed.v1",
                                    "tarot.wager_test_cleaned.v1"},
          .public_delivery_created = false};
}

namespace {

[[nodiscard]] std::int64_t scalar(SqliteConnection &connection,
                                  const std::string_view sql) {
  auto query = connection.prepare(sql);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Wager invariant query failed."};
  return query.column_int64(0);
}

} // namespace

WagerInvariantReport SqliteWagerRepository::check_invariants() {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  WagerInvariantReport report{
      .valid = true,
      .open_funded_obligation_count = static_cast<std::size_t>(scalar(
          connection, "SELECT (SELECT count(*) FROM tarot_wager WHERE state IN "
                      "('accepted_funded','awaiting_resolution','disputed')) + "
                      "(SELECT count(*) FROM tarot_house_wager WHERE "
                      "state='accepted_funded')")),
      .open_funded_obligation_amount = scalar(
          connection,
          "SELECT COALESCE((SELECT sum(2 * stake) FROM tarot_wager WHERE "
          "state IN ('accepted_funded','awaiting_resolution','disputed')),0) "
          "+ COALESCE((SELECT sum(stake + profit) FROM tarot_house_wager "
          "WHERE state='accepted_funded'),0)"),
      .escrow_balance = scalar(
          connection,
          "SELECT COALESCE(sum(posting.amount), 0) FROM tarot_posting posting "
          "JOIN tarot_transaction tx ON tx.transaction_id = "
          "posting.transaction_id "
          "JOIN tarot_account account ON account.account_id = "
          "posting.account_id "
          "WHERE account.account_kind = 'ESCROW' AND tx.state = 'committed'"),
      .disputed_count = static_cast<std::size_t>(
          scalar(connection,
                 "SELECT count(*) FROM tarot_wager WHERE state = 'disputed'")),
      .malformed_transfer_count = static_cast<std::size_t>(scalar(
          connection,
          "SELECT (SELECT count(*) FROM tarot_transaction tx WHERE "
          "tx.transaction_type LIKE 'WAGER_%' "
          "AND (tx.state <> 'committed' OR ((EXISTS (SELECT 1 "
          "FROM tarot_wager_transfer transfer WHERE transfer.transaction_id = "
          "tx.transaction_id)) + (EXISTS (SELECT 1 FROM tarot_house_transfer "
          "transfer WHERE transfer.transaction_id = tx.transaction_id))) <> 1 "
          "OR "
          "(SELECT count(*) FROM tarot_posting posting WHERE "
          "posting.transaction_id = tx.transaction_id) "
          "<> tx.expected_posting_count)) "
          "+ (SELECT count(*) FROM tarot_wager_transfer transfer "
          "LEFT JOIN tarot_wager wager ON wager.wager_id = transfer.wager_id "
          "LEFT JOIN tarot_transaction tx ON tx.transaction_id = "
          "transfer.transaction_id "
          "LEFT JOIN event_journal event ON event.event_id = tx.event_id "
          "WHERE wager.wager_id IS NULL OR tx.transaction_id IS NULL "
          "OR tx.state <> 'committed' OR tx.is_test <> wager.is_test "
          "OR event.event_id IS NULL OR event.aggregate_type <> 'tarot_wager' "
          "OR event.aggregate_id <> wager.wager_id "
          "OR NOT ((transfer.transfer_kind = 'fund' "
          "AND tx.transaction_type = 'WAGER_ESCROW_FUND' "
          "AND event.event_type = 'tarot.wager_funded.v1' "
          "AND wager.state IN ('accepted_funded','awaiting_resolution',"
          "'disputed','resolved','void_refunded')) "
          "OR (transfer.transfer_kind = 'payout' "
          "AND tx.transaction_type = 'WAGER_PAYOUT' "
          "AND event.event_type = 'tarot.wager_resolved.v1' "
          "AND wager.state = 'resolved') "
          "OR (transfer.transfer_kind = 'refund' "
          "AND tx.transaction_type = 'WAGER_REFUND' "
          "AND event.event_type = 'tarot.wager_voided.v1' "
          "AND wager.state = 'void_refunded')))")),
      .invalid_deadline_action_link_count = static_cast<std::size_t>(scalar(
          connection,
          "SELECT (SELECT count(*) FROM tarot_wager_job link "
          "JOIN scheduled_job job ON job.job_id = link.job_id "
          "WHERE job.aggregate_type <> 'tarot_wager' OR job.aggregate_id <> "
          "link.wager_id "
          "OR job.job_type <> 'tarot.wager-deadline.v1' "
          "OR json_extract(job.payload_json,'$.wager_id') <> link.wager_id "
          "OR json_extract(job.payload_json,'$.phase') <> link.phase "
          "OR json_extract(job.payload_json,'$.expected_revision') <> "
          "link.expected_revision) "
          "+ (SELECT count(*) FROM tarot_wager wager WHERE NOT EXISTS "
          "(SELECT 1 FROM tarot_wager_job link WHERE link.wager_id = "
          "wager.wager_id AND link.phase = 'draft_expiry')) "
          "+ (SELECT count(*) FROM tarot_wager wager WHERE "
          "(wager.state = 'offered' OR EXISTS (SELECT 1 FROM "
          "tarot_wager_action action WHERE action.wager_id = wager.wager_id "
          "AND action.action = 'confirmed')) AND NOT EXISTS (SELECT 1 FROM "
          "tarot_wager_job link WHERE link.wager_id = wager.wager_id "
          "AND link.phase = 'offer_expiry')) "
          "+ (SELECT count(*) FROM tarot_wager wager WHERE "
          "wager.fund_transaction_id IS NOT NULL AND (NOT EXISTS (SELECT 1 "
          "FROM tarot_wager_job link WHERE link.wager_id = wager.wager_id "
          "AND link.phase = 'reminder') OR NOT EXISTS (SELECT 1 FROM "
          "tarot_wager_job link WHERE link.wager_id = wager.wager_id "
          "AND link.phase = 'outcome_due') OR NOT EXISTS (SELECT 1 FROM "
          "tarot_wager_job link WHERE link.wager_id = wager.wager_id "
          "AND link.phase = 'grace'))) "
          "+ (SELECT count(*) FROM tarot_wager_action action "
          "LEFT JOIN event_journal event ON event.event_id = action.event_id "
          "WHERE event.event_id IS NULL OR event.aggregate_type <> "
          "'tarot_wager' "
          "OR event.aggregate_id <> action.wager_id "
          "OR event.actor_user_id <> action.actor_user_id)")),
      .orphaned_link_count = static_cast<std::size_t>(scalar(
          connection,
          "SELECT (SELECT count(*) FROM tarot_wager WHERE state IN "
          "('accepted_funded','awaiting_resolution','disputed','resolved','"
          "void_refunded') "
          "AND NOT EXISTS (SELECT 1 FROM tarot_wager_transfer transfer "
          "WHERE transfer.wager_id = tarot_wager.wager_id AND "
          "transfer.transfer_kind = 'fund')) "
          "+ (SELECT count(*) FROM tarot_wager WHERE state IN "
          "('resolved','void_refunded') "
          "AND NOT EXISTS (SELECT 1 FROM tarot_wager_resolution resolution "
          "WHERE resolution.wager_id = tarot_wager.wager_id))")),
  };
  report.valid =
      report.escrow_balance == report.open_funded_obligation_amount &&
      report.malformed_transfer_count == 0 &&
      report.invalid_deadline_action_link_count == 0 &&
      report.orphaned_link_count == 0;
  return report;
}

} // namespace sanguinius::persistence
