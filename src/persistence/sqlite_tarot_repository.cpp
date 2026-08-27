#include "sanguinius/persistence/sqlite_tarot_repository.hpp"

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr std::array<std::string_view, 4> system_kinds{"MINT", "HOUSE",
                                                       "ESCROW", "BURN"};

[[nodiscard]] std::int64_t checked_add(const std::int64_t left,
                                       const std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    throw std::overflow_error{"Tarot balance arithmetic overflowed."};
  return left + right;
}

[[nodiscard]] bool blank_reason(const std::string_view reason) {
  return reason.empty() ||
         std::ranges::all_of(reason, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

[[nodiscard]] std::string visibility_name(const TarotVisibility visibility) {
  return visibility == TarotVisibility::private_result ? "private" : "public";
}

[[nodiscard]] TarotVisibility visibility_value(const std::string_view value) {
  if (value == "public")
    return TarotVisibility::public_result;
  if (value == "private")
    return TarotVisibility::private_result;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Tarot visibility is incompatible."};
}

[[nodiscard]] std::string recovery_kind_name(const TarotRecoveryKind kind) {
  return kind == TarotRecoveryKind::grace ? "GRACE" : "TRIAL";
}

[[nodiscard]] TarotRecoveryKind recovery_kind(const std::string_view value) {
  if (value == "GRACE")
    return TarotRecoveryKind::grace;
  if (value == "TRIAL")
    return TarotRecoveryKind::trial;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Tarot recovery kind is incompatible."};
}

[[nodiscard]] std::string custom_id(const std::string_view token_id) {
  return std::string{tarot_component_prefix} + std::string{token_id};
}

[[nodiscard]] std::optional<std::string>
account_for_user(SqliteConnection &connection,
                 const DiscordSnowflake &user_id) {
  auto query =
      connection.prepare("SELECT account_id FROM tarot_account "
                         "WHERE account_kind = 'HUMAN' AND user_id = ?");
  query.bind(1, user_id.str());
  if (!query.step())
    return std::nullopt;
  auto result = query.column_text(0);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot human account."};
  return result;
}

[[nodiscard]] std::string system_account(SqliteConnection &connection,
                                         const std::string_view kind) {
  auto query = connection.prepare(
      "SELECT account_id FROM tarot_account WHERE account_kind = ?");
  query.bind(1, kind);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot system account is missing."};
  auto result = query.column_text(0);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot system account."};
  return result;
}

[[nodiscard]] std::int64_t balance_for_account(SqliteConnection &connection,
                                               const std::string_view account) {
  auto query = connection.prepare(
      "SELECT posting.amount FROM tarot_posting posting "
      "JOIN tarot_transaction tx ON tx.transaction_id = posting.transaction_id "
      "WHERE posting.account_id = ? AND tx.state = 'committed' "
      "ORDER BY tx.ledger_sequence, posting.posting_id");
  query.bind(1, account);
  std::int64_t result{};
  while (query.step())
    result = checked_add(result, query.column_int64(0));
  return result;
}

[[nodiscard]] std::int64_t
balance_for_account_through_transaction(SqliteConnection &connection,
                                        const std::string_view account,
                                        const std::string_view transaction_id) {
  auto target =
      connection.prepare("SELECT ledger_sequence FROM tarot_transaction "
                         "WHERE transaction_id = ? AND state = 'committed'");
  target.bind(1, transaction_id);
  if (!target.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot recovery transaction is missing."};
  const auto ledger_sequence = target.column_int64(0);
  if (target.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot recovery transaction."};

  auto postings = connection.prepare(
      "SELECT posting.amount FROM tarot_posting posting "
      "JOIN tarot_transaction tx ON tx.transaction_id = posting.transaction_id "
      "WHERE posting.account_id = ? AND tx.state = 'committed' "
      "AND tx.ledger_sequence <= ? "
      "ORDER BY tx.ledger_sequence, posting.posting_id");
  postings.bind(1, account);
  postings.bind(2, ledger_sequence);
  std::int64_t result{};
  while (postings.step())
    result = checked_add(result, postings.column_int64(0));
  return result;
}

[[nodiscard]] EventJournalEntry
event_for(const TarotInvocation &call, std::string event_id,
          std::string event_type, std::string aggregate_type,
          std::string aggregate_id, std::string idempotency_key,
          nlohmann::json payload) {
  return EventJournalEntry{
      .event_id = std::move(event_id),
      .event_type = std::move(event_type),
      .aggregate_type = std::move(aggregate_type),
      .aggregate_id = std::move(aggregate_id),
      .actor_user_id = call.user_id,
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

void insert_transaction(
    SqliteConnection &connection, const std::string_view transaction_id,
    const std::string_view transaction_type, const std::string_view event_id,
    const std::string_view idempotency_key,
    const DiscordSnowflake &actor_user_id,
    const std::optional<std::string> &reason, const bool is_test,
    const std::optional<std::string> &reversal_of, const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_transaction "
      "(transaction_id, transaction_type, state, expected_posting_count, "
      "event_id, idempotency_key, actor_user_id, reason, is_test, "
      "reversal_of_transaction_id, created_at_ms) "
      "VALUES (?, ?, 'prepared', 2, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, transaction_id);
  insert.bind(2, transaction_type);
  insert.bind(3, event_id);
  insert.bind(4, idempotency_key);
  insert.bind(5, actor_user_id.str());
  if (reason)
    insert.bind(6, *reason);
  else
    insert.bind_null(6);
  insert.bind(7, is_test ? 1 : 0);
  if (reversal_of)
    insert.bind(8, *reversal_of);
  else
    insert.bind_null(8);
  insert.bind(9, now_ms);
  insert.execute();
}

void insert_posting(SqliteConnection &connection,
                    const std::string_view posting_id,
                    const std::string_view transaction_id,
                    const std::string_view account_id,
                    const std::int64_t amount, const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_posting "
      "(posting_id, transaction_id, account_id, amount, created_at_ms) "
      "VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, posting_id);
  insert.bind(2, transaction_id);
  insert.bind(3, account_id);
  insert.bind(4, amount);
  insert.bind(5, now_ms);
  insert.execute();
}

void seal_transaction(SqliteConnection &connection,
                      const std::string_view transaction_id,
                      const std::int64_t now_ms) {
  auto seal = connection.prepare(
      "UPDATE tarot_transaction SET state = 'committed', committed_at_ms = ? "
      "WHERE transaction_id = ? AND state = 'prepared'");
  seal.bind(1, now_ms);
  seal.bind(2, transaction_id);
  seal.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Tarot transaction could not be sealed."};
}

struct InteractionReceipt {
  std::string operation;
  std::string account_id;
  nlohmann::json request;
  nlohmann::json result;
  std::optional<std::string> claim_id;
  std::optional<std::string> transaction_id;
};

[[nodiscard]] std::optional<InteractionReceipt>
load_receipt(SqliteConnection &connection,
             const std::string_view idempotency_key) {
  auto query = connection.prepare(
      "SELECT operation, account_id, request_json, result_json, claim_id, "
      "transaction_id FROM tarot_interaction_receipt WHERE idempotency_key = "
      "?");
  query.bind(1, idempotency_key);
  if (!query.step())
    return std::nullopt;
  nlohmann::json stored_request;
  nlohmann::json stored_result;
  try {
    stored_request = nlohmann::json::parse(query.column_text(2));
    stored_result = nlohmann::json::parse(query.column_text(3));
  } catch (const nlohmann::json::exception &) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot interaction receipt is incompatible."};
  }
  if (!stored_request.is_object() || !stored_result.is_object())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot interaction receipt is incompatible."};
  InteractionReceipt receipt{
      .operation = query.column_text(0),
      .account_id = query.column_text(1),
      .request = std::move(stored_request),
      .result = std::move(stored_result),
      .claim_id = query.column_is_null(4)
                      ? std::nullopt
                      : std::optional<std::string>{query.column_text(4)},
      .transaction_id = query.column_is_null(5)
                            ? std::nullopt
                            : std::optional<std::string>{query.column_text(5)},
  };
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot interaction receipt."};
  return receipt;
}

void insert_receipt(SqliteConnection &connection,
                    const std::string_view idempotency_key,
                    const std::string_view operation,
                    const std::string_view account_id,
                    const nlohmann::json &request, const nlohmann::json &result,
                    const std::optional<std::string> &claim_id,
                    const std::optional<std::string> &transaction_id,
                    const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tarot_interaction_receipt "
      "(idempotency_key, operation, account_id, request_json, result_json, "
      "claim_id, transaction_id, created_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, "
      "?)");
  insert.bind(1, idempotency_key);
  insert.bind(2, operation);
  insert.bind(3, account_id);
  insert.bind(4, request.dump());
  insert.bind(5, result.dump());
  if (claim_id)
    insert.bind(6, *claim_id);
  else
    insert.bind_null(6);
  if (transaction_id)
    insert.bind(7, *transaction_id);
  else
    insert.bind_null(7);
  insert.bind(8, now_ms);
  insert.execute();
}

[[noreturn]] void receipt_conflict() {
  throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                      SQLITE_CONSTRAINT,
                      "Tarot interaction idempotency conflict."};
}

struct ClaimRecord {
  std::string claim_id;
  std::string account_id;
  TarotRecoveryKind kind{TarotRecoveryKind::grace};
  std::string state;
  TarotVisibility visibility{TarotVisibility::public_result};
  bool is_test{};
  std::int64_t threshold{};
  std::optional<std::int64_t> grace_target;
  std::int64_t eligibility_balance{};
  std::optional<std::int64_t> reward;
  std::optional<std::int64_t> prompt_variant;
  std::optional<std::string> transaction_id;
  std::optional<std::int64_t> cooldown_until_ms;
  std::int64_t created_at_ms{};
  std::int64_t expires_at_ms{};
  std::optional<std::string> terminal_event_type;
};

[[nodiscard]] ClaimRecord claim_record(SqliteStatement &query) {
  ClaimRecord result{
      .claim_id = query.column_text(0),
      .account_id = query.column_text(1),
      .kind = recovery_kind(query.column_text(2)),
      .state = query.column_text(3),
      .visibility = visibility_value(query.column_text(4)),
      .is_test = query.column_int64(5) != 0,
      .threshold = query.column_int64(6),
      .grace_target = query.column_is_null(7)
                          ? std::nullopt
                          : std::optional<std::int64_t>{query.column_int64(7)},
      .eligibility_balance = query.column_int64(8),
      .reward = query.column_is_null(9)
                    ? std::nullopt
                    : std::optional<std::int64_t>{query.column_int64(9)},
      .prompt_variant =
          query.column_is_null(10)
              ? std::nullopt
              : std::optional<std::int64_t>{query.column_int64(10)},
      .transaction_id = query.column_is_null(11)
                            ? std::nullopt
                            : std::optional<std::string>{query.column_text(11)},
      .cooldown_until_ms =
          query.column_is_null(12)
              ? std::nullopt
              : std::optional<std::int64_t>{query.column_int64(12)},
      .created_at_ms = query.column_int64(13),
      .expires_at_ms = query.column_int64(14),
      .terminal_event_type =
          query.column_is_null(15)
              ? std::nullopt
              : std::optional<std::string>{query.column_text(15)},
  };
  return result;
}

[[nodiscard]] std::optional<ClaimRecord>
load_claim(SqliteConnection &connection, const std::string_view claim_id) {
  auto query = connection.prepare(
      "SELECT claim.claim_id, claim.account_id, claim.claim_type, claim.state, "
      "claim.visibility, claim.is_test, claim.eligibility_threshold, "
      "claim.grace_target, claim.eligibility_balance, claim.reward, "
      "draw.prompt_variant, "
      "claim.transaction_id, claim.cooldown_until_ms, claim.created_at_ms, "
      "claim.expires_at_ms, terminal_event.event_type "
      "FROM tarot_recovery_claim claim "
      "LEFT JOIN tarot_draw draw ON draw.draw_id = claim.draw_id "
      "LEFT JOIN event_journal terminal_event ON terminal_event.event_id = "
      "claim.event_id "
      "WHERE claim.claim_id = ?");
  query.bind(1, claim_id);
  if (!query.step())
    return std::nullopt;
  auto result = claim_record(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot recovery claim."};
  return result;
}

[[nodiscard]] TarotRecoveryStatus
stored_recovery_status(const ClaimRecord &claim) {
  if (claim.state == "pending")
    return TarotRecoveryStatus::pending;
  if (claim.state == "completed")
    return TarotRecoveryStatus::completed;
  if (claim.state == "expired")
    return TarotRecoveryStatus::expired;
  if (claim.state == "abandoned" && claim.terminal_event_type &&
      *claim.terminal_event_type == "tarot.recovery_eligibility_lost.v1")
    return TarotRecoveryStatus::lost_eligibility;
  return TarotRecoveryStatus::abandoned;
}

[[nodiscard]] TarotRecoveryResult
recovery_result(SqliteConnection &connection, const ClaimRecord &claim,
                const TarotRecoveryStatus status, const bool created = false,
                const bool public_created = false,
                std::vector<std::string> committed_event_types = {}) {
  const auto balance =
      claim.state == "completed" && claim.transaction_id
          ? balance_for_account_through_transaction(
                connection, claim.account_id, *claim.transaction_id)
          : balance_for_account(connection, claim.account_id);
  TarotRecoveryResult result{
      .status = status,
      .kind = claim.kind,
      .visibility = claim.visibility,
      .claim_id = claim.claim_id,
      .balance = balance,
      .reward = claim.reward,
      .cooldown_until_ms = claim.cooldown_until_ms,
      .prompt_variant = claim.prompt_variant,
      .custom_ids = {},
      .mutation_created = created,
      .public_delivery_created = public_created,
      .committed_event_types = std::move(committed_event_types),
  };
  if (status == TarotRecoveryStatus::pending) {
    auto tokens = connection.prepare(
        "SELECT token_id FROM interaction_token WHERE entity_type = "
        "'tarot_recovery_claim' AND entity_id = ? ORDER BY action");
    tokens.bind(1, claim.claim_id);
    while (tokens.step())
      result.custom_ids.push_back(custom_id(tokens.column_text(0)));
  }
  return result;
}

void finish_claim_without_reward(SqliteConnection &connection,
                                 const ClaimRecord &claim,
                                 const TarotInvocation &call,
                                 const std::string_view event_id,
                                 const std::string_view state,
                                 const std::string_view event_type,
                                 const std::string_view idempotency_key) {
  auto effective_call = call;
  effective_call.now_ms = std::max(call.now_ms, claim.created_at_ms);
  static_cast<void>(detail::insert_event_uncommitted(
      connection, event_for(effective_call, std::string{event_id},
                            std::string{event_type}, "tarot_recovery_claim",
                            claim.claim_id, std::string{idempotency_key},
                            {{"claim_type", recovery_kind_name(claim.kind)},
                             {"is_test", claim.is_test}})));
  auto update = connection.prepare(
      "UPDATE tarot_recovery_claim SET state = ?, event_id = ?, "
      "completion_idempotency_key = ?, completed_at_ms = ? "
      "WHERE claim_id = ? AND state = 'pending'");
  update.bind(1, state);
  update.bind(2, event_id);
  update.bind(3, idempotency_key);
  update.bind(4, effective_call.now_ms);
  update.bind(5, claim.claim_id);
  update.execute();
}

[[nodiscard]] std::size_t scalar_count(SqliteConnection &connection,
                                       const std::string_view sql) {
  auto query = connection.prepare(sql);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot invariant query failed."};
  const auto value = query.column_int64(0);
  if (value < 0)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot invariant count is invalid."};
  return static_cast<std::size_t>(value);
}

[[nodiscard]] TarotHistoryPage page_status(const TarotPageStatus status) {
  return TarotHistoryPage{.status = status,
                          .entries = {},
                          .offset = 0,
                          .total = 0,
                          .previous_custom_id = std::nullopt,
                          .next_custom_id = std::nullopt};
}

[[nodiscard]] TarotRecoveryResult recovery_status(
    const TarotRecoveryStatus status,
    const TarotRecoveryKind kind = TarotRecoveryKind::grace,
    const TarotVisibility visibility = TarotVisibility::public_result,
    const std::int64_t balance = 0,
    const std::optional<std::int64_t> cooldown = std::nullopt) {
  return TarotRecoveryResult{.status = status,
                             .kind = kind,
                             .visibility = visibility,
                             .claim_id = {},
                             .balance = balance,
                             .reward = std::nullopt,
                             .cooldown_until_ms = cooldown,
                             .prompt_variant = std::nullopt,
                             .custom_ids = {},
                             .mutation_created = false,
                             .public_delivery_created = false,
                             .committed_event_types = {}};
}

[[nodiscard]] TarotMutationResult
mutation_status(const TarotMutationStatus status, const std::int64_t balance) {
  return TarotMutationResult{
      .status = status, .transaction_id = {}, .balance = balance};
}

} // namespace

SqliteTarotRepository::SqliteTarotRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite repository context is required."};
}

void SqliteTarotRepository::initialize_system_accounts(
    const std::vector<std::string> &account_ids, const std::int64_t now_ms) {
  if (account_ids.size() != system_kinds.size() || now_ms < 0 ||
      std::ranges::any_of(
          account_ids, [](const auto &id) { return !valid_uuid_v4(id); }))
    throw std::invalid_argument{"Tarot system account request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  for (std::size_t index = 0; index < system_kinds.size(); ++index) {
    auto insert =
        context_->connection().prepare("INSERT INTO tarot_account "
                                       "(account_id, account_kind, user_id, "
                                       "created_at_ms) VALUES (?, ?, NULL, ?) "
                                       "ON CONFLICT DO NOTHING");
    insert.bind(1, account_ids[index]);
    insert.bind(2, system_kinds[index]);
    insert.bind(3, now_ms);
    insert.execute();
  }
  for (const auto kind : system_kinds)
    static_cast<void>(system_account(context_->connection(), kind));
  transaction.commit();
}

TarotAccountProvisionResult SqliteTarotRepository::ensure_account(
    const TarotAccountProvisionRequest &request) {
  if (!request.invocation.user_id.is_set() || request.starting_fate < 1 ||
      request.starting_fate > 1'000'000'000 ||
      !valid_uuid_v4(request.account_id) ||
      !valid_uuid_v4(request.transaction_id) ||
      !valid_uuid_v4(request.event_id) ||
      !valid_uuid_v4(request.mint_posting_id) ||
      !valid_uuid_v4(request.human_posting_id))
    throw std::invalid_argument{"Tarot account provision request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  if (const auto existing =
          account_for_user(connection, request.invocation.user_id)) {
    const auto current = balance_for_account(connection, *existing);
    transaction.commit();
    return {.account_id = *existing, .balance = current, .created = false};
  }

  auto account =
      connection.prepare("INSERT INTO tarot_account "
                         "(account_id, account_kind, user_id, created_at_ms) "
                         "VALUES (?, 'HUMAN', ?, ?)");
  account.bind(1, request.account_id);
  account.bind(2, request.invocation.user_id.str());
  account.bind(3, request.invocation.now_ms);
  account.execute();

  const auto idempotency =
      "tarot.starting_grant:" + request.invocation.user_id.str();
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, request.event_id, "tarot.starting_grant.v1",
                "tarot_account", request.account_id, idempotency,
                {{"amount", request.starting_fate}, {"currency", "Fate"}})));
  insert_transaction(connection, request.transaction_id, "STARTING_GRANT",
                     request.event_id, idempotency, request.invocation.user_id,
                     std::nullopt, false, std::nullopt,
                     request.invocation.now_ms);
  const auto mint = system_account(connection, "MINT");
  insert_posting(connection, request.mint_posting_id, request.transaction_id,
                 mint, -request.starting_fate, request.invocation.now_ms);
  insert_posting(connection, request.human_posting_id, request.transaction_id,
                 request.account_id, request.starting_fate,
                 request.invocation.now_ms);
  seal_transaction(connection, request.transaction_id,
                   request.invocation.now_ms);
  transaction.commit();
  return {.account_id = request.account_id,
          .balance = request.starting_fate,
          .created = true};
}

