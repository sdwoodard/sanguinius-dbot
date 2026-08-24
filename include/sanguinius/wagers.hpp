#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view wager_component_prefix{"sgw:1:"};
inline constexpr std::string_view wager_form_prefix{"sgwf:1:"};
inline constexpr std::string_view wager_evidence_prefix{"sgwe:1:"};
inline constexpr std::string_view wager_outcome_prefix{"sgwo:1:"};
inline constexpr std::string_view wager_history_prefix{"sgwh:1:"};
inline constexpr std::string_view wager_deadline_job_type{
    "tarot.wager-deadline.v1"};
inline constexpr std::string_view wager_public_edit_outbox_kind{
    "discord.message_edit.v1"};
inline constexpr std::int64_t wager_control_lifetime_ms = 15 * 60 * 1'000;
inline constexpr std::size_t wager_history_page_size = 5;
inline constexpr std::size_t wager_history_evidence_limit = 5;

struct WagerPolicy {
  std::int64_t minimum_stake{1};
  std::int64_t maximum_stake{100};
  std::int64_t offer_expiry_hours{24};
  std::int64_t default_outcome_hours{24};
  std::int64_t resolution_grace_hours{48};

  void validate() const;
};

enum class WagerVisibility { public_offer, sealed };
enum class WagerResolutionPolicy { mutual, designated };
enum class WagerState {
  draft,
  offered,
  accepted_funded,
  awaiting_resolution,
  disputed,
  resolved,
  void_refunded,
  cancelled,
  declined,
  expired,
};
enum class WagerRole { creator, target, judge, owner, scheduler };
enum class WagerAction {
  confirm,
  discard,
  accept,
  decline,
  cancel,
  dispute,
  agree,
  void_wager,
};
enum class WagerJudgment { creator, target, void_wager };
enum class WagerDeadlinePhase {
  draft_expiry,
  offer_expiry,
  reminder,
  outcome_due,
  grace,
};
enum class WagerMutationStatus {
  applied,
  unchanged,
  forbidden,
  invalid_state,
  expired,
  insufficient_funds,
  not_found,
  stale,
};

struct WagerInvocation {
  DiscordSnowflake user_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  std::string interaction_idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
  bool owner{};
  bool test_mode{};
};

using WagerIdFactory = std::function<std::string()>;

struct WagerRecord {
  std::string wager_id;
  WagerState state{WagerState::draft};
  std::size_t revision{1};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake creator_user_id;
  DiscordSnowflake target_user_id;
  std::optional<DiscordSnowflake> judge_user_id;
  std::string creator_display_name;
  std::string target_display_name;
  std::optional<std::string> judge_display_name;
  WagerVisibility visibility{WagerVisibility::public_offer};
  WagerResolutionPolicy resolution_policy{WagerResolutionPolicy::mutual};
  std::optional<std::string> proposition;
  std::optional<std::int64_t> stake;
  std::optional<std::string> evidence_instructions;
  std::int64_t outcome_window_ms{};
  std::int64_t resolution_grace_ms{};
  std::optional<std::int64_t> offer_duration_ms;
  std::optional<std::int64_t> offer_expires_at_ms;
  std::optional<std::int64_t> outcome_due_at_ms;
  std::optional<std::int64_t> resolution_grace_until_ms;
  std::optional<WagerRole> winner;
  std::optional<std::string> terminal_reason;
  bool is_test{};
  std::int64_t created_at_ms{};
  std::int64_t updated_at_ms{};
};

struct WagerCreateRequest {
  WagerInvocation invocation;
  DiscordSnowflake target_user_id;
  std::optional<DiscordSnowflake> judge_user_id;
  WagerVisibility visibility{WagerVisibility::public_offer};
  WagerResolutionPolicy resolution_policy{WagerResolutionPolicy::mutual};
  std::int64_t outcome_window_ms{};
  std::int64_t resolution_grace_ms{};
  std::int64_t draft_expires_at_ms{};
  bool is_test{};
  WagerIdFactory next_id;
};

struct WagerPreviewRequest {
  WagerInvocation invocation;
  std::string token_id;
  std::string proposition;
  std::int64_t stake{};
  std::optional<std::string> evidence_instructions;
  std::int64_t offer_expiry_ms{};
  WagerIdFactory next_id;
};

struct WagerActionRequest {
  WagerInvocation invocation;
  std::string wager_id;
  std::optional<std::string> token_id;
  WagerAction action{WagerAction::cancel};
  std::int64_t starting_fate{};
  std::int64_t offer_expiry_ms{};
  std::int64_t resolution_grace_ms{};
  WagerIdFactory next_id;
};

struct WagerOutcomeRequest {
  WagerInvocation invocation;
  std::string wager_id;
  std::optional<std::string> token_id;
  WagerRole winner{WagerRole::creator};
  WagerIdFactory next_id;
};

struct WagerEvidenceRequest {
  WagerInvocation invocation;
  std::string wager_id;
  std::optional<std::string> token_id;
  std::string body;
  WagerIdFactory next_id;
};

struct WagerJudgmentRequest {
  WagerInvocation invocation;
  std::string wager_id;
  WagerJudgment judgment{WagerJudgment::void_wager};
  std::string reason;
  WagerIdFactory next_id;
};

struct WagerHistoryRequest {
  WagerInvocation invocation;
  std::optional<std::string> wager_id;
  std::optional<std::string> cursor_id;
  WagerIdFactory next_id;
};

