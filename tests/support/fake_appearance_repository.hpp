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
    candidates_.push_back(candidate);
    return candidate;
  }
  std::vector<AppearanceCandidate> scan_events(const AppearancePolicy &,
                                               std::int64_t,
                                               std::string_view) override {
    return {};
  }
  bool record_final(const AppearancePolicy &, AppearanceMode,
                    const AppearanceCandidate &candidate,
                    const AppearanceEvaluation &evaluation,
                    std::string decision_id, std::string, std::string_view,
                    std::string model_status,
                    std::optional<AppearanceModelResult> result,
                    std::int64_t now_ms) override {
    store(candidate, evaluation, std::move(decision_id),
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
  bool complete_model(const AppearancePolicy &, AppearanceMode,
                      const AppearanceCandidate &candidate,
                      const AppearanceEvaluation &evaluation,
                      std::string_view decision_id, std::string,
                      std::string model_status,
                      std::optional<AppearanceModelResult> result,
                      std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    auto found = std::ranges::find(decisions_, decision_id,
                                   &AppearanceDecisionRecord::decision_id);
    if (found == decisions_.end())
      return false;
    found->state = "final";
    found->model_status = std::move(model_status);
    const bool hypothetical = evaluation.eligible_for_model && result &&
                              result->should_speak && !result->serious_context;
    found->action = hypothetical ? "hypothetical" : "reject";
    found->reason = hypothetical ? "hypothetical" : found->model_status;
    if (hypothetical)
      found->preview = result->text;
    found->created_at_ms = now_ms;
    static_cast<void>(candidate);
    return true;
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
  void store(const AppearanceCandidate &candidate,
             const AppearanceEvaluation &evaluation, std::string decision_id,
             std::string model_status,
             std::optional<AppearanceModelResult> result,
             const std::int64_t now_ms) {
    const bool hypothetical = evaluation.eligible_for_model && result &&
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
         .action = hypothetical ? "hypothetical" : "reject",
         .reason = hypothetical ? "hypothetical" : evaluation.reason,
         .score = evaluation.score,
         .model_status = std::move(model_status),
         .preview = hypothetical ? std::optional<std::string>{result->text}
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
