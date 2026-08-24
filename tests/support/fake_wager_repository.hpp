#pragma once

#include "sanguinius/wagers.hpp"

#include <optional>

namespace sanguinius::test {

class FakeWagerRepository final : public TarotWagerRepository {
public:
  WagerMutationResult create_draft(const WagerCreateRequest &request) override {
    create_request = request;
    return mutation_result;
  }
  WagerMutationResult preview(const WagerPreviewRequest &request) override {
    preview_request = request;
    return mutation_result;
  }
  WagerMutationResult act(const WagerActionRequest &request) override {
    action_request = request;
    return mutation_result;
  }
  WagerMutationResult submit_outcome(const WagerOutcomeRequest &request) override {
    outcome_request = request;
    return mutation_result;
  }
  WagerMutationResult add_evidence(const WagerEvidenceRequest &request) override {
    evidence_request = request;
    return mutation_result;
  }
  WagerMutationResult judge(const WagerJudgmentRequest &request) override {
    judgment_request = request;
    return mutation_result;
  }
  WagerHistoryResult history(const WagerHistoryRequest &request) override {
    history_request = request;
    return history_result;
  }
  WagerHistoryResult disputes(const WagerHistoryRequest &request) override {
    history_request = request;
    return history_result;
  }
  WagerMutationResult handle_deadline(const WagerDeadlineRequest &request) override {
    deadline_request = request;
    return mutation_result;
  }
  WagerMutationResult set_test_role(const WagerTestRoleRequest &request) override {
    test_role_request = request;
    return mutation_result;
  }
  WagerMutationResult force_test_deadline(const WagerTestDeadlineRequest &request) override {
    test_deadline_request = request;
    return mutation_result;
  }
  WagerMutationResult cleanup_test_wager(const WagerTestCleanupRequest &request) override {
    cleanup_request = request;
    return mutation_result;
  }
  WagerInvariantReport check_invariants() override { return invariant_result; }

  WagerMutationResult mutation_result;
  WagerHistoryResult history_result;
  WagerInvariantReport invariant_result;
  std::optional<WagerCreateRequest> create_request;
  std::optional<WagerPreviewRequest> preview_request;
  std::optional<WagerActionRequest> action_request;
  std::optional<WagerOutcomeRequest> outcome_request;
  std::optional<WagerEvidenceRequest> evidence_request;
  std::optional<WagerJudgmentRequest> judgment_request;
  std::optional<WagerHistoryRequest> history_request;
  std::optional<WagerDeadlineRequest> deadline_request;
  std::optional<WagerTestRoleRequest> test_role_request;
  std::optional<WagerTestDeadlineRequest> test_deadline_request;
  std::optional<WagerTestCleanupRequest> cleanup_request;
};

} // namespace sanguinius::test
