#pragma once

#include "sanguinius/persistence/database.hpp"
#include "sanguinius/repositories.hpp"

#include <memory>
#include <mutex>

namespace sanguinius::persistence {

class SqliteRepositoryContext {
public:
  explicit SqliteRepositoryContext(Database database);
  [[nodiscard]] SqliteConnection &connection() noexcept;
  [[nodiscard]] std::mutex &mutex() noexcept;

private:
  Database database_;
  std::mutex mutex_;
};

class SqliteApplicationInstanceRepository final
    : public ApplicationInstanceRepository {
public:
  explicit SqliteApplicationInstanceRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void record_start(const ApplicationInstanceRecord &record) override;
  void record_heartbeat(const std::string &instance_id,
                        std::int64_t heartbeat_at_ms) override;
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

class SqlitePendingNoticeRepository final : public PendingNoticeRepository {
public:
  explicit SqlitePendingNoticeRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] CreatePendingNoticeResult
  create_with_token(const CreatePendingNoticeRequest &request) override;
  [[nodiscard]] OpenPendingNoticeResult
  open_by_token(const OpenNoticeByTokenRequest &request) override;
  [[nodiscard]] OpenPendingNoticeResult
  open_next(const OpenNextNoticeRequest &request) override;
  [[nodiscard]] PendingNoticeMutationStatus
  confirm_open_delivery(const std::string &interaction_idempotency_key,
                        std::int64_t now_ms) override;
  [[nodiscard]] PendingNoticeMutationStatus
  release_open_delivery(const std::string &interaction_idempotency_key,
                        std::int64_t now_ms) override;
  [[nodiscard]] std::size_t
  recover_incomplete_open_deliveries(std::int64_t now_ms) override;
  [[nodiscard]] PendingNoticeMutationStatus
  consume(const std::string &notice_id, const DiscordSnowflake &user_id,
          std::int64_t now_ms) override;
  [[nodiscard]] PendingNoticeMutationStatus
  cancel(const std::string &notice_id, const DiscordSnowflake &user_id,
         std::int64_t now_ms) override;
  [[nodiscard]] std::size_t expire_due(std::int64_t now_ms) override;
  [[nodiscard]] std::size_t pending_count(const DiscordSnowflake &user_id,
                                          std::int64_t now_ms) override;
  [[nodiscard]] std::size_t pending_count_all(std::int64_t now_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
