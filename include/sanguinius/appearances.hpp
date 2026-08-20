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

enum class AppearanceCandidateType {
  conversation,
  recurrence,
  chronicle_entry,
  session_started,
  session_completed,
  title_awarded,
  anniversary,
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
              std::string_view instance_id) = 0;
  [[nodiscard]] virtual bool
  record_final(const AppearancePolicy &policy, AppearanceMode mode,
               const AppearanceCandidate &candidate,
               const AppearanceEvaluation &evaluation, std::string decision_id,
               std::string event_id, std::string_view instance_id,
               std::string model_status,
               std::optional<AppearanceModelResult> model_result,
               std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool
  prepare_model(const AppearancePolicy &policy, AppearanceMode mode,
                const AppearanceCandidate &candidate,
                const AppearanceEvaluation &evaluation, std::string decision_id,
                std::string event_id, std::string_view instance_id,
                std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool
  complete_model(const AppearancePolicy &policy, AppearanceMode mode,
                 const AppearanceCandidate &candidate,
                 const AppearanceEvaluation &fresh_evaluation,
                 std::string_view decision_id, std::string event_id,
                 std::string model_status,
                 std::optional<AppearanceModelResult> result,
                 std::int64_t now_ms) = 0;
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
                    AppearanceRuntimeStateProvider runtime_state = {});

  void start();
  void observe_message(const AppearanceMessageObservation &observation);
  [[nodiscard]] std::string
  simulate(const AppearanceSimulationRequest &request);
  [[nodiscard]] std::string preview(std::string_view reference);
  [[nodiscard]] std::string recent();
  [[nodiscard]] std::string set_callback_consent(DiscordSnowflake user_id,
                                                 bool enabled,
                                                 std::string idempotency_key,
                                                 std::string correlation_id);
  [[nodiscard]] bool scan_events();
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
                      std::int64_t prepared_at_ms) noexcept;
  void decorate_runtime(AppearanceCandidate &candidate,
                        const AppearancePolicy &policy) const;
  [[nodiscard]] std::int64_t now_ms() const;

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
};

} // namespace sanguinius
