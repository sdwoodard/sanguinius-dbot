#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/vox_narration.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteVoxNarrationRepository final : public VoxNarrationRepository {
public:
  explicit SqliteVoxNarrationRepository(
      std::shared_ptr<SqliteRepositoryContext> context,
      TtsUsagePolicy usage_policy = {});

  std::size_t observe_batch(const VoxNarrationObserveRequest &) override;
  [[nodiscard]] VoxNarrationEnqueueResult
  enqueue_reference(const VoxNarrationEnqueueRequest &) override;
  [[nodiscard]] std::string
  enqueue_reference_with_receipt(const VoxNarrationEnqueueRequest &,
                                 const VoxNarrationControlContext &) override;
  [[nodiscard]] std::optional<VoxNarrationCandidate>
  claim_next(const VoxNarrationClaimRequest &) override;
  [[nodiscard]] std::optional<VoxNarrationCandidate>
  begin_generation(const VoxNarrationGenerationStartRequest &) override;
  void complete_generation(const VoxNarrationCompletion &) override;
  std::size_t reconcile(std::int64_t now_ms,
                        const std::function<std::string()> &next_id,
                        const std::function<bool(std::string_view)>
                            &generation_is_live = {}) override;
  [[nodiscard]] std::optional<VoxNarrationCandidate>
  preview(std::string_view source_event_id, std::int64_t now_ms) override;
  [[nodiscard]] std::vector<VoxNarrationRecent>
  recent(std::size_t limit) override;
  [[nodiscard]] VoxNarrationHealth health() override;
  [[nodiscard]] std::optional<std::string>
  control_receipt(const VoxNarrationControlContext &) override;
  [[nodiscard]] std::string
  record_control_receipt(const VoxNarrationControlContext &,
                         std::string message) override;
  [[nodiscard]] bool automatic_speech_suppressed(std::int64_t now_ms) override;
  [[nodiscard]] bool
  automatic_speech_admission_suppressed(std::int64_t now_ms) override;
  [[nodiscard]] std::optional<std::string>
  session_flavor_context(std::string_view session_id, std::string_view guild_id,
                         std::string_view summoner_user_id) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
  TtsUsagePolicy usage_policy_;
};

} // namespace sanguinius::persistence
