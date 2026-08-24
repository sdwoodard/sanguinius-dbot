#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/wagers.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteWagerRepository final : public TarotWagerRepository {
public:
  explicit SqliteWagerRepository(std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] WagerMutationResult create_draft(const WagerCreateRequest &request) override;
  [[nodiscard]] WagerMutationResult preview(const WagerPreviewRequest &request) override;
  [[nodiscard]] WagerMutationResult act(const WagerActionRequest &request) override;
  [[nodiscard]] WagerMutationResult submit_outcome(const WagerOutcomeRequest &request) override;
  [[nodiscard]] WagerMutationResult add_evidence(const WagerEvidenceRequest &request) override;
  [[nodiscard]] WagerMutationResult judge(const WagerJudgmentRequest &request) override;
  [[nodiscard]] WagerHistoryResult history(const WagerHistoryRequest &request) override;
  [[nodiscard]] WagerHistoryResult disputes(const WagerHistoryRequest &request) override;
  [[nodiscard]] WagerMutationResult handle_deadline(const WagerDeadlineRequest &request) override;
  [[nodiscard]] WagerMutationResult set_test_role(const WagerTestRoleRequest &request) override;
  [[nodiscard]] WagerMutationResult force_test_deadline(const WagerTestDeadlineRequest &request) override;
  [[nodiscard]] WagerMutationResult cleanup_test_wager(const WagerTestCleanupRequest &request) override;
  [[nodiscard]] WagerInvariantReport check_invariants() override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
