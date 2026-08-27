#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/tarot_house.hpp"
#include "sanguinius/tarot_integration.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteTarotCatalogRepository final : public TarotCatalogRepository {
public:
  explicit SqliteTarotCatalogRepository(
      std::shared_ptr<SqliteRepositoryContext> context);
  void install(const TarotDeckCatalog &deck, const TarotHouseCatalog &house,
               std::int64_t installed_at_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

class SqliteTarotDrawRepository final : public TarotDrawRepository {
public:
  explicit SqliteTarotDrawRepository(
      std::shared_ptr<SqliteRepositoryContext> context);
  [[nodiscard]] TarotDrawResult draw(const TarotDrawRequest &request) override;
  [[nodiscard]] std::optional<TarotDrawRecord>
  find(std::string_view draw_id, const DiscordSnowflake &requester) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

class SqliteTarotHouseRepository final : public TarotHouseRepository {
public:
  explicit SqliteTarotHouseRepository(
      std::shared_ptr<SqliteRepositoryContext> context);
  [[nodiscard]] HouseMutationResult
  play(const HousePlayRequest &request) override;
  [[nodiscard]] HouseMutationResult
  resolve(const HouseResolveRequest &request) override;
  [[nodiscard]] std::vector<HouseMutationResult>
  observe_draw(const TarotDrawRecord &draw, std::int64_t now_ms,
               std::function<std::string()> next_id) override;
  [[nodiscard]] std::vector<HouseMutationResult>
  reconcile_draws(std::int64_t now_ms, std::function<std::string()> next_id,
                  std::size_t limit = 50) override;
  [[nodiscard]] std::vector<HouseMutationResult>
  resolve_due(std::int64_t now_ms, bool test_only,
              std::function<std::string()> next_id) override;
  [[nodiscard]] HouseMutationResult
  handle_deadline(const HouseDeadlineRequest &request) override;
  [[nodiscard]] HouseMutationResult
  cleanup_test_wager(const HouseTestCleanupRequest &request) override;
  void ensure_weekly_schedule(std::int64_t now_ms, std::int64_t due_at_ms,
                              std::string catalog_version,
                              std::string job_id) override;
  [[nodiscard]] HouseWeeklyOfferResult
  handle_weekly_offer(const HouseWeeklyOfferRequest &request) override;
  [[nodiscard]] HouseOfferExpiryResult
  handle_offer_expiry(const HouseOfferExpiryRequest &request) override;
  [[nodiscard]] HouseControlResult
  inspect_control(const HouseControlRequest &request) override;
  [[nodiscard]] HouseAvailability
  availability(const TarotInvocation &invocation,
               const HouseTemplateDefinition &definition, bool is_test,
               std::int64_t starting_fate) override;
  [[nodiscard]] std::vector<HouseWagerRecord>
  history(const DiscordSnowflake &user_id,
          std::optional<std::string_view> reference) override;
  [[nodiscard]] HouseHistoryPage begin_history(const DiscordSnowflake &user_id,
                                               std::string cursor_id,
                                               std::int64_t now_ms) override;
  [[nodiscard]] HouseHistoryPage
  load_history_page(const DiscordSnowflake &user_id, std::string_view cursor_id,
                    std::size_t page, std::int64_t now_ms) override;
  [[nodiscard]] TarotPlayerRecord
  record(const DiscordSnowflake &user_id) override;
  [[nodiscard]] HouseEconomyReport economy() override;
  [[nodiscard]] TarotPlayerProjectionReport check_player_projection() override;
  [[nodiscard]] TarotPlayerProjectionReport
  rebuild_player_projection() override;
  [[nodiscard]] TarotPlayerProjectionReport
  rebuild_player_projection_uncommitted();

private:
  [[nodiscard]] TarotPlayerProjectionReport
  rebuild_player_projection_unlocked();
  std::shared_ptr<SqliteRepositoryContext> context_;
};

class SqliteTarotIntegrationRepository final
    : public TarotIntegrationRepository {
public:
  explicit SqliteTarotIntegrationRepository(
      std::shared_ptr<SqliteRepositoryContext> context);
  void ensure_schedule(std::int64_t now_ms, std::string job_id) override;
  [[nodiscard]] TarotIntegrationReport
  scan(std::int64_t now_ms, std::size_t limit,
       std::function<std::string()> next_id,
       TarotIntegrationSinkPolicy sink_policy = {}) override;
  [[nodiscard]] std::size_t suppress_disabled(std::int64_t now_ms,
                                              std::size_t limit) override;
  [[nodiscard]] bool retry(std::string_view source_event_id,
                           std::int64_t now_ms) override;
  [[nodiscard]] TarotIntegrationReport inspect() override;
  [[nodiscard]] TarotIntegrationProjectionReport check_projection();
  [[nodiscard]] TarotIntegrationProjectionReport
  rebuild_projection_uncommitted();

private:
  [[nodiscard]] TarotIntegrationProjectionReport check_projection_unlocked();
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
