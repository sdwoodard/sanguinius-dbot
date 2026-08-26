#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/vox.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteVoxRepository final : public VoxRepository {
public:
  explicit SqliteVoxRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] VoxCommandResult
  preflight_summon(const VoxCommandContext &context) override;
  [[nodiscard]] VoxCommandResult
  record_summon_rejection(const VoxCommandContext &context, VoxResultCode code,
                          std::string message) override;
  [[nodiscard]] VoxCommandResult start(const VoxStartRequest &request) override;
  [[nodiscard]] VoxCommandResult
  finalize_summon(const VoxCommandContext &context, std::string_view session_id,
                  std::size_t expected_revision, bool gateway_accepted,
                  std::string event_id) override;
  [[nodiscard]] VoxCommandResult
  command_status(const VoxCommandContext &context) override;
  [[nodiscard]] VoxCommandResult
  command_leave(const VoxCommandContext &context, std::string event_id,
                std::string timeout_job_id) override;
  [[nodiscard]] VoxCommandResult
  command_mute(const VoxCommandContext &context, bool unmute,
               std::optional<std::int64_t> mute_until_ms, std::string event_id,
               std::optional<std::string> mute_job_id) override;
  [[nodiscard]] std::optional<VoxCommandResult>
  command_receipt(const VoxCommandContext &context, std::string_view operation,
                  std::string_view request_fingerprint) override;
  [[nodiscard]] VoxCommandResult record_command_receipt(
      const VoxCommandContext &context, std::string_view operation,
      std::string_view request_fingerprint, VoxCommandResult result) override;
  [[nodiscard]] VoxCommandResult
  command_test_disconnect(const VoxCommandContext &context,
                          std::string event_id,
                          std::string timeout_job_id) override;
  [[nodiscard]] VoxCommandResult
  transition(const VoxTransitionRequest &request,
             std::optional<std::string> outbox_id) override;
  [[nodiscard]] VoxCommandResult
  fixture(const VoxFixtureRequest &request) override;
  [[nodiscard]] VoxCommandResult
  occupancy(const VoxOccupancyRequest &request) override;
  [[nodiscard]] VoxCommandResult
  handle_timeout(const ClaimedScheduledJob &job, std::int64_t now_ms,
                 std::string event_id, std::string outbox_id,
                 std::string fixture_event_id,
                 std::optional<std::size_t> observed_humans) override;
  [[nodiscard]] WorkMutationStatus
  fail_timeout_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
                   std::int64_t retry_at_ms, std::string error_code,
                   bool retryable) override;
  [[nodiscard]] std::optional<VoxSession> active() override;
  [[nodiscard]] std::size_t recover(std::string_view instance_id,
                                    std::int64_t now_ms, std::string event_id,
                                    std::string fixture_event_id) override;
  [[nodiscard]] VoxCommandResult
  shutdown(std::int64_t now_ms, std::string event_id,
           std::string fixture_event_id,
           std::optional<std::string> queued_fixture_failure_category =
               std::nullopt) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
