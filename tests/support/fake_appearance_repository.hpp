#pragma once

#include "sanguinius/appearances.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sanguinius::test {

[[nodiscard]] inline AppearancePolicy test_appearance_policy() {
  const auto path = std::filesystem::path{__FILE__}
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "config/appearance-policy-v1.json";
  std::ifstream stream{path};
  if (!stream)
    throw std::runtime_error{"Unable to load test appearance policy."};
  return parse_appearance_policy(
      std::string{std::istreambuf_iterator<char>{stream},
                  std::istreambuf_iterator<char>{}});
}

class FakeAppearanceRepository final : public AppearanceRepository {
public:
  void register_policy(const AppearancePolicy &policy, std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    registered_ = true;
    policy_ = policy;
  }
  void activate_mode(const AppearanceMode mode, std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    active_mode_ = mode;
    ++mode_activation_count_;
  }
  AppearancePolicy load_policy(std::string_view) override {
    const std::scoped_lock lock{mutex_};
    return policy_.value();
  }
  std::size_t
  abandon_prior_instance_attempts(std::string_view, std::int64_t,
                                  PersistentIdGenerator &) override {
    return 0;
  }
  bool set_callback_consent(DiscordSnowflake, bool enabled, std::int64_t,
                            std::string, std::string, std::string) override {
    const std::scoped_lock lock{mutex_};
    const bool changed = callback_enabled_ != enabled;
    callback_enabled_ = enabled;
    return changed;
  }
  std::optional<AppearanceCandidate>
  observe_message(const AppearancePolicy &,
                  const AppearanceMessageObservation &observation, std::string,
                  std::string) override {
    if (throw_on_observe)
      throw std::runtime_error{"synthetic appearance failure"};
    const std::scoped_lock lock{mutex_};
    ++observation_count_;
    last_observation_ = observation;
    return std::nullopt;
  }
  AppearanceCandidate
  simulate(const AppearancePolicy &policy,
           const AppearanceSimulationRequest &request) override {
    AppearanceCandidate candidate{};
    candidate.candidate_id = request.candidate_id;
    candidate.policy_version = policy.policy_version;
    candidate.type = AppearanceCandidateType::simulation;
    candidate.created_at_ms = request.now_ms;
    candidate.expires_at_ms =
        request.now_ms + policy.candidate_expiry_ms.at("simulation");
    candidate.actors = {request.owner_user_id};
    candidate.excerpts = {"Sanitized synthetic fixture."};
    candidate.safe_summary =
        "Owner simulation fixture: " + request.fixture + ".";
    candidate.owner_simulation = true;
    candidate.alternating_turns = true;
    candidate.recurrence_matches = 2;
    candidate.human_messages_since_bot = 8;
    const std::scoped_lock lock{mutex_};
    candidate.globally_disabled = globally_disabled_;
    candidate.global_quiet =
        quiet_until_ms_ && *quiet_until_ms_ > request.now_ms;
    candidates_.push_back(candidate);
    return candidate;
  }
  std::vector<AppearanceCandidate> scan_events(const AppearancePolicy &,
                                               std::int64_t, std::string_view,
                                               std::size_t) override {
    return {};
  }
  bool event_scan_backlog() override { return false; }
  bool record_final(const AppearancePolicy &, const AppearanceMode mode,
                    const AppearanceCandidate &candidate,
                    const AppearanceEvaluation &evaluation,
                    std::string decision_id, std::string, std::string_view,
                    std::string model_status,
                    std::optional<AppearanceModelResult> result,
                    const AppearanceDeliveryIds &,
                    std::int64_t now_ms) override {
    store(candidate, evaluation, mode, std::move(decision_id),
          std::move(model_status), std::move(result), now_ms);
    return true;
  }
  bool prepare_model(const AppearancePolicy &, AppearanceMode,
                     const AppearanceCandidate &candidate,
                     const AppearanceEvaluation &evaluation,
                     std::string decision_id, std::string, std::string_view,
                     std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    decisions_.push_back(
        {.decision_id = std::move(decision_id),
         .candidate_id = candidate.candidate_id,
         .policy_version = candidate.policy_version,
         .candidate_type =
             std::string{appearance_candidate_type_name(candidate.type)},
         .safe_summary = candidate.safe_summary,
         .state = "model_pending",
         .action = {},
         .reason = "model_pending",
         .score = evaluation.score,
         .model_status = "model_pending",
         .preview = std::nullopt,
         .created_at_ms = now_ms,
         .gates = evaluation.gates,
         .score_components = evaluation.score_components,
         .memory_ids = {},
         .serious_categories = {}});
    return true;
  }
  bool complete_model(const AppearancePolicy &, const AppearanceMode mode,
                      const AppearanceCandidate &candidate,
                      const AppearanceEvaluation &evaluation,
                      std::string_view decision_id, std::string,
                      std::string model_status,
                      std::optional<AppearanceModelResult> result,
                      const AppearanceDeliveryIds &,
                      std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    auto found = std::ranges::find(decisions_, decision_id,
                                   &AppearanceDecisionRecord::decision_id);
    if (found == decisions_.end())
      return false;
    found->state = "final";
    found->model_status = std::move(model_status);
    const bool accepted = evaluation.eligible_for_model && result &&
                          result->should_speak && !result->serious_context;
    found->action = accepted ? (mode == AppearanceMode::live ? "live_queued"
                                                             : "hypothetical")
                             : "reject";
    found->reason = accepted ? found->action : found->model_status;
    if (accepted && mode != AppearanceMode::live)
      found->preview = result->text;
    found->created_at_ms = now_ms;
    static_cast<void>(candidate);
    return true;
  }
  AppearanceMutationResult
  set_quiet(const AppearanceQuietMutation &request) override {
    const std::scoped_lock lock{mutex_};
    const auto replay =
        std::ranges::find(quiet_requests_, request.idempotency_key,
                          &QuietRequest::idempotency_key);
    if (replay != quiet_requests_.end()) {
      if (replay->actor_user_id != request.actor_user_id ||
          replay->reason != request.reason ||
          replay->request_value != request.request_value)
        throw std::runtime_error{"Fake appearance quiet replay conflicts."};
      return replay->result;
    }
    const auto finish = [&](const AppearanceMutationResult result) {
      quiet_requests_.push_back({.idempotency_key = request.idempotency_key,
                                 .actor_user_id = request.actor_user_id,
                                 .reason = request.reason,
                                 .request_value = request.request_value,
                                 .result = result});
      return result;
    };
    if (!request.quiet_until_ms) {
      if (quiet_until_ms_ && request.actor_user_id != quiet_setter_ &&
          request.actor_user_id != DiscordSnowflake{30})
        return finish(AppearanceMutationResult::unauthorized);
      if (!quiet_until_ms_)
        return finish(AppearanceMutationResult::unchanged);
      quiet_until_ms_.reset();
      quiet_setter_ = {};
      return finish(AppearanceMutationResult::applied);
    }
    if (quiet_until_ms_ && *request.quiet_until_ms <= *quiet_until_ms_)
      return finish(AppearanceMutationResult::unchanged);
    quiet_until_ms_ = request.quiet_until_ms;
    quiet_setter_ = request.actor_user_id;
    return finish(AppearanceMutationResult::applied);
  }
  AppearanceMutationResult
  set_global_disabled(const DiscordSnowflake actor_user_id, const bool disabled,
                      std::int64_t, std::string, std::string,
                      std::string) override {
    const std::scoped_lock lock{mutex_};
    if (actor_user_id != DiscordSnowflake{30})
      return AppearanceMutationResult::unauthorized;
    if (globally_disabled_ == disabled)
      return AppearanceMutationResult::unchanged;
    globally_disabled_ = disabled;
    return AppearanceMutationResult::applied;
  }
  AppearanceMutationResult
  record_feedback(const AppearanceFeedbackMutation &request) override {
    const std::scoped_lock lock{mutex_};
    if (std::ranges::find(feedback_keys_, request.idempotency_key) !=
        feedback_keys_.end())
      return AppearanceMutationResult::unchanged;
    feedback_keys_.push_back(request.idempotency_key);
    ++feedback_count_;
    return request.action == AppearanceFeedbackAction::quiet_tonight
               ? AppearanceMutationResult::quiet_applied
               : AppearanceMutationResult::applied;
  }
  AppearanceControlSummary control_summary(std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    AppearanceControlSummary result;
    result.persisted_mode = active_mode_;
    result.globally_disabled = globally_disabled_;
    result.quiet_until_ms = quiet_until_ms_ && *quiet_until_ms_ > now_ms
                                ? quiet_until_ms_
                                : std::nullopt;
    return result;
  }
  std::optional<AppearanceDecisionRecord>
  decision(const std::string_view reference) override {
    const std::scoped_lock lock{mutex_};
    std::optional<AppearanceDecisionRecord> result;
    for (const auto &item : decisions_) {
      if (item.decision_id.starts_with(reference) ||
          item.candidate_id.starts_with(reference)) {
        if (result)
          return std::nullopt;
        result = item;
      }
    }
    return result;
  }
  std::vector<AppearanceDecisionRecord>
  recent(const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    auto result = decisions_;
    if (result.size() > limit)
      result.erase(result.begin(),
                   result.end() - static_cast<std::ptrdiff_t>(limit));
    std::ranges::reverse(result);
    return result;
  }
  std::size_t public_outbox_violation_count() override { return 0; }
  void purge(const AppearancePolicy &, std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    ++purge_count_;
  }

