#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/relationships.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRelationshipRepository final : public RelationshipRepository {
public:
  explicit SqliteRelationshipRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] PreparedPromptContext
  prepare_prompt_context(const PreparePromptContextRequest &request) override;
  [[nodiscard]] PromptFinalizationStatus
  complete_prompt_attempt(const CompletePromptAttemptRequest &request) override;
  [[nodiscard]] PromptFinalizationStatus
  fail_prompt_attempt(const FailPromptAttemptRequest &request) override;
  std::size_t recover_prompt_attempts(std::string_view instance_id,
                                      std::int64_t now_ms,
                                      std::size_t limit = 50) override;
  std::size_t synchronize_chronicle_sources(PersistentIdGenerator &ids,
                                            std::int64_t now_ms,
                                            std::size_t limit = 50) override;
  [[nodiscard]] RelationshipProfile profile(const DiscordSnowflake &viewer,
                                            const DiscordSnowflake &target,
                                            bool public_view,
                                            std::int64_t now_ms) override;
  [[nodiscard]] PreferenceChangeStatus
  set_memory_callbacks(const SetMemoryCallbacksRequest &request) override;
  [[nodiscard]] ProjectionCheckResult check_projection() override;
  [[nodiscard]] ProjectionCheckResult rebuild_projection() override;
  [[nodiscard]] ProjectionCheckResult rebuild_projection_uncommitted();

private:
  [[nodiscard]] ProjectionCheckResult rebuild_projection_unlocked();
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