std::int64_t SqliteTarotRepository::balance(const DiscordSnowflake &user_id) {
  const std::scoped_lock lock{context_->mutex()};
  const auto account = account_for_user(context_->connection(), user_id);
  if (!account)
    throw std::invalid_argument{"Tarot account does not exist."};
  return balance_for_account(context_->connection(), *account);
}

TarotHistoryPage SqliteTarotRepository::create_history_snapshot(
    const TarotHistorySnapshotRequest &request) {
  if (!valid_uuid_v4(request.cursor_id) ||
      request.page_token_ids.size() != 10 ||
      std::ranges::any_of(request.page_token_ids,
                          [](const auto &id) { return !valid_uuid_v4(id); }))
    throw std::invalid_argument{"Tarot history snapshot request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto cancel_expired = connection.prepare(
      "UPDATE interaction_token SET state = 'cancelled' "
      "WHERE entity_type = 'tarot_history_cursor' AND state = 'active' "
      "AND expires_at_ms <= ?");
  cancel_expired.bind(1, request.invocation.now_ms);
  cancel_expired.execute();
  auto purge_expired = connection.prepare(
      "DELETE FROM tarot_history_cursor WHERE expires_at_ms <= ?");
  purge_expired.bind(1, request.invocation.now_ms);
  purge_expired.execute();
  const auto account = account_for_user(connection, request.invocation.user_id);
  if (!account)
    throw std::invalid_argument{"Tarot account does not exist."};

  std::vector<TarotHistoryEntry> entries;
  auto history = connection.prepare(
      "SELECT tx.transaction_id, tx.ledger_sequence, tx.transaction_type, "
      "posting.amount, tx.committed_at_ms, tx.is_test, tx.reason "
      "FROM tarot_posting posting JOIN tarot_transaction tx "
      "ON tx.transaction_id = posting.transaction_id "
      "WHERE posting.account_id = ? AND tx.state = 'committed' "
      "ORDER BY tx.ledger_sequence");
  history.bind(1, *account);
  std::int64_t running{};
  while (history.step()) {
    running = checked_add(running, history.column_int64(3));
    entries.push_back(TarotHistoryEntry{
        .transaction_id = history.column_text(0),
        .ledger_sequence = history.column_int64(1),
        .transaction_type = history.column_text(2),
        .amount = history.column_int64(3),
        .balance_after = running,
        .occurred_at_ms = history.column_int64(4),
        .is_test = history.column_int64(5) != 0,
        .reason = history.column_is_null(6)
                      ? std::nullopt
                      : std::optional<std::string>{history.column_text(6)},
    });
  }
  std::ranges::reverse(entries);
  if (entries.size() > tarot_history_maximum_items)
    entries.resize(tarot_history_maximum_items);

  auto cursor = connection.prepare(
      "INSERT INTO tarot_history_cursor "
      "(cursor_id, account_id, item_count, created_at_ms, expires_at_ms) "
      "VALUES (?, ?, ?, ?, ?)");
  cursor.bind(1, request.cursor_id);
  cursor.bind(2, *account);
  cursor.bind(3, static_cast<std::int64_t>(entries.size()));
  cursor.bind(4, request.invocation.now_ms);
  cursor.bind(
      5, checked_add(request.invocation.now_ms, tarot_interaction_lifetime_ms));
  cursor.execute();
  for (std::size_t index = 0; index < entries.size(); ++index) {
    auto item = connection.prepare(
        "INSERT INTO tarot_history_item (cursor_id, position, transaction_id) "
        "VALUES (?, ?, ?)");
    item.bind(1, request.cursor_id);
    item.bind(2, static_cast<std::int64_t>(index));
    item.bind(3, entries[index].transaction_id);
    item.execute();
  }

  const auto page_count =
      std::max<std::size_t>(1, (entries.size() + tarot_history_page_size - 1) /
                                   tarot_history_page_size);
  for (std::size_t page = 0; page < page_count; ++page) {
    const auto offset = page * tarot_history_page_size;
    auto token = connection.prepare(
        "INSERT INTO interaction_token "
        "(token_id, token_version, interaction_kind, action, entity_type, "
        "entity_id, expected_user_id, guild_id, channel_id, state, "
        "expires_at_ms, idempotency_key, created_at_ms) "
        "VALUES (?, 1, 'button', ?, 'tarot_history_cursor', ?, ?, ?, ?, "
        "'active', ?, ?, ?)");
    token.bind(1, request.page_token_ids[page]);
    token.bind(2, "tarot.history." + std::to_string(offset));
    token.bind(3, request.cursor_id);
    token.bind(4, request.invocation.user_id.str());
    token.bind(5, request.invocation.guild_id.str());
    token.bind(6, request.invocation.channel_id.str());
    token.bind(7, checked_add(request.invocation.now_ms,
                              tarot_interaction_lifetime_ms));
    token.bind(8, "tarot.history_token:" + request.cursor_id + ":" +
                      std::to_string(offset));
    token.bind(9, request.invocation.now_ms);
    token.execute();
  }
  transaction.commit();

  TarotHistoryPage result{.status = TarotPageStatus::available,
                          .entries = {},
                          .offset = 0,
                          .total = entries.size(),
                          .previous_custom_id = std::nullopt,
                          .next_custom_id = std::nullopt};
  const auto first_count = std::min(entries.size(), tarot_history_page_size);
  result.entries.assign(entries.begin(),
                        entries.begin() +
                            static_cast<std::ptrdiff_t>(first_count));
  if (entries.size() > tarot_history_page_size)
    result.next_custom_id = custom_id(request.page_token_ids[1]);
  return result;
}

