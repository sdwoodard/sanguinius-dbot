#pragma once

#include "sanguinius/retention.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext;

class SqliteRetentionRepository final : public RetentionRepository {
public:
  explicit SqliteRetentionRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void ensure_schedule(std::int64_t now_ms, std::string job_id) override;
  [[nodiscard]] RetentionCounts run(std::int64_t now_ms, std::string run_id,
                                    RetentionCounts initial = {}) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
