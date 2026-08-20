#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/random.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view tarot_component_prefix{"sgt:1:"};
inline constexpr std::int64_t tarot_interaction_lifetime_ms = 15 * 60 * 1'000;
inline constexpr std::size_t tarot_history_page_size = 5;
inline constexpr std::size_t tarot_history_maximum_items = 50;

struct TarotPolicy {
  std::int64_t starting_fate{100};
  std::int64_t grace_threshold{10};
  std::int64_t grace_target{25};
  std::int64_t grace_cooldown_hours{72};
  std::int64_t trial_threshold{50};
  std::int64_t trial_reward_min{5};
  std::int64_t trial_reward_max{15};
  std::int64_t trial_cooldown_hours{24};

  void validate() const;
};

enum class TarotVisibility { public_result, private_result };
enum class TarotRecoveryKind { grace, trial };
enum class TarotRecoveryAction { claim, vow_one, vow_two, vow_three, abandon };

struct TarotInvocation {
  DiscordSnowflake user_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  std::string display_name;
  std::string interaction_idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

struct TarotAccountProvisionRequest {
  TarotInvocation invocation;
  std::int64_t starting_fate{};
  std::string account_id;
  std::string transaction_id;
  std::string event_id;
  std::string mint_posting_id;
  std::string human_posting_id;
};

struct TarotAccountProvisionResult {
  std::string account_id;
  std::int64_t balance{};
  bool created{};
};

struct TarotHistoryEntry {
  std::string transaction_id;
  std::int64_t ledger_sequence{};
  std::string transaction_type;
  std::int64_t amount{};
  std::int64_t balance_after{};
  std::int64_t occurred_at_ms{};
  bool is_test{};
  std::optional<std::string> reason;
};

enum class TarotPageStatus {
  available,
  invalid_token,
  wrong_user,
  wrong_scope,
  expired,
};

struct TarotHistoryPage {
  TarotPageStatus status{TarotPageStatus::available};
  std::vector<TarotHistoryEntry> entries;
  std::size_t offset{};
  std::size_t total{};
  std::optional<std::string> next_custom_id;
};

struct TarotHistorySnapshotRequest {
  TarotInvocation invocation;
  std::string cursor_id;
  std::vector<std::string> page_token_ids;
};

struct TarotHistoryPageRequest {
  TarotInvocation invocation;
  std::string token_id;
};

struct TarotStanding {
  DiscordSnowflake user_id;
  std::string display_name;
  std::int64_t balance{};
};

struct TarotVisibilityRequest {
  TarotInvocation invocation;
  bool public_standings{};
  std::string event_id;
};

struct TarotVisibilityResult {
  bool public_standings{};
  bool changed{};
};

enum class TarotRecoveryStatus {
  pending,
  completed,
  ineligible,
  cooldown,
  expired,
  abandoned,
  lost_eligibility,
  invalid_token,
  wrong_user,
  wrong_scope,
};

struct TarotTrialDraw {
  std::int64_t reward{};
  std::int64_t prompt_variant{};
};

struct TarotRecoveryStartRequest {
  TarotInvocation invocation;
  TarotRecoveryKind kind{TarotRecoveryKind::grace};
  TarotVisibility visibility{TarotVisibility::public_result};
  bool is_test{};
  std::int64_t threshold{};
  std::optional<std::int64_t> grace_target;
  std::int64_t cooldown_ms{};
  std::function<TarotTrialDraw()> trial_draw;
  std::string claim_id;
  std::optional<std::string> draw_id;
  std::string started_event_id;
  std::string expired_event_id;
  std::vector<std::string> token_ids;
};

struct TarotRecoveryCompleteRequest {
  TarotInvocation invocation;
  std::string token_id;
  TarotRecoveryAction action{TarotRecoveryAction::claim};
  std::int64_t grace_cooldown_ms{};
  std::int64_t trial_cooldown_ms{};
  std::string transaction_id;
  std::string event_id;
  std::string mint_posting_id;
  std::string human_posting_id;
  std::string outbox_id;
  std::string provider_nonce;
};

struct TarotRecoveryResult {
  TarotRecoveryStatus status{TarotRecoveryStatus::invalid_token};
  TarotRecoveryKind kind{TarotRecoveryKind::grace};
  TarotVisibility visibility{TarotVisibility::public_result};
  std::string claim_id;
  std::int64_t balance{};
  std::optional<std::int64_t> reward;
  std::optional<std::int64_t> cooldown_until_ms;
  std::optional<std::int64_t> prompt_variant;
  std::vector<std::string> custom_ids;
  bool mutation_created{};
  bool public_delivery_created{};
  std::vector<std::string> committed_event_types;
};

struct TarotAdjustmentRequest {
  TarotInvocation invocation;
  std::int64_t amount{};
  std::string reason;
  std::string transaction_id;
  std::string event_id;
  std::string system_posting_id;
  std::string human_posting_id;
};

struct TarotReversalRequest {
  TarotInvocation invocation;
  std::string original_transaction_id;
  std::string reason;
  std::string transaction_id;
  std::string event_id;
  std::string first_posting_id;
  std::string second_posting_id;
};

enum class TarotMutationStatus {
  applied,
  unchanged,
  not_found,
  forbidden,
  would_overdraw,
};

struct TarotMutationResult {
  TarotMutationStatus status{TarotMutationStatus::not_found};
  std::string transaction_id;
  std::int64_t balance{};
};

struct TarotInvariantReport {
  bool valid{true};
  std::size_t account_count{};
  std::size_t committed_transaction_count{};
  std::size_t prepared_transaction_count{};
  std::size_t posting_count{};
  std::size_t unbalanced_transaction_count{};
  std::size_t negative_history_count{};
  std::size_t overflow_count{};
  std::size_t illegal_reversal_count{};
  std::size_t claim_mismatch_count{};
  std::size_t orphaned_link_count{};
};

class TarotRepository {
public:
  virtual ~TarotRepository() = default;