TarotHistoryPage
SqliteTarotRepository::history_page(const TarotHistoryPageRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto token = connection.prepare(
      "SELECT action, entity_id, expected_user_id, guild_id, channel_id, "
      "state, expires_at_ms FROM interaction_token "
      "WHERE token_id = ? AND entity_type = 'tarot_history_cursor'");
  token.bind(1, request.token_id);
  if (!token.step())
    return page_status(TarotPageStatus::invalid_token);
  const auto action = token.column_text(0);
  const auto cursor_id = token.column_text(1);
  if (token.column_text(2) != request.invocation.user_id.str())
    return page_status(TarotPageStatus::wrong_user);
  if (token.column_text(3) != request.invocation.guild_id.str() ||
      token.column_text(4) != request.invocation.channel_id.str())
    return page_status(TarotPageStatus::wrong_scope);
  if (token.column_text(5) != "active" ||
      token.column_int64(6) <= request.invocation.now_ms)
    return page_status(TarotPageStatus::expired);
  constexpr std::string_view prefix{"tarot.history."};
  if (!action.starts_with(prefix))
    return page_status(TarotPageStatus::invalid_token);
  std::size_t offset{};
  try {
    const auto parsed = std::stoul(action.substr(prefix.size()));
    if (parsed > tarot_history_maximum_items ||
        parsed % tarot_history_page_size != 0)
      return page_status(TarotPageStatus::invalid_token);
    offset = parsed;
  } catch (...) {
    return page_status(TarotPageStatus::invalid_token);
  }

  auto cursor = connection.prepare(
      "SELECT item_count, expires_at_ms FROM tarot_history_cursor "
      "WHERE cursor_id = ?");
  cursor.bind(1, cursor_id);
  if (!cursor.step() || cursor.column_int64(1) <= request.invocation.now_ms)
    return page_status(TarotPageStatus::expired);
  const auto total = static_cast<std::size_t>(cursor.column_int64(0));
  if (offset >= total)
    return page_status(TarotPageStatus::invalid_token);

  TarotHistoryPage result{.status = TarotPageStatus::available,
                          .entries = {},
                          .offset = offset,
                          .total = total,
                          .previous_custom_id = std::nullopt,
                          .next_custom_id = std::nullopt};
  auto items = connection.prepare(
      "SELECT tx.transaction_id, tx.ledger_sequence, tx.transaction_type, "
      "posting.amount, tx.committed_at_ms, tx.is_test, tx.reason, "
      "item.position FROM tarot_history_item item "
      "JOIN tarot_transaction tx ON tx.transaction_id = item.transaction_id "
      "JOIN tarot_history_cursor cursor ON cursor.cursor_id = item.cursor_id "
      "JOIN tarot_posting posting ON posting.transaction_id = "
      "tx.transaction_id "
      "AND posting.account_id = cursor.account_id "
      "WHERE item.cursor_id = ? AND item.position >= ? AND item.position < ? "
      "ORDER BY item.position");
  items.bind(1, cursor_id);
  items.bind(2, static_cast<std::int64_t>(offset));
  items.bind(3, static_cast<std::int64_t>(offset + tarot_history_page_size));
  while (items.step()) {
    const auto sequence = items.column_int64(1);
    auto running_query = connection.prepare(
        "SELECT posting.amount FROM tarot_posting posting "
        "JOIN tarot_transaction tx ON tx.transaction_id = "
        "posting.transaction_id "
        "JOIN tarot_history_cursor cursor ON cursor.cursor_id = ? "
        "WHERE posting.account_id = cursor.account_id AND tx.state = "
        "'committed' "
        "AND tx.ledger_sequence <= ? ORDER BY tx.ledger_sequence");
    running_query.bind(1, cursor_id);
    running_query.bind(2, sequence);
    std::int64_t running{};
    while (running_query.step())
      running = checked_add(running, running_query.column_int64(0));
    result.entries.push_back(
        {.transaction_id = items.column_text(0),
         .ledger_sequence = sequence,
         .transaction_type = items.column_text(2),
         .amount = items.column_int64(3),
         .balance_after = running,
         .occurred_at_ms = items.column_int64(4),
         .is_test = items.column_int64(5) != 0,
         .reason = items.column_is_null(6)
                       ? std::nullopt
                       : std::optional<std::string>{items.column_text(6)}});
  }
  if (offset > 0) {
    auto previous = connection.prepare(
        "SELECT token_id FROM interaction_token "
        "WHERE entity_type = 'tarot_history_cursor' AND entity_id = ? "
        "AND action = ?");
    previous.bind(1, cursor_id);
    previous.bind(2, "tarot.history." +
                         std::to_string(offset - tarot_history_page_size));
    if (previous.step())
      result.previous_custom_id = custom_id(previous.column_text(0));
  }
  if (offset + tarot_history_page_size < total) {
    auto next = connection.prepare(
        "SELECT token_id FROM interaction_token "
        "WHERE entity_type = 'tarot_history_cursor' AND entity_id = ? "
        "AND action = ?");
    next.bind(1, cursor_id);
    next.bind(2, "tarot.history." +
                     std::to_string(offset + tarot_history_page_size));
    if (next.step())
      result.next_custom_id = custom_id(next.column_text(0));
  }
  return result;
}

