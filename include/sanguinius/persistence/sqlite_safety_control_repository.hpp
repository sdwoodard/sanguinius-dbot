#pragma once

#include "sanguinius/safety_controls.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext;

class SqliteRuntimeFeatureControlRepository final
    : public RuntimeFeatureControlRepository {
public:
  explicit SqliteRuntimeFeatureControlRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] std::vector<RuntimeFeatureControl> snapshot() override;
  [[nodiscard]] RuntimeControlMutation
  set(std::string_view feature, bool disabled, DiscordSnowflake actor_user_id,
      std::string transition_id, std::string idempotency_key,
      std::int64_t now_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
