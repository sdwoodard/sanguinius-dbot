#pragma once

#include "sanguinius/chronicle_sessions.hpp"

namespace sanguinius::test {

class FakeChronicleSessionRepository final : public ChronicleSessionRepository {
public:
  SessionMutationResult start(const StartSessionRequest &request) override {
    last_started = request;
    return start_result;
  }
  SessionMutationResult close(const CloseSessionRequest &request) override {
    last_closed = request;
    return close_result;
  }
  std::optional<ChronicleSession> status(const DiscordSnowflake &) override {
    return status_result;
  }
  bool observe_context(const SessionContextObservation &observation) override {
    last_observation = observation;
    return true;
  }
  ChronicleSummaryValidationContext summary_context(std::string_view) override {
    return summary_context_result;
  }
  WorkMutationStatus complete_summary_job(
      const SummaryJobCompletionRequest &request) override {
    last_summary_completion = request;
    return summary_completion_result;
  }
  WorkMutationStatus purge_context_job(const ClaimedScheduledJob &,
                                       std::int64_t) override {
    return WorkMutationStatus::applied;
  }
  SessionMutationResult edit_summary(const SummaryEditRequest &) override {
    return {.code = ChronicleSessionResultCode::updated,
            .session = std::nullopt, .wake_scheduler = false,
            .wake_outbox = false};
  }
  SessionMutationResult decide_summary(const SummaryDecisionRequest &) override {
    return {.code = ChronicleSessionResultCode::updated,
            .session = std::nullopt, .wake_scheduler = false,
            .wake_outbox = false};
  }
  std::optional<SummaryControlResolution> resolve_summary_control(
      std::string_view, const DiscordSnowflake &, const DiscordSnowflake &,
      const DiscordSnowflake &, InteractionKind, std::string_view,
      std::int64_t) override {
    return std::nullopt;
  }
  TitleMutationResult propose_title(const ProposeTitleRequest &) override {
    return {.code = ChronicleSessionResultCode::created,
            .grant = proposed_title, .wake_outbox = false};
  }
  TitleMutationResult mutate_title(const TitleMutationRequest &) override {
    return {.code = ChronicleSessionResultCode::updated,
            .grant = std::nullopt, .wake_outbox = false};
  }
  ChronicleTitlePage list_titles(const DiscordSnowflake &,
                                 const DiscordSnowflake &, bool,
                                 std::size_t page) override {
    auto result = title_page;
    result.page = page;
    return result;
  }
  ChronicleSearchPage begin_search(const DiscordSnowflake &,
                                   const ChronicleSearchFilter &filter,
                                   std::string cursor_id,
                                   std::int64_t) override {
    search_result.cursor_id = std::move(cursor_id);
    search_result.presentation = filter.presentation;
    return search_result;
  }
  ChronicleSearchPage search_page(const DiscordSnowflake &, std::string_view,
                                  std::size_t, std::int64_t) override {
    return search_result;
  }
  ChronicleSearchPage advance_search(
      const DiscordSnowflake &, const DiscordSnowflake &,
      const DiscordSnowflake &, std::string_view, std::string,
      std::int64_t) override {
    return search_result;
  }
  bool set_anniversary_reminders(const DiscordSnowflake &, bool,
                                 std::int64_t) override {
    return true;
  }
  AnniversaryScanResult run_anniversary_scan(
      const ClaimedScheduledJob &, std::string_view, bool, std::int64_t,
      PersistentIdGenerator &) override {
    return {.status = WorkMutationStatus::applied, .wake_outbox = false,
            .next_due_at_ms = std::nullopt};
  }
  bool queue_anniversary_scan(const ScheduledJobEnqueue &,
                              const AnniversaryScanJobPayload &,
                              std::string_view) override {
    ++anniversary_queue_calls;
    return false;
  }

  SessionMutationResult start_result{
      .code = ChronicleSessionResultCode::created,
      .session = std::nullopt, .wake_scheduler = false,
      .wake_outbox = false};
  SessionMutationResult close_result{
      .code = ChronicleSessionResultCode::updated,
      .session = std::nullopt, .wake_scheduler = false,
      .wake_outbox = false};
  std::optional<ChronicleSession> status_result;
  ChronicleSummaryValidationContext summary_context_result;
  WorkMutationStatus summary_completion_result{WorkMutationStatus::applied};
  ChronicleSearchPage search_result;
  std::optional<ChronicleTitleGrant> proposed_title;
  ChronicleTitlePage title_page;
  std::size_t anniversary_queue_calls{};
  std::optional<StartSessionRequest> last_started;
  std::optional<CloseSessionRequest> last_closed;
  std::optional<SessionContextObservation> last_observation;
  std::optional<SummaryJobCompletionRequest> last_summary_completion;
};

} // namespace sanguinius::test