struct WagerDeadlineRequest {
  ClaimedScheduledJob job;
  std::int64_t now_ms{};
  WagerIdFactory next_id;
};

struct WagerTestRoleRequest {
  WagerInvocation invocation;
  std::string wager_id;
  WagerRole role{WagerRole::creator};
  WagerIdFactory next_id;
};

struct WagerTestDeadlineRequest {
  WagerInvocation invocation;
  std::string wager_id;
  WagerDeadlinePhase phase{WagerDeadlinePhase::offer_expiry};
  WagerIdFactory next_id;
};

struct WagerTestCleanupRequest {
  WagerInvocation invocation;
  std::string wager_id;
  std::string reason;
  WagerIdFactory next_id;
};

struct WagerMutationResult {
  WagerMutationStatus status{WagerMutationStatus::not_found};
  std::optional<WagerRecord> wager;
  struct Control {
    std::string custom_id;
    std::string action;
  };
  std::vector<Control> controls;
  std::vector<std::string> committed_event_types;
  bool public_delivery_created{};
};

struct WagerHistoryResult {
  WagerMutationStatus status{WagerMutationStatus::not_found};
  std::vector<WagerRecord> wagers;
  std::vector<std::string> outcomes;
  std::vector<std::string> evidence;
  std::size_t evidence_total_count{};
  std::optional<std::string> next_cursor_id;
  std::vector<WagerMutationResult::Control> controls;
  bool exact{};
};

struct WagerInvariantReport {
  bool valid{true};
  std::size_t open_funded_obligation_count{};
  std::int64_t open_funded_obligation_amount{};
  std::int64_t escrow_balance{};
  std::size_t disputed_count{};
  std::size_t malformed_transfer_count{};
  std::size_t invalid_deadline_action_link_count{};
  std::size_t orphaned_link_count{};
};

class TarotWagerRepository {
public:
  virtual ~TarotWagerRepository() = default;

  [[nodiscard]] virtual WagerMutationResult
  create_draft(const WagerCreateRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  preview(const WagerPreviewRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  act(const WagerActionRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  submit_outcome(const WagerOutcomeRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  add_evidence(const WagerEvidenceRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  judge(const WagerJudgmentRequest &request) = 0;
  [[nodiscard]] virtual WagerHistoryResult
  history(const WagerHistoryRequest &request) = 0;
  [[nodiscard]] virtual WagerHistoryResult
  disputes(const WagerHistoryRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  handle_deadline(const WagerDeadlineRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  set_test_role(const WagerTestRoleRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  force_test_deadline(const WagerTestDeadlineRequest &request) = 0;
  [[nodiscard]] virtual WagerMutationResult
  cleanup_test_wager(const WagerTestCleanupRequest &request) = 0;
  [[nodiscard]] virtual WagerInvariantReport check_invariants() = 0;
};

class TarotWagerService {
public:
  TarotWagerService(TarotWagerRepository &repository, const Clock &clock,
                    PersistentIdGenerator &ids, WagerPolicy policy,
                    std::int64_t starting_fate, ServerScopeConfiguration scope,
                    bool test_mode, Diagnostics &diagnostics,
                    std::function<void()> wake_scheduler,
                    std::function<void()> wake_outbox,
                    std::function<void(std::string_view)> observer = {});

  [[nodiscard]] InteractionMessage
  create(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  preview(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  apply_component(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  action(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  outcome(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  evidence(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  judgment(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  wagers(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  disputes(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  set_test_role(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  force_test_deadline(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  cleanup_test_wager(const IncomingInteraction &interaction);
  void handle_deadline(const ClaimedScheduledJob &job);
  [[nodiscard]] WagerInvariantReport check_invariants();

  [[nodiscard]] static ModalPayload wager_form(std::string token_id);
  [[nodiscard]] static ModalPayload evidence_form(std::string token_id);
  [[nodiscard]] static ModalPayload outcome_form(std::string token_id);

private:
  [[nodiscard]] WagerInvocation
  invocation(const IncomingInteraction &interaction) const;
  [[nodiscard]] WagerIdFactory id_factory();
  void post_commit(const WagerMutationResult &result) const noexcept;

  TarotWagerRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  WagerPolicy policy_;
  std::int64_t starting_fate_{};
  ServerScopeConfiguration scope_;
  bool test_mode_{};
  Diagnostics &diagnostics_;
  std::function<void()> wake_scheduler_;
  std::function<void()> wake_outbox_;
  std::function<void(std::string_view)> observer_;
};

[[nodiscard]] std::optional<std::string>
parse_wager_component(std::string_view custom_id);
[[nodiscard]] std::optional<std::string>
parse_wager_form(std::string_view custom_id);
[[nodiscard]] std::optional<std::string>
parse_wager_evidence_form(std::string_view custom_id);
[[nodiscard]] std::optional<std::string>
parse_wager_outcome_form(std::string_view custom_id);
[[nodiscard]] std::optional<std::string>
parse_wager_history(std::string_view custom_id);
[[nodiscard]] const char *wager_state_name(WagerState state) noexcept;
[[nodiscard]] const char *wager_role_name(WagerRole role) noexcept;
[[nodiscard]] const char *
wager_deadline_phase_name(WagerDeadlinePhase phase) noexcept;

} // namespace sanguinius