  virtual void
  initialize_system_accounts(const std::vector<std::string> &account_ids,
                             std::int64_t now_ms) = 0;
  [[nodiscard]] virtual TarotAccountProvisionResult
  ensure_account(const TarotAccountProvisionRequest &request) = 0;
  [[nodiscard]] virtual std::int64_t
  balance(const DiscordSnowflake &user_id) = 0;
  [[nodiscard]] virtual TarotHistoryPage
  create_history_snapshot(const TarotHistorySnapshotRequest &request) = 0;
  [[nodiscard]] virtual TarotHistoryPage
  history_page(const TarotHistoryPageRequest &request) = 0;
  [[nodiscard]] virtual std::vector<TarotStanding> standings() = 0;
  [[nodiscard]] virtual TarotVisibilityResult
  set_standings_visibility(const TarotVisibilityRequest &request) = 0;
  [[nodiscard]] virtual bool
  standings_visibility(const DiscordSnowflake &user_id) = 0;
  [[nodiscard]] virtual TarotRecoveryResult
  start_recovery(const TarotRecoveryStartRequest &request) = 0;
  [[nodiscard]] virtual TarotRecoveryResult
  complete_recovery(const TarotRecoveryCompleteRequest &request) = 0;
  [[nodiscard]] virtual TarotMutationResult
  adjust(const TarotAdjustmentRequest &request) = 0;
  [[nodiscard]] virtual TarotMutationResult
  reverse(const TarotReversalRequest &request) = 0;
  [[nodiscard]] virtual TarotInvariantReport check_invariants() = 0;
};

class TarotService {
public:
  TarotService(TarotRepository &repository, const Clock &clock,
               PersistentIdGenerator &ids, Random &random, TarotPolicy policy,
               ServerScopeConfiguration scope, bool test_mode,
               Diagnostics &diagnostics, std::function<void()> wake_outbox,
               std::function<void(std::string_view)> observer = {});

  void initialize();
  [[nodiscard]] InteractionMessage
  balance(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  history(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  standings(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  set_standings_visibility(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  start_grace(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  start_trial(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  apply_component(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  adjust(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  reverse(const IncomingInteraction &interaction);
  [[nodiscard]] TarotInvariantReport check_invariants();
  [[nodiscard]] std::string privacy_summary(const DiscordSnowflake &user_id);

private:
  [[nodiscard]] TarotInvocation
  invocation(const IncomingInteraction &interaction) const;
  [[nodiscard]] TarotAccountProvisionResult
  ensure_account(const TarotInvocation &invocation);
  [[nodiscard]] TarotVisibility
  requested_visibility(const IncomingInteraction &interaction) const;
  [[nodiscard]] InteractionMessage
  start_recovery(const IncomingInteraction &interaction,
                 TarotRecoveryKind kind);
  void observe(std::string_view event_type) const noexcept;

  TarotRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Random &random_;
  TarotPolicy policy_;
  ServerScopeConfiguration scope_;
  bool test_mode_{};
  Diagnostics &diagnostics_;
  std::function<void()> wake_outbox_;
  std::function<void(std::string_view)> observer_;
};

[[nodiscard]] std::optional<std::string>
parse_tarot_component(std::string_view custom_id);

} // namespace sanguinius