  [[nodiscard]] bool registered() const {
    const std::scoped_lock lock{mutex_};
    return registered_;
  }
  [[nodiscard]] std::size_t candidate_count() const {
    const std::scoped_lock lock{mutex_};
    return candidates_.size();
  }
  [[nodiscard]] bool callback_enabled() const {
    const std::scoped_lock lock{mutex_};
    return callback_enabled_;
  }
  [[nodiscard]] std::size_t observation_count() const {
    const std::scoped_lock lock{mutex_};
    return observation_count_;
  }
  [[nodiscard]] std::size_t purge_count() const {
    const std::scoped_lock lock{mutex_};
    return purge_count_;
  }
  [[nodiscard]] std::size_t mode_activation_count() const {
    const std::scoped_lock lock{mutex_};
    return mode_activation_count_;
  }
  [[nodiscard]] std::size_t feedback_count() const {
    const std::scoped_lock lock{mutex_};
    return feedback_count_;
  }
  [[nodiscard]] AppearanceMode active_mode() const {
    const std::scoped_lock lock{mutex_};
    return active_mode_;
  }
  [[nodiscard]] std::optional<AppearanceMessageObservation>
  last_observation() const {
    const std::scoped_lock lock{mutex_};
    return last_observation_;
  }

  bool throw_on_observe{};

private:
  struct QuietRequest {
    std::string idempotency_key;
    DiscordSnowflake actor_user_id;
    std::string reason;
    std::string request_value;
    AppearanceMutationResult result{AppearanceMutationResult::invalid};
  };

