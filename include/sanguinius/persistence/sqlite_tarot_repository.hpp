#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/tarot.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteTarotRepository final : public TarotRepository {
public:
  explicit SqliteTarotRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void initialize_system_accounts(const std::vector<std::string> &account_ids,
                                  std::int64_t now_ms) override;
  [[nodiscard]] TarotAccountProvisionResult
  ensure_account(const TarotAccountProvisionRequest &request) override;
  [[nodiscard]] std::int64_t
  balance(const DiscordSnowflake &user_id) override;
  [[nodiscard]] TarotHistoryPage create_history_snapshot(
      const TarotHistorySnapshotRequest &request) override;
  [[nodiscard]] TarotHistoryPage
  history_page(const TarotHistoryPageRequest &request) override;
  [[nodiscard]] std::vector<TarotStanding> standings() override;
  [[nodiscard]] TarotVisibilityResult set_standings_visibility(
      const TarotVisibilityRequest &request) override;
  [[nodiscard]] bool
  standings_visibility(const DiscordSnowflake &user_id) override;
  [[nodiscard]] TarotRecoveryResult
  start_recovery(const TarotRecoveryStartRequest &request) override;
  [[nodiscard]] TarotRecoveryResult
  complete_recovery(const TarotRecoveryCompleteRequest &request) override;
  [[nodiscard]] TarotMutationResult
  adjust(const TarotAdjustmentRequest &request) override;
  [[nodiscard]] TarotMutationResult
  reverse(const TarotReversalRequest &request) override;
  [[nodiscard]] TarotInvariantReport check_invariants() override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
