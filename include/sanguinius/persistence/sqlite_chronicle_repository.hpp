#pragma once

#include "sanguinius/chronicle.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteChronicleRepository final : public ChronicleRepository {
public:
  explicit SqliteChronicleRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] ProposalResult
  create_or_get_proposal(const CreateProposalRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  edit_proposal(const EditProposalRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  submit_proposal(const SubmitProposalRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  apply_approval(const ApplyApprovalRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  confirm_memory(const ConfirmMemoryRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  retract_memory(const RetractItemRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  retract_entry(const RetractItemRequest &request) override;
  [[nodiscard]] ChronicleMutationResult
  expire_memory(const ClaimedScheduledJob &job, std::string event_id,
                std::int64_t now_ms) override;
  [[nodiscard]] RecallResults
  recall(const DiscordSnowflake &viewer, std::string_view query,
         std::int64_t now_ms, std::size_t limit) override;
  [[nodiscard]] std::vector<ChronicleEntry>
  timeline(std::optional<std::int64_t> since_ms, std::int64_t now_ms,
           std::size_t limit) override;
  [[nodiscard]] std::vector<ManageableChronicleItem>
  manageable(const DiscordSnowflake &viewer, const DiscordSnowflake &owner,
             std::string_view reference, std::int64_t now_ms,
             std::size_t limit) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
