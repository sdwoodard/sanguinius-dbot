#pragma once

#include "sanguinius/tarot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sanguinius::test {

class FakeTarotRepository final : public TarotRepository {
public:
  void initialize_system_accounts(const std::vector<std::string> &,
                                  std::int64_t) override {
    ++initialize_calls;
  }

  TarotAccountProvisionResult
  ensure_account(const TarotAccountProvisionRequest &) override {
    ++ensure_calls;
    const bool created = ensure_calls == 1;
    if (created)
      ++starting_grant_count;
    return {.account_id = "00000000-0000-4000-8000-000000009001",
            .balance = current_balance,
            .created = created};
  }

  std::int64_t balance(const DiscordSnowflake &) override {
    return current_balance;
  }

  TarotHistoryPage
  create_history_snapshot(const TarotHistorySnapshotRequest &) override {
    return {.status = TarotPageStatus::available,
            .entries = {},
            .offset = 0,
            .total = 0,
            .previous_custom_id = std::nullopt,
            .next_custom_id = std::nullopt};
  }

  TarotHistoryPage history_page(const TarotHistoryPageRequest &) override {
    return {.status = TarotPageStatus::invalid_token,
            .entries = {},
            .offset = 0,
            .total = 0,
            .previous_custom_id = std::nullopt,
            .next_custom_id = std::nullopt};
  }

  std::vector<TarotStanding> standings() override {
    return {
        {.user_id = 30, .display_name = "Owner", .balance = current_balance}};
  }

  TarotVisibilityResult
  set_standings_visibility(const TarotVisibilityRequest &request) override {
    const bool changed = public_standings != request.public_standings;
    public_standings = request.public_standings;
    return {.public_standings = public_standings, .changed = changed};
  }

  bool standings_visibility(const DiscordSnowflake &) override {
    return public_standings;
  }

  TarotRecoveryResult
  start_recovery(const TarotRecoveryStartRequest &request) override {
    ++recovery_starts;
    if (recovery_start_result &&
        recovery_start_key == request.invocation.interaction_idempotency_key) {
      auto replay = *recovery_start_result;
      replay.mutation_created = false;
      replay.committed_event_types.clear();
      return replay;
    }
    std::optional<TarotTrialDraw> draw;
    if (request.kind == TarotRecoveryKind::trial)
      draw = request.trial_draw();
    last_reward =
        draw ? std::optional<std::int64_t>{draw->reward} : std::nullopt;
    last_prompt_variant =
        draw ? std::optional<std::int64_t>{draw->prompt_variant} : std::nullopt;
    std::vector<std::string> custom_ids;
    for (const auto &token : request.token_ids)
      custom_ids.push_back(std::string{tarot_component_prefix} + token);
    TarotRecoveryResult result{
        .status = TarotRecoveryStatus::pending,
        .kind = request.kind,
        .visibility = request.visibility,
        .claim_id = request.claim_id,
        .balance = current_balance,
        .reward = last_reward,
        .cooldown_until_ms = std::nullopt,
        .prompt_variant = last_prompt_variant,
        .custom_ids = std::move(custom_ids),
        .mutation_created = true,
        .public_delivery_created = false,
        .committed_event_types = {request.kind == TarotRecoveryKind::grace
                                      ? "tarot.grace_started.v1"
                                      : "tarot.trial_started.v1"}};
    recovery_start_key = request.invocation.interaction_idempotency_key;
    recovery_start_result = result;
    return result;
  }

  TarotRecoveryResult
  complete_recovery(const TarotRecoveryCompleteRequest &) override {
    return completion_result;
  }

  TarotMutationResult adjust(const TarotAdjustmentRequest &request) override {
    current_balance += request.amount;
    return {.status = TarotMutationStatus::applied,
            .transaction_id = request.transaction_id,
            .balance = current_balance};
  }

  TarotMutationResult reverse(const TarotReversalRequest &request) override {
    return {.status = TarotMutationStatus::applied,
            .transaction_id = request.transaction_id,
            .balance = current_balance};
  }

  TarotInvariantReport check_invariants() override {
    return {.valid = invariants_valid,
            .account_count = 4,
            .committed_transaction_count = ensure_calls == 0 ? 0U : 1U,
            .prepared_transaction_count = invariants_valid ? 0U : 1U,
            .posting_count = ensure_calls == 0 ? 0U : 2U,
            .unbalanced_transaction_count = 0,
            .negative_history_count = 0,
            .overflow_count = 0,
            .illegal_reversal_count = 0,
            .claim_mismatch_count = 0,
            .orphaned_link_count = 0};
  }

  std::int64_t current_balance{100};
  bool public_standings{true};
  bool invariants_valid{true};
  std::size_t initialize_calls{};
  std::size_t ensure_calls{};
  std::size_t starting_grant_count{};
  std::size_t recovery_starts{};
  std::optional<std::int64_t> last_reward;
  std::optional<std::int64_t> last_prompt_variant;
  std::string recovery_start_key;
  std::optional<TarotRecoveryResult> recovery_start_result;
  TarotRecoveryResult completion_result{
      .status = TarotRecoveryStatus::invalid_token,
      .kind = TarotRecoveryKind::grace,
      .visibility = TarotVisibility::private_result,
      .claim_id = {},
      .balance = 100,
      .reward = std::nullopt,
      .cooldown_until_ms = std::nullopt,
      .prompt_variant = std::nullopt,
      .custom_ids = {},
      .mutation_created = false,
      .public_delivery_created = false,
      .committed_event_types = {}};
};

} // namespace sanguinius::test