  void store(const AppearanceCandidate &candidate,
             const AppearanceEvaluation &evaluation, AppearanceMode mode,
             std::string decision_id, std::string model_status,
             std::optional<AppearanceModelResult> result,
             const std::int64_t now_ms) {
    const bool accepted = evaluation.eligible_for_model && result &&
                          result->should_speak && !result->serious_context;
    const std::scoped_lock lock{mutex_};
    decisions_.push_back(
        {.decision_id = std::move(decision_id),
         .candidate_id = candidate.candidate_id,
         .policy_version = candidate.policy_version,
         .candidate_type =
             std::string{appearance_candidate_type_name(candidate.type)},
         .safe_summary = candidate.safe_summary,
         .state = "final",
         .action = accepted ? (mode == AppearanceMode::live ? "live_queued"
                                                            : "hypothetical")
                            : "reject",
         .reason = accepted ? (mode == AppearanceMode::live ? "live_queued"
                                                            : "hypothetical")
                            : evaluation.reason,
         .score = evaluation.score,
         .model_status = std::move(model_status),
         .preview = accepted && mode != AppearanceMode::live
                        ? std::optional<std::string>{result->text}
                        : std::nullopt,
         .created_at_ms = now_ms,
         .gates = evaluation.gates,
         .score_components = evaluation.score_components,
         .memory_ids = {},
         .serious_categories = {}});
  }

  mutable std::mutex mutex_;
  bool registered_{};
  bool callback_enabled_{};
  bool globally_disabled_{};
  std::optional<std::int64_t> quiet_until_ms_;
  DiscordSnowflake quiet_setter_{};
  std::vector<QuietRequest> quiet_requests_;
  std::size_t feedback_count_{};
  std::vector<std::string> feedback_keys_;
  AppearanceMode active_mode_{AppearanceMode::off};
  std::size_t mode_activation_count_{};
  std::size_t observation_count_{};
  std::size_t purge_count_{};
  std::optional<AppearanceMessageObservation> last_observation_;
  std::optional<AppearancePolicy> policy_;
  std::vector<AppearanceCandidate> candidates_;
  std::vector<AppearanceDecisionRecord> decisions_;
};

} // namespace sanguinius::test