std::vector<TarotStanding> SqliteTarotRepository::standings() {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  std::vector<TarotStanding> result;
  auto accounts = connection.prepare(
      "SELECT account.account_id, account.user_id, "
      "COALESCE(user.display_name_cache, user.username_cache, account.user_id) "
      "FROM tarot_account account JOIN discord_user user "
      "ON user.user_id = account.user_id JOIN user_preference preference "
      "ON preference.user_id = account.user_id "
      "WHERE account.account_kind = 'HUMAN' "
      "AND preference.public_tarot_results_opt_in = 1");
  while (accounts.step()) {
    result.push_back(
        {.user_id = DiscordSnowflake::parse(accounts.column_text(1)),
         .display_name = accounts.column_text(2),
         .balance = balance_for_account(connection, accounts.column_text(0))});
  }
  std::ranges::sort(result, [](const auto &left, const auto &right) {
    if (left.balance != right.balance)
      return left.balance > right.balance;
    const auto left_id = left.user_id.str();
    const auto right_id = right.user_id.str();
    if (left_id.size() != right_id.size())
      return left_id.size() < right_id.size();
    return left_id < right_id;
  });
  return result;
}

TarotVisibilityResult SqliteTarotRepository::set_standings_visibility(
    const TarotVisibilityRequest &request) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto idempotency =
      request.invocation.interaction_idempotency_key + ":standings_visibility";
  const auto account = account_for_user(connection, request.invocation.user_id);
  if (!account)
    throw std::invalid_argument{"Tarot preference account does not exist."};
  const nlohmann::json receipt_request{
      {"user_id", request.invocation.user_id.str()},
      {"guild_id", request.invocation.guild_id.str()},
      {"channel_id", request.invocation.channel_id.str()},
      {"public", request.public_standings}};
  if (const auto replay = load_receipt(connection, idempotency)) {
    if (replay->operation != "standings_visibility" ||
        replay->account_id != *account || replay->request != receipt_request ||
        replay->claim_id || replay->transaction_id ||
        !replay->result.contains("public") ||
        !replay->result.at("public").is_boolean() ||
        !replay->result.contains("changed") ||
        !replay->result.at("changed").is_boolean())
      receipt_conflict();
    const auto stored_public = replay->result.at("public").get<bool>();
    transaction.commit();
    return {.public_standings = stored_public, .changed = false};
  }
  auto current = connection.prepare("SELECT public_tarot_results_opt_in FROM "
                                    "user_preference WHERE user_id = ?");
  current.bind(1, request.invocation.user_id.str());
  if (!current.step())
    throw std::invalid_argument{"Tarot preference account does not exist."};
  const bool previous = current.column_int64(0) != 0;
  if (previous == request.public_standings) {
    insert_receipt(connection, idempotency, "standings_visibility", *account,
                   receipt_request, {{"public", previous}, {"changed", false}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return {.public_standings = previous, .changed = false};
  }
  if (!detail::insert_event_uncommitted(
          connection, event_for(request.invocation, request.event_id,
                                "tarot.standings_visibility_changed.v1",
                                "tarot_account", *account, idempotency,
                                {{"public", request.public_standings}})))
    throw DatabaseError{
        DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT, SQLITE_CONSTRAINT,
        "Tarot standings visibility event idempotency conflict."};
  auto update = connection.prepare(
      "UPDATE user_preference SET public_tarot_results_opt_in = ?, "
      "updated_at_ms = ? WHERE user_id = ?");
  update.bind(1, request.public_standings ? 1 : 0);
  update.bind(2, request.invocation.now_ms);
  update.bind(3, request.invocation.user_id.str());
  update.execute();
  insert_receipt(connection, idempotency, "standings_visibility", *account,
                 receipt_request,
                 {{"public", request.public_standings}, {"changed", true}},
                 std::nullopt, std::nullopt, request.invocation.now_ms);
  transaction.commit();
  return {.public_standings = request.public_standings, .changed = true};
}

bool SqliteTarotRepository::standings_visibility(
    const DiscordSnowflake &user_id) {
  const std::scoped_lock lock{context_->mutex()};
  auto query =
      context_->connection().prepare("SELECT public_tarot_results_opt_in FROM "
                                     "user_preference WHERE user_id = ?");
  query.bind(1, user_id.str());
  if (!query.step())
    return true;
  return query.column_int64(0) != 0;
}

TarotRecoveryResult SqliteTarotRepository::start_recovery(
    const TarotRecoveryStartRequest &request) {
  const auto trial = request.kind == TarotRecoveryKind::trial;
  if (!valid_uuid_v4(request.claim_id) ||
      !valid_uuid_v4(request.started_event_id) ||
      !valid_uuid_v4(request.expired_event_id) || request.threshold < 1 ||
      (trial == request.grace_target.has_value()) ||
      (request.grace_target && (*request.grace_target <= request.threshold ||
                                *request.grace_target > 1'000'000'000)) ||
      request.cooldown_ms <= 0 || (trial != request.draw_id.has_value()) ||
      (trial != static_cast<bool>(request.trial_draw)) ||
      (trial ? request.token_ids.size() != 4 : request.token_ids.size() != 1) ||
      (request.draw_id && !valid_uuid_v4(*request.draw_id)) ||
      std::ranges::any_of(
          request.token_ids, [](const auto &id) { return !valid_uuid_v4(id); }))
    throw std::invalid_argument{"Tarot recovery start request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto account = account_for_user(connection, request.invocation.user_id);
  if (!account)
    throw std::invalid_argument{"Tarot account does not exist."};
  const auto start_idempotency =
      request.invocation.interaction_idempotency_key + ":recovery_start";
  const nlohmann::json receipt_request{
      {"user_id", request.invocation.user_id.str()},
      {"guild_id", request.invocation.guild_id.str()},
      {"channel_id", request.invocation.channel_id.str()},
      {"claim_type", recovery_kind_name(request.kind)},
      {"visibility", visibility_name(request.visibility)},
      {"is_test", request.is_test},
      {"threshold", request.threshold},
      {"cooldown_ms", request.cooldown_ms}};
  if (const auto replay = load_receipt(connection, start_idempotency)) {
    if (replay->operation != "recovery_start" ||
        replay->account_id != *account || replay->request != receipt_request ||
        replay->transaction_id || !replay->result.contains("status") ||
        !replay->result.at("status").is_string())
      receipt_conflict();
    const auto status = replay->result.at("status").get<std::string>();
    if (replay->claim_id) {
      if (status != "pending")
        receipt_conflict();
      const auto claim = load_claim(connection, *replay->claim_id);
      if (!claim || claim->account_id != *account)
        throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                            SQLITE_SCHEMA, "Tarot claim replay is orphaned."};
      auto result =
          recovery_result(connection, *claim, stored_recovery_status(*claim));
      transaction.commit();
      return result;
    }
    if (!replay->result.contains("balance") ||
        !replay->result.at("balance").is_number_integer())
      receipt_conflict();
    const auto stored_balance =
        replay->result.at("balance").get<std::int64_t>();
    if (status == "ineligible") {
      auto result =
          recovery_status(TarotRecoveryStatus::ineligible, request.kind,
                          request.visibility, stored_balance);
      transaction.commit();
      return result;
    }
    if (status == "cooldown" && replay->result.contains("cooldown_until_ms") &&
        replay->result.at("cooldown_until_ms").is_number_integer()) {
      auto result = recovery_status(
          TarotRecoveryStatus::cooldown, request.kind, request.visibility,
          stored_balance,
          replay->result.at("cooldown_until_ms").get<std::int64_t>());
      transaction.commit();
      return result;
    }
    receipt_conflict();
  }

  std::vector<std::string> committed_event_types;
  auto pending = connection.prepare(
      "SELECT claim_id FROM tarot_recovery_claim "
      "WHERE account_id = ? AND claim_type = ? AND is_test = ? "
      "AND state = 'pending'");
  pending.bind(1, *account);
  pending.bind(2, recovery_kind_name(request.kind));
  pending.bind(3, request.is_test ? 1 : 0);
  if (pending.step()) {
    auto existing = load_claim(connection, pending.column_text(0));
    if (!existing)
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA, "Active Tarot claim is missing."};
    if (existing->expires_at_ms > request.invocation.now_ms) {
      if (request.visibility == TarotVisibility::private_result &&
          existing->visibility == TarotVisibility::public_result) {
        static_cast<void>(detail::insert_event_uncommitted(
            connection,
            event_for(request.invocation, request.started_event_id,
                      "tarot.recovery_visibility_tightened.v1",
                      "tarot_recovery_claim", existing->claim_id,
                      start_idempotency + ":privacy",
                      {{"claim_type", recovery_kind_name(existing->kind)},
                       {"visibility", "private"},
                       {"is_test", existing->is_test}})));
        auto tighten = connection.prepare(
            "UPDATE tarot_recovery_claim SET visibility = 'private' "
            "WHERE claim_id = ? AND state = 'pending' AND visibility = "
            "'public'");
        tighten.bind(1, existing->claim_id);
        tighten.execute();
        if (connection.changes() != 1)
          throw DatabaseError{DatabaseErrorCategory::constraint,
                              SQLITE_CONSTRAINT, SQLITE_CONSTRAINT,
                              "Tarot recovery privacy could not be tightened."};
        existing = load_claim(connection, existing->claim_id);
        if (!existing)
          throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                              SQLITE_SCHEMA, "Private Tarot claim is missing."};
        committed_event_types.emplace_back(
            "tarot.recovery_visibility_tightened.v1");
      }
      insert_receipt(connection, start_idempotency, "recovery_start", *account,
                     receipt_request, {{"status", "pending"}},
                     existing->claim_id, std::nullopt,
                     request.invocation.now_ms);
      auto result =
          recovery_result(connection, *existing, TarotRecoveryStatus::pending);
      result.mutation_created = !committed_event_types.empty();
      result.committed_event_types = std::move(committed_event_types);
      transaction.commit();
      return result;
    }
    finish_claim_without_reward(connection, *existing, request.invocation,
                                request.expired_event_id, "expired",
                                "tarot.recovery_expired.v1",
                                "tarot.recovery_expired:" + existing->claim_id);
    committed_event_types.emplace_back("tarot.recovery_expired.v1");
  }

  const auto current_balance = balance_for_account(connection, *account);
  if (current_balance >= request.threshold) {
    insert_receipt(connection, start_idempotency, "recovery_start", *account,
                   receipt_request,
                   {{"status", "ineligible"}, {"balance", current_balance}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    auto result = recovery_status(TarotRecoveryStatus::ineligible, request.kind,
                                  request.visibility, current_balance);
    result.mutation_created = !committed_event_types.empty();
    result.committed_event_types = std::move(committed_event_types);
    transaction.commit();
    return result;
  }
  auto cooldown = connection.prepare(
      "SELECT cooldown_until_ms FROM tarot_recovery_claim "
      "WHERE account_id = ? AND claim_type = ? AND is_test = ? "
      "AND state = 'completed' ORDER BY cooldown_until_ms DESC LIMIT 1");
  cooldown.bind(1, *account);
  cooldown.bind(2, recovery_kind_name(request.kind));
  cooldown.bind(3, request.is_test ? 1 : 0);
  if (cooldown.step() && cooldown.column_int64(0) > request.invocation.now_ms) {
    const auto cooldown_until_ms = cooldown.column_int64(0);
    insert_receipt(connection, start_idempotency, "recovery_start", *account,
                   receipt_request,
                   {{"status", "cooldown"},
                    {"balance", current_balance},
                    {"cooldown_until_ms", cooldown_until_ms}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    auto result =
        recovery_status(TarotRecoveryStatus::cooldown, request.kind,
                        request.visibility, current_balance, cooldown_until_ms);
    result.mutation_created = !committed_event_types.empty();
    result.committed_event_types = std::move(committed_event_types);
    transaction.commit();
    return result;
  }

  std::optional<TarotTrialDraw> trial_draw;
  if (trial) {
    trial_draw = request.trial_draw();
    if (trial_draw->reward < 1 || trial_draw->reward > 1'000'000'000 ||
        trial_draw->prompt_variant < 0 || trial_draw->prompt_variant > 2)
      throw std::invalid_argument{"Tarot Trial draw is invalid."};
  }

  const auto start_event_type =
      trial ? "tarot.trial_started.v1" : "tarot.grace_started.v1";
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, request.started_event_id, start_event_type,
                "tarot_recovery_claim", request.claim_id, start_idempotency,
                {{"claim_type", recovery_kind_name(request.kind)},
                 {"visibility", visibility_name(request.visibility)},
                 {"is_test", request.is_test}})));
  if (trial) {
    auto draw = connection.prepare(
        "INSERT INTO tarot_draw "
        "(draw_id, claim_id, prompt_variant, reward, created_at_ms) "
        "VALUES (?, ?, ?, ?, ?)");
    draw.bind(1, *request.draw_id);
    draw.bind(2, request.claim_id);
    draw.bind(3, trial_draw->prompt_variant);
    draw.bind(4, trial_draw->reward);
    draw.bind(5, request.invocation.now_ms);
    draw.execute();
  }
  auto claim = connection.prepare(
      "INSERT INTO tarot_recovery_claim "
      "(claim_id, account_id, claim_type, state, visibility, is_test, "
      "eligibility_threshold, grace_target, eligibility_balance, reward, "
      "draw_id, "
      "started_event_id, start_idempotency_key, created_at_ms, expires_at_ms) "
      "VALUES (?, ?, ?, 'pending', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  claim.bind(1, request.claim_id);
  claim.bind(2, *account);
  claim.bind(3, recovery_kind_name(request.kind));
  claim.bind(4, visibility_name(request.visibility));
  claim.bind(5, request.is_test ? 1 : 0);
  claim.bind(6, request.threshold);
  if (request.grace_target)
    claim.bind(7, *request.grace_target);
  else
    claim.bind_null(7);
  claim.bind(8, current_balance);
  if (trial_draw)
    claim.bind(9, trial_draw->reward);
  else
    claim.bind_null(9);
  if (request.draw_id)
    claim.bind(10, *request.draw_id);
  else
    claim.bind_null(10);
  claim.bind(11, request.started_event_id);
  claim.bind(12, start_idempotency);
  claim.bind(13, request.invocation.now_ms);
  claim.bind(14, checked_add(request.invocation.now_ms,
                             tarot_interaction_lifetime_ms));
  claim.execute();

  static constexpr std::array<std::string_view, 4> trial_actions{
      "tarot.recovery.0.vow", "tarot.recovery.1.vow", "tarot.recovery.2.vow",
      "tarot.recovery.3.abandon"};
  for (std::size_t index = 0; index < request.token_ids.size(); ++index) {
    const auto action = trial ? trial_actions[index]
                              : std::string_view{"tarot.recovery.0.claim"};
    auto token = connection.prepare(
        "INSERT INTO interaction_token "
        "(token_id, token_version, interaction_kind, action, entity_type, "
        "entity_id, expected_user_id, guild_id, channel_id, state, "
        "expires_at_ms, idempotency_key, created_at_ms) "
        "VALUES (?, 1, 'button', ?, 'tarot_recovery_claim', ?, ?, ?, ?, "
        "'active', ?, ?, ?)");
    token.bind(1, request.token_ids[index]);
    token.bind(2, action);
    token.bind(3, request.claim_id);
    token.bind(4, request.invocation.user_id.str());
    token.bind(5, request.invocation.guild_id.str());
    token.bind(6, request.invocation.channel_id.str());
    token.bind(7, checked_add(request.invocation.now_ms,
                              tarot_interaction_lifetime_ms));
    token.bind(8, "tarot.recovery_token:" + request.claim_id + ":" +
                      std::to_string(index));
    token.bind(9, request.invocation.now_ms);
    token.execute();
  }
  auto created = load_claim(connection, request.claim_id);
  if (!created)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Created Tarot claim is missing."};
  insert_receipt(connection, start_idempotency, "recovery_start", *account,
                 receipt_request, {{"status", "pending"}}, request.claim_id,
                 std::nullopt, request.invocation.now_ms);
  committed_event_types.push_back(start_event_type);
  auto result =
      recovery_result(connection, *created, TarotRecoveryStatus::pending, true,
                      false, std::move(committed_event_types));
  transaction.commit();
  return result;
}

TarotRecoveryResult SqliteTarotRepository::complete_recovery(
    const TarotRecoveryCompleteRequest &request) {
  if (!valid_uuid_v4(request.token_id) ||
      !valid_uuid_v4(request.transaction_id) ||
      !valid_uuid_v4(request.event_id) ||
      !valid_uuid_v4(request.mint_posting_id) ||
      !valid_uuid_v4(request.human_posting_id) ||
      !valid_uuid_v4(request.outbox_id) ||
      request.provider_nonce.size() != 25 || request.grace_cooldown_ms <= 0 ||
      request.trial_cooldown_ms <= 0)
    throw std::invalid_argument{
        "Tarot recovery completion request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto token = connection.prepare(
      "SELECT action, entity_id, expected_user_id, guild_id, channel_id, "
      "expires_at_ms FROM interaction_token WHERE token_id = ? "
      "AND entity_type = 'tarot_recovery_claim'");
  token.bind(1, request.token_id);
  if (!token.step())
    return recovery_status(TarotRecoveryStatus::invalid_token);
  const auto action = token.column_text(0);
  const auto claim_id = token.column_text(1);
  if (token.column_text(2) != request.invocation.user_id.str())
    return recovery_status(TarotRecoveryStatus::wrong_user);
  if (token.column_text(3) != request.invocation.guild_id.str() ||
      token.column_text(4) != request.invocation.channel_id.str())
    return recovery_status(TarotRecoveryStatus::wrong_scope);
  auto claim = load_claim(connection, claim_id);
  if (!claim)
    return recovery_status(TarotRecoveryStatus::invalid_token);
  if (claim->state != "pending") {
    auto result =
        recovery_result(connection, *claim, stored_recovery_status(*claim));
    transaction.commit();
    return result;
  }
  if (token.column_int64(5) <= request.invocation.now_ms ||
      claim->expires_at_ms <= request.invocation.now_ms) {
    finish_claim_without_reward(connection, *claim, request.invocation,
                                request.event_id, "expired",
                                "tarot.recovery_expired.v1",
                                "tarot.recovery_expired:" + claim->claim_id);
    auto updated = load_claim(connection, claim->claim_id);
    auto result =
        recovery_result(connection, *updated, stored_recovery_status(*updated),
                        true, false, {"tarot.recovery_expired.v1"});
    transaction.commit();
    return result;
  }
  const bool abandon = action == "tarot.recovery.3.abandon";
  const bool valid_grace = claim->kind == TarotRecoveryKind::grace &&
                           action == "tarot.recovery.0.claim";
  const bool valid_trial =
      claim->kind == TarotRecoveryKind::trial &&
      (action == "tarot.recovery.0.vow" || action == "tarot.recovery.1.vow" ||
       action == "tarot.recovery.2.vow" || abandon);
  if (!valid_grace && !valid_trial)
    return recovery_status(TarotRecoveryStatus::invalid_token);
  if (abandon) {
    finish_claim_without_reward(connection, *claim, request.invocation,
                                request.event_id, "abandoned",
                                "tarot.trial_abandoned.v1",
                                "tarot.recovery_abandoned:" + claim->claim_id);
    auto updated = load_claim(connection, claim->claim_id);
    auto result =
        recovery_result(connection, *updated, stored_recovery_status(*updated),
                        true, false, {"tarot.trial_abandoned.v1"});
    transaction.commit();
    return result;
  }

  const auto current_balance =
      balance_for_account(connection, claim->account_id);
  auto cooldown = connection.prepare(
      "SELECT cooldown_until_ms FROM tarot_recovery_claim "
      "WHERE account_id = ? AND claim_type = ? AND is_test = ? "
      "AND state = 'completed' ORDER BY cooldown_until_ms DESC LIMIT 1");
  cooldown.bind(1, claim->account_id);
  cooldown.bind(2, recovery_kind_name(claim->kind));
  cooldown.bind(3, claim->is_test ? 1 : 0);
  const bool cooldown_active =
      cooldown.step() && cooldown.column_int64(0) > request.invocation.now_ms;
  if (current_balance >= claim->threshold || cooldown_active) {
    finish_claim_without_reward(connection, *claim, request.invocation,
                                request.event_id, "abandoned",
                                "tarot.recovery_eligibility_lost.v1",
                                "tarot.recovery_lost:" + claim->claim_id);
    auto updated = load_claim(connection, claim->claim_id);
    auto result =
        recovery_result(connection, *updated, stored_recovery_status(*updated),
                        true, false, {"tarot.recovery_eligibility_lost.v1"});
    transaction.commit();
    return result;
  }

  if (claim->kind == TarotRecoveryKind::grace && !claim->grace_target)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot Grace claim is missing its policy target."};
  const auto reward = claim->kind == TarotRecoveryKind::grace
                          ? *claim->grace_target - current_balance
                          : *claim->reward;
  if (reward <= 0 || reward > 1'000'000'000)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT, "Tarot recovery reward is invalid."};
  const auto event_type = claim->kind == TarotRecoveryKind::grace
                              ? "tarot.grace_completed.v1"
                              : "tarot.trial_completed.v1";
  const auto idempotency = "tarot.recovery_complete:" + claim->claim_id;
  auto effective_invocation = request.invocation;
  effective_invocation.now_ms =
      std::max(request.invocation.now_ms, claim->created_at_ms);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(effective_invocation, request.event_id, event_type,
                "tarot_recovery_claim", claim->claim_id, idempotency,
                {{"claim_type", recovery_kind_name(claim->kind)},
                 {"is_test", claim->is_test},
                 {"visibility", visibility_name(claim->visibility)}})));
  insert_transaction(
      connection, request.transaction_id,
      claim->kind == TarotRecoveryKind::grace ? "GRACE" : "TRIAL",
      request.event_id, idempotency, effective_invocation.user_id, std::nullopt,
      claim->is_test, std::nullopt, effective_invocation.now_ms);
  insert_posting(connection, request.mint_posting_id, request.transaction_id,
                 system_account(connection, "MINT"), -reward,
                 effective_invocation.now_ms);
  insert_posting(connection, request.human_posting_id, request.transaction_id,
                 claim->account_id, reward, effective_invocation.now_ms);
  seal_transaction(connection, request.transaction_id,
                   effective_invocation.now_ms);

  bool public_created{};
  if (claim->visibility == TarotVisibility::public_result) {
    auto public_message = text_message(
        std::string{claim->is_test ? "[TEST] " : ""} +
        request.invocation.display_name + " completed " +
        (claim->kind == TarotRecoveryKind::grace ? "Grace of the Throne."
                                                 : "a Trial of Renewal."));
    public_message.allowed_user_mentions.clear();
    const OutboxEnqueue outbox{
        .outbox_id = request.outbox_id,
        .kind = std::string{public_discord_outbox_kind},
        .aggregate_type = "tarot_recovery_claim",
        .aggregate_id = claim->claim_id,
        .target_guild_id = request.invocation.guild_id,
        .target_channel_id = request.invocation.channel_id,
        .target_user_id = std::nullopt,
        .available_at_ms = effective_invocation.now_ms,
        .max_attempts = 5,
        .idempotency_key = "tarot.recovery_public:" + claim->claim_id,
        .provider_nonce = request.provider_nonce,
        .created_at_ms = effective_invocation.now_ms,
    };
    const PublicOutboxPayload payload{
        .request =
            PublicMessageRequest{.guild_id = request.invocation.guild_id,
                                 .channel_id = request.invocation.channel_id,
                                 .message = std::move(public_message)},
        .fail_before_first_send = false,
    };
    public_created = detail::insert_outbox_uncommitted(
        connection, outbox,
        detail::encode_public_payload(
            payload, request.invocation.correlation_id, request.event_id));
  }
  const auto cooldown_ms = claim->kind == TarotRecoveryKind::grace
                               ? request.grace_cooldown_ms
                               : request.trial_cooldown_ms;
  if (effective_invocation.now_ms >
      std::numeric_limits<std::int64_t>::max() - cooldown_ms)
    throw std::overflow_error{"Tarot cooldown timestamp overflowed."};
  auto update = connection.prepare(
      "UPDATE tarot_recovery_claim SET state = 'completed', reward = ?, "
      "transaction_id = ?, event_id = ?, outbox_id = ?, "
      "completion_idempotency_key = ?, completed_at_ms = ?, "
      "cooldown_until_ms = ? WHERE claim_id = ? AND state = 'pending'");
  update.bind(1, reward);
  update.bind(2, request.transaction_id);
  update.bind(3, request.event_id);
  if (claim->visibility == TarotVisibility::public_result)
    update.bind(4, request.outbox_id);
  else
    update.bind_null(4);
  update.bind(5, idempotency);
  update.bind(6, effective_invocation.now_ms);
  update.bind(7, effective_invocation.now_ms + cooldown_ms);
  update.bind(8, claim->claim_id);
  update.execute();
  auto updated = load_claim(connection, claim->claim_id);
  auto result =
      recovery_result(connection, *updated, TarotRecoveryStatus::completed,
                      true, public_created, {event_type});
  transaction.commit();
  return result;
}

TarotMutationResult
SqliteTarotRepository::adjust(const TarotAdjustmentRequest &request) {
  if (request.amount == 0 || request.amount < -1'000'000'000 ||
      request.amount > 1'000'000'000 || blank_reason(request.reason) ||
      request.reason.size() > 200 || !valid_uuid_v4(request.transaction_id) ||
      !valid_uuid_v4(request.event_id) ||
      !valid_uuid_v4(request.system_posting_id) ||
      !valid_uuid_v4(request.human_posting_id))
    throw std::invalid_argument{"Tarot adjustment request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto idempotency =
      request.invocation.interaction_idempotency_key + ":adjust";
  const auto account = account_for_user(connection, request.invocation.user_id);
  if (!account)
    throw std::invalid_argument{"Tarot account does not exist."};
  const nlohmann::json receipt_request{
      {"user_id", request.invocation.user_id.str()},
      {"guild_id", request.invocation.guild_id.str()},
      {"channel_id", request.invocation.channel_id.str()},
      {"amount", request.amount},
      {"reason", request.reason}};
  if (const auto replay = load_receipt(connection, idempotency)) {
    if (replay->operation != "adjust" || replay->account_id != *account ||
        replay->request != receipt_request || replay->claim_id ||
        !replay->result.contains("status") ||
        !replay->result.at("status").is_string() ||
        !replay->result.contains("balance") ||
        !replay->result.at("balance").is_number_integer())
      receipt_conflict();
    const auto status = replay->result.at("status").get<std::string>();
    const auto stored_balance =
        replay->result.at("balance").get<std::int64_t>();
    if (replay->transaction_id && status == "applied") {
      transaction.commit();
      return {.status = TarotMutationStatus::unchanged,
              .transaction_id = *replay->transaction_id,
              .balance = stored_balance};
    }
    if (!replay->transaction_id && status == "would_overdraw") {
      transaction.commit();
      return mutation_status(TarotMutationStatus::would_overdraw,
                             stored_balance);
    }
    receipt_conflict();
  }
  const auto current = balance_for_account(connection, *account);
  if (request.amount < 0 && current < -request.amount) {
    insert_receipt(connection, idempotency, "adjust", *account, receipt_request,
                   {{"status", "would_overdraw"}, {"balance", current}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return mutation_status(TarotMutationStatus::would_overdraw, current);
  }
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, request.event_id, "tarot.admin_adjusted.v1",
                "tarot_transaction", request.transaction_id, idempotency,
                {{"is_test", true}, {"reason", request.reason}})));
  insert_transaction(connection, request.transaction_id, "TEST_ADJUSTMENT",
                     request.event_id, idempotency, request.invocation.user_id,
                     request.reason, true, std::nullopt,
                     request.invocation.now_ms);
  insert_posting(
      connection, request.system_posting_id, request.transaction_id,
      system_account(connection, request.amount > 0 ? "MINT" : "BURN"),
      -request.amount, request.invocation.now_ms);
  insert_posting(connection, request.human_posting_id, request.transaction_id,
                 *account, request.amount, request.invocation.now_ms);
  seal_transaction(connection, request.transaction_id,
                   request.invocation.now_ms);
  const auto updated = balance_for_account(connection, *account);
  insert_receipt(connection, idempotency, "adjust", *account, receipt_request,
                 {{"status", "applied"}, {"balance", updated}}, std::nullopt,
                 request.transaction_id, request.invocation.now_ms);
  transaction.commit();
  return {.status = TarotMutationStatus::applied,
          .transaction_id = request.transaction_id,
          .balance = updated};
}

TarotMutationResult
SqliteTarotRepository::reverse(const TarotReversalRequest &request) {
  if (!valid_uuid_v4(request.original_transaction_id) ||
      !valid_uuid_v4(request.transaction_id) ||
      !valid_uuid_v4(request.event_id) ||
      !valid_uuid_v4(request.first_posting_id) ||
      !valid_uuid_v4(request.second_posting_id) ||
      blank_reason(request.reason) || request.reason.size() > 200)
    throw std::invalid_argument{"Tarot reversal request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto idempotency =
      request.invocation.interaction_idempotency_key + ":reverse";
  const auto account = account_for_user(connection, request.invocation.user_id);
  if (!account)
    throw std::invalid_argument{"Tarot account does not exist."};
  const nlohmann::json receipt_request{
      {"user_id", request.invocation.user_id.str()},
      {"guild_id", request.invocation.guild_id.str()},
      {"channel_id", request.invocation.channel_id.str()},
      {"original_transaction_id", request.original_transaction_id},
      {"reason", request.reason}};
  if (const auto replay = load_receipt(connection, idempotency)) {
    if (replay->operation != "reverse" || replay->account_id != *account ||
        replay->request != receipt_request || replay->claim_id ||
        !replay->result.contains("status") ||
        !replay->result.at("status").is_string() ||
        !replay->result.contains("balance") ||
        !replay->result.at("balance").is_number_integer())
      receipt_conflict();
    const auto status = replay->result.at("status").get<std::string>();
    const auto stored_balance =
        replay->result.at("balance").get<std::int64_t>();
    if (replay->transaction_id && status == "applied") {
      transaction.commit();
      return {.status = TarotMutationStatus::unchanged,
              .transaction_id = *replay->transaction_id,
              .balance = stored_balance};
    }
    if (!replay->transaction_id) {
      const auto stored_status =
          status == "not_found"        ? TarotMutationStatus::not_found
          : status == "forbidden"      ? TarotMutationStatus::forbidden
          : status == "would_overdraw" ? TarotMutationStatus::would_overdraw
                                       : TarotMutationStatus::applied;
      if (stored_status != TarotMutationStatus::applied) {
        transaction.commit();
        return mutation_status(stored_status, stored_balance);
      }
    }
    receipt_conflict();
  }
  auto original = connection.prepare(
      "SELECT transaction_type, state, is_test, actor_user_id "
      "FROM tarot_transaction WHERE transaction_id = ?");
  original.bind(1, request.original_transaction_id);
  if (!original.step()) {
    const auto current = balance_for_account(connection, *account);
    insert_receipt(connection, idempotency, "reverse", *account,
                   receipt_request,
                   {{"status", "not_found"}, {"balance", current}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return mutation_status(TarotMutationStatus::not_found, current);
  }
  const auto reversible =
      original.column_text(1) == "committed" && original.column_int64(2) == 1 &&
      !original.column_is_null(3) &&
      original.column_text(3) == request.invocation.user_id.str() &&
      original.column_text(0) == "TEST_ADJUSTMENT";
  auto prior_reversal = connection.prepare(
      "SELECT 1 FROM tarot_transaction "
      "WHERE reversal_of_transaction_id = ? AND state = 'committed'");
  prior_reversal.bind(1, request.original_transaction_id);
  if (!reversible || prior_reversal.step()) {
    const auto current = balance_for_account(connection, *account);
    insert_receipt(connection, idempotency, "reverse", *account,
                   receipt_request,
                   {{"status", "forbidden"}, {"balance", current}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return mutation_status(TarotMutationStatus::forbidden, current);
  }

  struct Posting {
    std::string account_id;
    std::int64_t amount{};
    bool human{};
  };
  std::vector<Posting> postings;
  auto load = connection.prepare(
      "SELECT posting.account_id, posting.amount, account.account_kind "
      "FROM tarot_posting posting JOIN tarot_account account "
      "ON account.account_id = posting.account_id "
      "WHERE posting.transaction_id = ? ORDER BY posting.posting_id");
  load.bind(1, request.original_transaction_id);
  while (load.step()) {
    postings.push_back({.account_id = load.column_text(0),
                        .amount = load.column_int64(1),
                        .human = load.column_text(2) == "HUMAN"});
  }
  if (postings.size() != 2 ||
      std::ranges::none_of(postings, [&](const Posting &posting) {
        return posting.human && posting.account_id == *account;
      })) {
    const auto current = balance_for_account(connection, *account);
    insert_receipt(connection, idempotency, "reverse", *account,
                   receipt_request,
                   {{"status", "forbidden"}, {"balance", current}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return mutation_status(TarotMutationStatus::forbidden, current);
  }
  const auto human = std::ranges::find_if(postings, &Posting::human);
  const auto current = balance_for_account(connection, *account);
  const auto inverse_human_amount = -human->amount;
  if (inverse_human_amount < 0 && current < -inverse_human_amount) {
    insert_receipt(connection, idempotency, "reverse", *account,
                   receipt_request,
                   {{"status", "would_overdraw"}, {"balance", current}},
                   std::nullopt, std::nullopt, request.invocation.now_ms);
    transaction.commit();
    return mutation_status(TarotMutationStatus::would_overdraw, current);
  }

  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      event_for(request.invocation, request.event_id,
                "tarot.transaction_reversed.v1", "tarot_transaction",
                request.transaction_id, idempotency,
                {{"is_test", true},
                 {"reversal_of", request.original_transaction_id},
                 {"reason", request.reason}})));
  insert_transaction(connection, request.transaction_id, "TEST_REVERSAL",
                     request.event_id, idempotency, request.invocation.user_id,
                     request.reason, true, request.original_transaction_id,
                     request.invocation.now_ms);
  insert_posting(connection, request.first_posting_id, request.transaction_id,
                 postings[0].account_id, -postings[0].amount,
                 request.invocation.now_ms);
  insert_posting(connection, request.second_posting_id, request.transaction_id,
                 postings[1].account_id, -postings[1].amount,
                 request.invocation.now_ms);
  seal_transaction(connection, request.transaction_id,
                   request.invocation.now_ms);
  const auto updated = balance_for_account(connection, *account);
  insert_receipt(connection, idempotency, "reverse", *account, receipt_request,
                 {{"status", "applied"}, {"balance", updated}}, std::nullopt,
                 request.transaction_id, request.invocation.now_ms);
  transaction.commit();
  return {.status = TarotMutationStatus::applied,
          .transaction_id = request.transaction_id,
          .balance = updated};
}

TarotInvariantReport SqliteTarotRepository::check_invariants() {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  TarotInvariantReport report;
  report.account_count =
      scalar_count(connection, "SELECT count(*) FROM tarot_account");
  report.committed_transaction_count = scalar_count(
      connection,
      "SELECT count(*) FROM tarot_transaction WHERE state = 'committed'");
  report.prepared_transaction_count = scalar_count(
      connection,
      "SELECT count(*) FROM tarot_transaction WHERE state = 'prepared'");
  report.posting_count =
      scalar_count(connection, "SELECT count(*) FROM tarot_posting");
  report.unbalanced_transaction_count = scalar_count(
      connection,
      "SELECT count(*) FROM tarot_transaction tx WHERE tx.state = 'committed' "
      "AND ((SELECT count(*) FROM tarot_posting posting "
      "      WHERE posting.transaction_id = tx.transaction_id) <> "
      "     tx.expected_posting_count "
      "OR (SELECT total(posting.amount) FROM tarot_posting posting "
      "    WHERE posting.transaction_id = tx.transaction_id) <> 0.0)");
  report.illegal_reversal_count = scalar_count(
      connection,
      "SELECT count(*) FROM ("
      "SELECT reversal.transaction_id FROM tarot_transaction reversal "
      "LEFT JOIN tarot_transaction original "
      "ON original.transaction_id = reversal.reversal_of_transaction_id "
      "WHERE reversal.transaction_type = 'TEST_REVERSAL' "
      "AND (original.transaction_id IS NULL OR original.is_test <> 1 "
      "OR original.state <> 'committed' "
      "OR original.transaction_type IN ('STARTING_GRANT', 'TEST_REVERSAL') "
      "OR (original.transaction_type <> 'TEST_ADJUSTMENT' AND NOT EXISTS ("
      "    SELECT 1 FROM tarot_wager_test_cleanup cleanup "
      "    WHERE cleanup.reversal_transaction_id = reversal.transaction_id "
      "      AND cleanup.original_transaction_id = original.transaction_id) "
      "    AND NOT EXISTS (SELECT 1 FROM tarot_house_test_cleanup cleanup "
      "    WHERE cleanup.reversal_transaction_id = reversal.transaction_id "
      "      AND cleanup.original_transaction_id = original.transaction_id)) "
      "OR reversal.expected_posting_count <> original.expected_posting_count "
      "OR (SELECT count(*) FROM tarot_posting original_post "
      "    JOIN tarot_posting inverse_post "
      "      ON inverse_post.transaction_id = reversal.transaction_id "
      "     AND inverse_post.account_id = original_post.account_id "
      "     AND inverse_post.amount = -original_post.amount "
      "    WHERE original_post.transaction_id = original.transaction_id) <> "
      "reversal.expected_posting_count) UNION ALL SELECT transaction_id FROM "
      "tarot_transaction "
      "WHERE transaction_type = 'STARTING_GRANT' AND is_test <> 0)");
  report.claim_mismatch_count = scalar_count(
      connection,
      "SELECT (SELECT count(*) FROM tarot_recovery_claim claim "
      "WHERE (claim.state = 'completed' AND NOT EXISTS ("
      "       SELECT 1 FROM tarot_transaction tx "
      "       JOIN tarot_posting posting "
      "         ON posting.transaction_id = tx.transaction_id "
      "       JOIN tarot_account account "
      "         ON account.account_id = posting.account_id "
      "       WHERE tx.transaction_id = claim.transaction_id "
      "         AND tx.state = 'committed' "
      "         AND tx.transaction_type = claim.claim_type "
      "         AND tx.event_id = claim.event_id "
      "         AND tx.is_test = claim.is_test "
      "         AND posting.account_id = claim.account_id "
      "         AND account.account_kind = 'HUMAN' "
      "         AND posting.amount = claim.reward)) "
      "OR (claim.state = 'completed' AND claim.visibility = 'public' "
      "    AND NOT EXISTS (SELECT 1 FROM outbox_message outbox "
      "      JOIN event_journal started "
      "        ON started.event_id = claim.started_event_id "
      "      WHERE outbox.outbox_id = claim.outbox_id "
      "        AND outbox.kind = 'discord.public.v1' "
      "        AND outbox.aggregate_type = 'tarot_recovery_claim' "
      "        AND outbox.aggregate_id = claim.claim_id "
      "        AND outbox.target_guild_id = started.guild_id "
      "        AND outbox.target_channel_id = started.channel_id "
      "        AND outbox.target_user_id IS NULL)) "
      "OR (claim.state = 'completed' AND claim.visibility = 'private' "
      "    AND claim.outbox_id IS NOT NULL) "
      "OR (claim.state <> 'pending' AND EXISTS "
      "    (SELECT 1 FROM interaction_token token "
      "     WHERE token.entity_type = 'tarot_recovery_claim' "
      "       AND token.entity_id = claim.claim_id AND token.state = "
      "'active')) "
      "OR NOT EXISTS (SELECT 1 FROM event_journal started "
      "    WHERE started.event_id = claim.started_event_id "
      "      AND started.aggregate_type = 'tarot_recovery_claim' "
      "      AND started.aggregate_id = claim.claim_id "
      "      AND started.event_type = CASE claim.claim_type "
      "          WHEN 'GRACE' THEN 'tarot.grace_started.v1' "
      "          ELSE 'tarot.trial_started.v1' END) "
      "OR (claim.state <> 'pending' AND NOT EXISTS ("
      "    SELECT 1 FROM event_journal terminal "
      "    WHERE terminal.event_id = claim.event_id "
      "      AND terminal.aggregate_type = 'tarot_recovery_claim' "
      "      AND terminal.aggregate_id = claim.claim_id "
      "      AND ((claim.state = 'completed' AND terminal.event_type = "
      "            CASE claim.claim_type WHEN 'GRACE' "
      "              THEN 'tarot.grace_completed.v1' "
      "              ELSE 'tarot.trial_completed.v1' END) "
      "        OR (claim.state = 'expired' "
      "            AND terminal.event_type = 'tarot.recovery_expired.v1') "
      "        OR (claim.state = 'abandoned' AND terminal.event_type = "
      "            'tarot.recovery_eligibility_lost.v1') "
      "        OR (claim.state = 'abandoned' AND claim.claim_type = 'TRIAL' "
      "            AND terminal.event_type = "
      "            'tarot.trial_abandoned.v1')))) "
      "OR (claim.claim_type = 'TRIAL' AND NOT EXISTS "
      "    (SELECT 1 FROM tarot_draw draw WHERE draw.draw_id = claim.draw_id "
      "     AND draw.claim_id = claim.claim_id AND draw.reward = "
      "claim.reward))) + "
      "(SELECT count(*) FROM tarot_transaction tx "
      " WHERE tx.state = 'committed' "
      "   AND tx.transaction_type IN ('GRACE', 'TRIAL') "
      "   AND NOT EXISTS ("
      "     SELECT 1 FROM tarot_recovery_claim claim "
      "     JOIN event_journal terminal ON terminal.event_id = tx.event_id "
      "     WHERE claim.transaction_id = tx.transaction_id "
      "       AND claim.state = 'completed' "
      "       AND claim.claim_type = tx.transaction_type "
      "       AND claim.event_id = tx.event_id "
      "       AND claim.is_test = tx.is_test "
      "       AND terminal.aggregate_type = 'tarot_recovery_claim' "
      "       AND terminal.aggregate_id = claim.claim_id "
      "       AND terminal.event_type = CASE tx.transaction_type "
      "         WHEN 'GRACE' THEN 'tarot.grace_completed.v1' "
      "         ELSE 'tarot.trial_completed.v1' END))");
  report.orphaned_link_count = scalar_count(
      connection,
      "SELECT (SELECT count(*) FROM tarot_transaction tx "
      "        LEFT JOIN event_journal event ON event.event_id = tx.event_id "
      "        WHERE event.event_id IS NULL "
      "           OR (tx.transaction_type IN "
      "               ('TEST_ADJUSTMENT', 'TEST_REVERSAL') AND ("
      "             tx.actor_user_id IS NULL OR tx.reason IS NULL "
      "             OR length(trim(tx.reason, char(9) || char(10) || char(11) "
      "                            || char(12) || char(13) || ' ')) = 0 "
      "             OR event.event_type <> CASE tx.transaction_type "
      "                  WHEN 'TEST_ADJUSTMENT' "
      "                    THEN 'tarot.admin_adjusted.v1' "
      "                  ELSE 'tarot.transaction_reversed.v1' END "
      "             OR event.aggregate_type <> 'tarot_transaction' "
      "             OR event.aggregate_id <> tx.transaction_id "
      "             OR event.actor_user_id IS NOT tx.actor_user_id "
      "             OR (NOT EXISTS ("
      "               SELECT 1 FROM tarot_posting human_post "
      "               JOIN tarot_account human_account "
      "                 ON human_account.account_id = human_post.account_id "
      "               WHERE human_post.transaction_id = tx.transaction_id "
      "                 AND human_account.account_kind = 'HUMAN' "
      "                 AND human_account.user_id = tx.actor_user_id) "
      "             AND NOT (tx.transaction_type = 'TEST_REVERSAL' "
      "               AND EXISTS (SELECT 1 FROM tarot_house_test_cleanup "
      "cleanup "
      "               JOIN tarot_house_wager wager "
      "                 ON wager.wager_id = cleanup.wager_id "
      "               WHERE cleanup.reversal_transaction_id = "
      "tx.transaction_id "
      "                 AND wager.user_id = tx.actor_user_id)))))) + "
      "       (SELECT count(*) FROM tarot_recovery_claim claim "
      "        LEFT JOIN event_journal event "
      "          ON event.event_id = claim.started_event_id "
      "        WHERE event.event_id IS NULL) + "
      "       (SELECT count(*) FROM tarot_draw draw "
      "        LEFT JOIN tarot_recovery_claim claim "
      "          ON claim.draw_id = draw.draw_id "
      "         AND claim.claim_id = draw.claim_id "
      "        WHERE claim.claim_id IS NULL) + "
      "       (SELECT count(*) FROM tarot_interaction_receipt receipt "
      "        LEFT JOIN tarot_account account "
      "          ON account.account_id = receipt.account_id "
      "        LEFT JOIN tarot_recovery_claim claim "
      "          ON claim.claim_id = receipt.claim_id "
      "        LEFT JOIN tarot_transaction tx "
      "          ON tx.transaction_id = receipt.transaction_id "
      "        WHERE account.account_id IS NULL "
      "           OR (receipt.claim_id IS NOT NULL AND claim.claim_id IS NULL) "
      "           OR (receipt.transaction_id IS NOT NULL "
      "               AND tx.transaction_id IS NULL)) + "
      "       (SELECT count(*) FROM tarot_account account "
      "        WHERE account.account_kind = 'HUMAN' "
      "          AND (SELECT count(*) FROM tarot_posting posting "
      "               JOIN tarot_transaction tx "
      "                 ON tx.transaction_id = posting.transaction_id "
      "               JOIN event_journal event ON event.event_id = tx.event_id "
      "               WHERE posting.account_id = account.account_id "
      "                 AND posting.amount > 0 "
      "                 AND tx.state = 'committed' "
      "                 AND tx.transaction_type = 'STARTING_GRANT' "
      "                 AND tx.is_test = 0 "
      "                 AND event.event_type = 'tarot.starting_grant.v1' "
      "                 AND event.aggregate_type = 'tarot_account' "
      "                 AND event.aggregate_id = account.account_id) <> 1)");

  std::map<std::string, std::pair<std::int64_t, bool>> balances;
  auto accounts =
      connection.prepare("SELECT account_id, account_kind FROM tarot_account");
  while (accounts.step())
    balances.emplace(
        accounts.column_text(0),
        std::pair<std::int64_t, bool>{0, accounts.column_text(1) == "HUMAN"});
  auto postings = connection.prepare(
      "SELECT posting.account_id, posting.amount FROM tarot_posting posting "
      "JOIN tarot_transaction tx ON tx.transaction_id = posting.transaction_id "
      "WHERE tx.state = 'committed' ORDER BY tx.ledger_sequence, "
      "posting.posting_id");
  while (postings.step()) {
    const auto account_id = postings.column_text(0);
    auto found = balances.find(account_id);
    if (found == balances.end()) {
      ++report.orphaned_link_count;
      continue;
    }
    try {
      found->second.first =
          checked_add(found->second.first, postings.column_int64(1));
      if (found->second.second && found->second.first < 0)
        ++report.negative_history_count;
    } catch (const std::overflow_error &) {
      ++report.overflow_count;
    }
  }
  report.valid =
      report.prepared_transaction_count == 0 &&
      report.unbalanced_transaction_count == 0 &&
      report.negative_history_count == 0 && report.overflow_count == 0 &&
      report.illegal_reversal_count == 0 && report.claim_mismatch_count == 0 &&
      report.orphaned_link_count == 0;
  return report;
}

} // namespace sanguinius::persistence
