#pragma once

#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteChronicleSessionRepository final
    : public ChronicleSessionRepository {
public:
  explicit SqliteChronicleSessionRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] SessionMutationResult
  start(const StartSessionRequest &request) override;
  [[nodiscard]] SessionMutationResult
  close(const CloseSessionRequest &request) override;
  [[nodiscard]] std::optional<ChronicleSession>
  status(const DiscordSnowflake &guild_id) override;
  bool observe_context(const SessionContextObservation &observation) override;
  [[nodiscard]] ChronicleSummaryValidationContext
  summary_context(std::string_view session_id) override;
  [[nodiscard]] WorkMutationStatus
  complete_summary_job(const SummaryJobCompletionRequest &request) override;
  [[nodiscard]] WorkMutationStatus
  purge_context_job(const ClaimedScheduledJob &job,
                    std::int64_t now_ms) override;
  [[nodiscard]] SessionMutationResult
  edit_summary(const SummaryEditRequest &request) override;
  [[nodiscard]] SessionMutationResult
  decide_summary(const SummaryDecisionRequest &request) override;
  [[nodiscard]] std::optional<SummaryControlResolution> resolve_summary_control(
      std::string_view token_id, const DiscordSnowflake &actor_user_id,
      const DiscordSnowflake &guild_id, const DiscordSnowflake &channel_id,
      InteractionKind interaction_kind,
      std::string_view interaction_idempotency_key,
      std::int64_t now_ms) override;
  [[nodiscard]] TitleMutationResult
  propose_title(const ProposeTitleRequest &request) override;
  [[nodiscard]] TitleMutationResult
  mutate_title(const TitleMutationRequest &request) override;
  [[nodiscard]] ChronicleTitlePage list_titles(const DiscordSnowflake &viewer,
                                               const DiscordSnowflake &target,
                                               bool owner_view,
                                               std::size_t page) override;
  [[nodiscard]] ChronicleTitlePage
  begin_title_list(const DiscordSnowflake &viewer,
                   const DiscordSnowflake &target, bool owner_view,
                   std::string cursor_id, std::int64_t now_ms) override;
  [[nodiscard]] ChronicleTitlePage
  load_title_page(const DiscordSnowflake &viewer, std::string_view cursor_id,
                  std::size_t page, std::int64_t now_ms) override;
  [[nodiscard]] ChronicleSearchPage
  begin_search(const DiscordSnowflake &viewer,
               const ChronicleSearchFilter &filter, std::string cursor_id,
               std::int64_t now_ms) override;
  [[nodiscard]] ChronicleSearchPage search_page(const DiscordSnowflake &viewer,
                                                std::string_view cursor_id,
                                                std::size_t page,
                                                std::int64_t now_ms) override;
  [[nodiscard]] ChronicleSearchPage
  advance_search(const DiscordSnowflake &viewer,
                 const DiscordSnowflake &guild_id,
                 const DiscordSnowflake &channel_id, std::string_view token_id,
                 std::string next_token_id, std::int64_t now_ms) override;
  [[nodiscard]] bool set_anniversary_reminders(const DiscordSnowflake &user_id,
                                               bool enabled,
                                               std::int64_t now_ms) override;
  [[nodiscard]] AnniversaryScanResult run_anniversary_scan(
      const ClaimedScheduledJob &job, std::string_view timezone, bool test_run,
      std::int64_t now_ms, PersistentIdGenerator &ids) override;
  [[nodiscard]] bool
  queue_anniversary_scan(const ScheduledJobEnqueue &job,
                         const AnniversaryScanJobPayload &payload,
                         std::string_view correlation_id) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
