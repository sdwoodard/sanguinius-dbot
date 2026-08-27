#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/work_queue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view appearance_scan_job_type{
    "appearance.scan.v1"};
inline constexpr std::string_view appearance_purge_job_type{
    "appearance.purge.v1"};
inline constexpr std::int64_t appearance_maximum_purge_interval_ms{60'000};
inline constexpr std::string_view appearance_feedback_component_prefix{
    "sga:1:"};

enum class AppearanceCandidateType {
  conversation,
  recurrence,
  chronicle_entry,
  session_started,
  session_completed,
  title_awarded,
  anniversary,
  tarot_event,
  simulation,
};

[[nodiscard]] std::string_view
appearance_candidate_type_name(AppearanceCandidateType type) noexcept;
[[nodiscard]] std::optional<AppearanceCandidateType>
parse_appearance_candidate_type(std::string_view value) noexcept;

struct AppearanceMessageObservation {
  DiscordSnowflake message_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake author_user_id;
  bool author_is_bot{};
  std::string excerpt;
  std::int64_t observed_at_ms{};
  std::string correlation_id;
};

struct AppearanceMemoryContext {
  std::string memory_id;
  std::size_t revision{};
  std::string text;
};

struct AppearanceCandidate {
  std::string candidate_id;
  std::string policy_version;
  AppearanceCandidateType type{AppearanceCandidateType::conversation};
  std::int64_t created_at_ms{};
  std::int64_t expires_at_ms{};
  std::int64_t mode_activated_at_ms{};
  std::vector<DiscordSnowflake> actors;
  std::vector<std::string> excerpts;
  std::vector<std::string> source_context;
  std::vector<std::string> supplied_memory_ids;
  std::vector<AppearanceMemoryContext> memory_context;
  std::string safe_summary;
  std::optional<std::string> theme_key;
  bool owner_simulation{};
  bool source_enabled{true};
  bool correct_scope{true};
  bool manual_quiet{};
  bool configured_quiet{};
  bool globally_disabled{};
  bool global_quiet{};
  bool mode_epoch_valid{true};
  bool bot_last_meaningful_speaker{};
  bool operational{true};
  bool degraded{};
  bool exact_duplicate{};
  bool budget_available{true};
  bool gap_available{true};
  bool messages_after_previous{true};
  bool theme_available{true};
  bool memory_available{true};
  bool consented{true};
  bool visible{true};
  bool alternating_turns{};
  int chronicle_specificity{};
  std::optional<std::int64_t> novelty_age_ms;
  std::size_t recurrence_matches{};
  std::optional<std::int64_t> bot_speech_age_ms;
  std::size_t human_messages_since_bot{};
  std::int64_t human_message_count{};
  std::optional<std::int64_t> repetition_age_ms;
  int uncertainty_penalty{};
  std::optional<std::string> deterministic_serious_category;
};

struct AppearanceDeliveryIds {
  std::string reservation_id;
  std::string outbox_id;
  std::array<std::string, 4> feedback_control_ids;
};

enum class AppearanceFeedbackAction {
  more,
  less,
  not_relevant,
  quiet_tonight,
};

[[nodiscard]] std::string_view
appearance_feedback_action_name(AppearanceFeedbackAction action) noexcept;
[[nodiscard]] std::optional<AppearanceFeedbackAction>
parse_appearance_feedback_action(std::string_view value) noexcept;

struct AppearanceControlSummary {
  AppearanceMode persisted_mode{AppearanceMode::off};
  bool globally_disabled{};
  std::optional<std::int64_t> quiet_until_ms;
  std::size_t active_reservations{};
  std::size_t pending_outbox{};
  std::size_t failed_outbox{};
  std::size_t ambiguous_outbox{};
  std::size_t recent_model_failures{};
  std::size_t recent_delivery_failures{};
  std::optional<std::int64_t> last_queued_at_ms;
  std::optional<std::int64_t> last_delivered_at_ms;
  std::size_t feedback_more{};
  std::size_t feedback_less{};
  std::size_t feedback_not_relevant{};
  std::string recommendation{"collect_more_feedback"};
  std::vector<std::string> theme_review_keys;
};

struct AppearanceFailureAlert {
  std::string category;
  std::size_t occurrences{};
};

struct AppearanceQuietMutation {
  DiscordSnowflake actor_user_id;
  std::optional<std::int64_t> quiet_until_ms;
  std::string reason;
  std::string request_value;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

struct AppearanceFeedbackMutation {
  DiscordSnowflake actor_user_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  AppearanceFeedbackAction action{AppearanceFeedbackAction::more};
  std::optional<std::string> control_id;
  std::optional<std::string> reference;
  std::optional<std::int64_t> quiet_until_ms;
  std::string feedback_id;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

enum class AppearanceMutationResult {
  applied,
  quiet_applied,
  unchanged,
  unauthorized,
  invalid,
  not_found,
  expired,
  conflict,
};

struct VerifiedAppearanceDelivery {
  std::string decision_id;
  bool test_delivery{};
};

struct AppearanceGate {
  std::string name;
  bool passed{};
};

struct AppearanceScoreComponent {
  std::string name;
  int points{};

  bool operator==(const AppearanceScoreComponent &) const = default;
};

struct AppearanceEvaluation {
  std::vector<AppearanceGate> gates;
  std::vector<AppearanceScoreComponent> score_components;
  int score{};
  bool eligible_for_model{};
  std::string reason;
};

[[nodiscard]] AppearanceEvaluation
evaluate_appearance(const AppearancePolicy &policy, AppearanceMode mode,
                    const AppearanceCandidate &candidate, std::int64_t now_ms);

struct AppearanceModelResult {
  bool serious_context{};
  std::vector<std::string> serious_categories;
  bool should_speak{};
  std::string text;
  std::string tone;
  std::vector<std::string> memory_ids_used;
  double confidence{};
};

[[nodiscard]] AppearanceModelResult parse_appearance_model_result(
    const AppearancePolicy &policy, std::string_view json,
    const std::vector<std::string> &supplied_memory_ids);
[[nodiscard]] bool validate_appearance_model_result(
    const AppearancePolicy &policy, const AppearanceModelResult &result,
    const std::vector<std::string> &supplied_memory_ids) noexcept;
[[nodiscard]] AiRequest
appearance_ai_request(const AppearancePolicy &policy,
                      const AppearanceCandidate &candidate,
                      std::string_view persona = {});

struct AppearanceRuntimeState {
  bool operational{true};
  bool degraded{};
};

using AppearanceRuntimeStateProvider = std::function<AppearanceRuntimeState()>;

[[nodiscard]] bool
appearance_quiet_window_active(const AppearancePolicy &policy,
                               std::int64_t now_ms, std::string_view timezone);

struct AppearanceDecisionRecord {
  std::string decision_id;
  std::string candidate_id;
  std::string policy_version;
  std::string candidate_type;
  std::string safe_summary;
  std::string state;
  std::string action;
  std::string reason;
  int score{};
  std::string model_status;
  std::optional<std::string> preview;
  std::int64_t created_at_ms{};
  std::vector<AppearanceGate> gates;
  std::vector<AppearanceScoreComponent> score_components;
  std::vector<std::string> memory_ids;
  std::vector<std::string> serious_categories;
};

struct AppearanceSimulationRequest {
  std::string fixture;
  std::string idempotency_key;
  std::string correlation_id;
  DiscordSnowflake owner_user_id;
  std::int64_t now_ms{};
  std::string candidate_id;
  std::string event_id;
};

class AppearanceRepository {
public:
  virtual ~AppearanceRepository() = default;
  virtual void register_policy(const AppearancePolicy &policy,
                               std::int64_t now_ms) = 0;
  virtual void activate_mode(AppearanceMode mode, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual AppearancePolicy
  load_policy(std::string_view policy_version) = 0;
  [[nodiscard]] virtual std::size_t
  abandon_prior_instance_attempts(std::string_view instance_id,
                                  std::int64_t now_ms,
                                  PersistentIdGenerator &ids) = 0;
  [[nodiscard]] virtual bool
  set_callback_consent(DiscordSnowflake user_id, bool enabled,
                       std::int64_t now_ms, std::string event_id,
                       std::string idempotency_key,
                       std::string correlation_id) = 0;
  [[nodiscard]] virtual std::optional<AppearanceCandidate>
  observe_message(const AppearancePolicy &policy,
                  const AppearanceMessageObservation &observation,
                  std::string candidate_id, std::string event_id) = 0;
  [[nodiscard]] virtual AppearanceCandidate
  simulate(const AppearancePolicy &policy,
           const AppearanceSimulationRequest &request) = 0;
  [[nodiscard]] virtual std::vector<AppearanceCandidate>
  scan_events(const AppearancePolicy &policy, std::int64_t now_ms,
              std::string_view instance_id, std::size_t limit = 50) = 0;
  [[nodiscard]] virtual bool event_scan_backlog() = 0;
  [[nodiscard]] virtual bool
  record_final(const AppearancePolicy &policy, AppearanceMode mode,
               const AppearanceCandidate &candidate,
               const AppearanceEvaluation &evaluation, std::string decision_id,
               std::string event_id, std::string_view instance_id,
               std::string model_status,
               std::optional<AppearanceModelResult> model_result,
               const AppearanceDeliveryIds &delivery_ids,
               std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool
  prepare_model(const AppearancePolicy &policy, AppearanceMode mode,
                const AppearanceCandidate &candidate,
                const AppearanceEvaluation &evaluation, std::string decision_id,
                std::string event_id, std::string_view instance_id,
                std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool complete_model(
      const AppearancePolicy &policy, AppearanceMode mode,
      const AppearanceCandidate &candidate,
      const AppearanceEvaluation &fresh_evaluation,
      std::string_view decision_id, std::string event_id,
      std::string model_status, std::optional<AppearanceModelResult> result,
      const AppearanceDeliveryIds &delivery_ids, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual AppearanceMutationResult
  set_quiet(const AppearanceQuietMutation &) {
    return AppearanceMutationResult::invalid;
  }
  [[nodiscard]] virtual AppearanceMutationResult
  set_global_disabled(DiscordSnowflake actor_user_id, bool disabled,
                      std::int64_t now_ms, std::string event_id,
                      std::string idempotency_key, std::string correlation_id) {
    static_cast<void>(actor_user_id);
    static_cast<void>(disabled);
    static_cast<void>(now_ms);
    static_cast<void>(event_id);
    static_cast<void>(idempotency_key);
    static_cast<void>(correlation_id);
    return AppearanceMutationResult::invalid;
  }
  [[nodiscard]] virtual AppearanceMutationResult
  record_feedback(const AppearanceFeedbackMutation &) {
    return AppearanceMutationResult::invalid;
  }
  [[nodiscard]] virtual AppearanceControlSummary control_summary(std::int64_t) {
    return {};
  }
  [[nodiscard]] virtual std::vector<AppearanceFailureAlert>
  claim_failure_alerts(std::int64_t) {
    return {};
  }
  [[nodiscard]] virtual std::optional<VerifiedAppearanceDelivery>
  verify_public_delivery(const ContextMessageSnapshot &) {
    return std::nullopt;
  }
  [[nodiscard]] virtual std::optional<AppearanceDecisionRecord>
  decision(std::string_view reference) = 0;
  [[nodiscard]] virtual std::vector<AppearanceDecisionRecord>
  recent(std::size_t limit) = 0;
  [[nodiscard]] virtual std::size_t public_outbox_violation_count() = 0;
  virtual void purge(const AppearancePolicy &policy, std::int64_t now_ms) = 0;
};

class AppearanceService {
public:
  AppearanceService(AppearanceRepository &repository, const Clock &clock,
                    PersistentIdGenerator &ids, AppearancePolicy policy,
                    AppearanceMode mode, std::string instance_id,
                    AiClient *ai_client, AiWorkService *ai_work,
                    Diagnostics &diagnostics, std::string persona = {},
                    std::string timezone = "America/New_York",
                    AppearanceRuntimeStateProvider runtime_state = {},
                    std::function<void()> outbox_wake = {});

  void start();
  void observe_message(const AppearanceMessageObservation &observation);
  [[nodiscard]] std::string
  simulate(const AppearanceSimulationRequest &request);
  [[nodiscard]] std::string preview(std::string_view reference);
  [[nodiscard]] std::string recent();
  [[nodiscard]] std::string member_status_summary();
  [[nodiscard]] std::string status_summary();
  [[nodiscard]] bool operator_disabled();
  [[nodiscard]] std::string set_callback_consent(DiscordSnowflake user_id,
                                                 bool enabled,
                                                 std::string idempotency_key,
                                                 std::string correlation_id);
  [[nodiscard]] std::string set_quiet(DiscordSnowflake actor_user_id,
                                      std::optional<std::int64_t> until_ms,
                                      std::string reason,
                                      std::string request_value,
                                      std::string idempotency_key,
                                      std::string correlation_id);
  [[nodiscard]] std::optional<std::int64_t>
  quiet_deadline(std::string_view kind, std::string_view local_time = {}) const;
  [[nodiscard]] std::string set_global_disabled(DiscordSnowflake actor_user_id,
                                                bool disabled,
                                                std::string idempotency_key,
                                                std::string correlation_id);
  [[nodiscard]] std::string feedback(const AppearanceFeedbackMutation &request);
  [[nodiscard]] std::string
  trigger_owner_live_safe(const AppearanceSimulationRequest &request);
  [[nodiscard]] std::optional<VerifiedAppearanceDelivery>
  verify_public_delivery(const ContextMessageSnapshot &message);
  [[nodiscard]] bool scan_events();
  [[nodiscard]] bool scan_event_batch(std::size_t limit = 50);
  void purge();
  [[nodiscard]] std::int64_t purge_interval_ms() const noexcept;

private:
  void evaluate(AppearanceCandidate candidate);
  void complete_model(AppearanceCandidate candidate,
                      const AppearancePolicy &policy,
                      const AppearanceEvaluation &prepared_evaluation,
                      std::string_view decision_id, std::string event_id,
                      std::string model_status,
                      std::optional<AppearanceModelResult> result,
                      AppearanceDeliveryIds delivery_ids,
                      std::int64_t prepared_at_ms) noexcept;
  void decorate_runtime(AppearanceCandidate &candidate,
                        const AppearancePolicy &policy) const;
  [[nodiscard]] std::int64_t now_ms() const;
  [[nodiscard]] AppearanceDeliveryIds delivery_ids();
  void emit_failure_alerts() noexcept;
  void wake_outbox() const;

  AppearanceRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  AppearancePolicy policy_;
  AppearanceMode mode_;
  std::string instance_id_;
  AiClient *ai_client_{};
  AiWorkService *ai_work_{};
  Diagnostics &diagnostics_;
  std::string persona_;
  std::string timezone_;
  AppearanceRuntimeStateProvider runtime_state_;
  std::function<void()> outbox_wake_;
};

[[nodiscard]] std::optional<std::int64_t>
appearance_quiet_deadline(std::int64_t now_ms, std::string_view timezone,
                          std::string_view kind,
                          std::string_view local_time = {});

} // namespace sanguinius
