#pragma once

#include "sanguinius/persistence/database.hpp"
#include "sanguinius/repositories.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext {
public:
  explicit SqliteRepositoryContext(Database database);
  [[nodiscard]] SqliteConnection &connection() noexcept;

private:
  Database database_;
};

class SqliteApplicationInstanceRepository final
    : public ApplicationInstanceRepository {
public:
  explicit SqliteApplicationInstanceRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void record_start(const ApplicationInstanceRecord &record) override;
  void record_stop(const std::string &instance_id, std::int64_t stopped_at_ms,
                   ApplicationStopReason reason) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

class SqliteCoreIdentityRepository final : public CoreIdentityRepository {
public:
  explicit SqliteCoreIdentityRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void initialize_or_validate_scope(const ServerScopeConfiguration &scope,
                                    std::int64_t now_ms) override;
  void ensure_user(const DiscordUserRecord &user) override;
  [[nodiscard]] std::optional<UserPreferences>
  load_preferences(const DiscordSnowflake &user_id) override;

private:
  void ensure_user_uncommitted(const DiscordUserRecord &user);

  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
