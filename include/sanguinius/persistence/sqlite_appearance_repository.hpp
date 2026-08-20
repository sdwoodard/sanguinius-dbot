#pragma once

#include "sanguinius/appearances.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteAppearanceRepository final : public AppearanceRepository {
public:
  explicit SqliteAppearanceRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void register_policy(const AppearancePolicy &policy,
                       std::int64_t now_ms) override;
  void activate_mode(AppearanceMode mode, std::int64_t now_ms) override;
  [[nodiscard]] AppearancePolicy
  load_policy(std::string_view policy_version) override;
  [[nodiscard]] std::size_t
  abandon_prior_instance_attempts(std::string_view instance_id,
                                  std::int64_t now_ms,
                                  PersistentIdGenerator &ids) override;
  [[nodiscard]] bool set_callback_consent(DiscordSnowflake user_id,
                                          bool enabled, std::int64_t now_ms,
                                          std::string event_id,
                                          std::string idempotency_key,
                                          std::string correlation_id) override;
  [[nodiscard]] std::optional<AppearanceCandidate>
  observe_message(const AppearancePolicy &policy,
                  const AppearanceMessageObservation &observation,
                  std::string candidate_id, std::string event_id) override;
  [[nodiscard]] AppearanceCandidate
  simulate(const AppearancePolicy &policy,
           const AppearanceSimulationRequest &request) override;
  [[nodiscard]] std::vector<AppearanceCandidate>
  scan_events(const AppearancePolicy &policy, std::int64_t now_ms,
              std::string_view instance_id) override;
  [[nodiscard]] bool
  record_final(const AppearancePolicy &policy, AppearanceMode mode,
               const AppearanceCandidate &candidate,
               const AppearanceEvaluation &evaluation, std::string decision_id,
               std::string event_id, std::string_view instance_id,
               std::string model_status,
               std::optional<AppearanceModelResult> model_result,
               std::int64_t now_ms) override;
  [[nodiscard]] bool
  prepare_model(const AppearancePolicy &policy, AppearanceMode mode,
                const AppearanceCandidate &candidate,
                const AppearanceEvaluation &evaluation, std::string decision_id,
                std::string event_id, std::string_view instance_id,
                std::int64_t now_ms) override;
  [[nodiscard]] bool
  complete_model(const AppearancePolicy &policy, AppearanceMode mode,
                 const AppearanceCandidate &candidate,
                 const AppearanceEvaluation &fresh_evaluation,
                 std::string_view decision_id, std::string event_id,
                 std::string model_status,
                 std::optional<AppearanceModelResult> result,
                 std::int64_t now_ms) override;
  [[nodiscard]] std::optional<AppearanceDecisionRecord>
  decision(std::string_view reference) override;
  [[nodiscard]] std::vector<AppearanceDecisionRecord>
  recent(std::size_t limit) override;
  [[nodiscard]] std::size_t public_outbox_violation_count() override;
  void purge(const AppearancePolicy &policy, std::int64_t now_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
