#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view session_summary_job_type{
    "chronicle.session-summary.v1"};
inline constexpr std::string_view session_context_purge_job_type{
    "chronicle.session-context-purge.v1"};
inline constexpr std::string_view anniversary_scan_job_type{
    "chronicle.anniversary-scan.v1"};
inline constexpr std::string_view chronicle_session_component_prefix{"sgs:1:"};
inline constexpr std::string_view chronicle_session_edit_prefix{"sgse:1:"};
inline constexpr std::string_view chronicle_search_component_prefix{"sgp:1:"};
inline constexpr std::string_view chronicle_search_page_prefix{"sgp:2:"};
inline constexpr std::string_view chronicle_title_page_prefix{"sgt:1:"};
inline constexpr std::size_t maximum_session_context_rows = 20;
inline constexpr std::size_t maximum_session_context_excerpt_bytes = 500;
inline constexpr std::size_t maximum_session_context_total_bytes = 12 * 1'024;
inline constexpr std::size_t maximum_session_linked_entries = 50;
inline constexpr std::size_t maximum_session_linked_events = 200;
inline constexpr std::size_t maximum_session_summary_input_bytes = 32 * 1'024;
inline constexpr std::int64_t session_context_retention_ms =
    24LL * 60 * 60 * 1'000;
inline constexpr std::int64_t chronicle_search_cursor_lifetime_ms =
    15LL * 60 * 1'000;
inline constexpr std::size_t chronicle_search_maximum_items = 50;
inline constexpr std::size_t chronicle_search_page_size = 5;
inline constexpr std::size_t chronicle_title_page_size = 5;

enum class ChronicleSessionState { open, closing, closed, abandoned };
enum class ChronicleSummaryState { pending, approved, rejected };
enum class ChronicleSummarySource { fallback, model, manual };
enum class ChronicleTitleState { proposed, active, rejected, revoked };
enum class ChronicleTitleProvenance { owner_curated, session_ai, tarot_system };

enum class SessionAction { close_with_entries, close_empty, finish_summary };
enum class SummaryAction { edit, approve, reject };
enum class TitleAction { approve, reject, feature, revoke };

[[nodiscard]] std::optional<ChronicleSessionState>
transition_session(ChronicleSessionState state, SessionAction action) noexcept;
[[nodiscard]] std::optional<ChronicleSummaryState>
transition_summary(ChronicleSummaryState state, SummaryAction action) noexcept;
[[nodiscard]] std::optional<ChronicleTitleState>
transition_title(ChronicleTitleState state, TitleAction action) noexcept;

struct ChronicleSession {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake opened_by_user_id;
  ChronicleSessionState state{ChronicleSessionState::open};
  std::int64_t opened_at_ms{};
  std::optional<std::int64_t> closing_at_ms;
  std::optional<std::int64_t> closed_at_ms;
  std::size_t revision{1};
  std::vector<DiscordSnowflake> participants;
  std::size_t linked_shared_canon_entries{};
  std::optional<std::string> draft_id;
  std::optional<ChronicleSummaryState> draft_state;
  std::optional<std::size_t> draft_revision;
};

struct SummaryTitleProposal {
  DiscordSnowflake recipient_user_id;
  std::string title;
  std::string description;
  std::optional<std::string> supporting_entry_id;
};

struct ChronicleSummaryCandidate {
  std::string chapter_title;
  std::string summary;
  std::vector<std::string> highlighted_entry_ids;
  std::vector<SummaryTitleProposal> proposed_titles;
};

struct ChronicleSummaryValidationContext {
  std::vector<DiscordSnowflake> opted_in_participants;
  std::vector<std::string> shared_entry_ids;
  std::vector<std::string> shared_entry_context;
  std::vector<std::string> transient_context;

  friend bool operator==(const ChronicleSummaryValidationContext &,
                         const ChronicleSummaryValidationContext &) = default;
};

enum class SummaryValidationCode {
  valid,
  invalid_utf8,
  invalid_length,
  unknown_participant,
  unknown_entry,
  duplicate_recipient,
  copied_context,
};

[[nodiscard]] SummaryValidationCode validate_summary_candidate(
    const ChronicleSummaryCandidate &candidate,
    const ChronicleSummaryValidationContext &context) noexcept;
[[nodiscard]] ChronicleSummaryCandidate
deterministic_summary_fallback(std::string_view session_id,
                               std::size_t shared_entry_count);

enum class ChronicleSessionResultCode {
  created,
  existing,
  updated,
  unchanged,
  not_found,
  unauthorized,
  opted_out,
  invalid_state,
  stale_revision,
  expired,
  invalid_input,
};

struct SessionMutationResult {
  ChronicleSessionResultCode code{ChronicleSessionResultCode::invalid_state};
  std::optional<ChronicleSession> session;
  bool wake_scheduler{};
  bool wake_outbox{};
};

struct StartSessionRequest {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  std::string event_id;
  std::string correlation_id;
  std::string idempotency_key;
  std::int64_t now_ms{};
};

struct CloseSessionRequest {
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string draft_id;
  std::string summary_job_id;
  std::string purge_job_id;
  std::string event_id;
  std::string correlation_id;
  std::string idempotency_key;
  std::int64_t now_ms{};
};

struct SessionContextObservation {
  std::string context_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake message_id;
  DiscordSnowflake author_user_id;
  std::string excerpt;
  std::string correlation_id;
  std::int64_t observed_at_ms{};
};

struct ChronicleSummaryDraft {
  std::string draft_id;
  std::string session_id;
  ChronicleSummaryState state{ChronicleSummaryState::pending};
  ChronicleSummaryCandidate content;
  ChronicleSummarySource source{ChronicleSummarySource::fallback};
  std::optional<std::string> model_failure_category;
  std::size_t revision{1};
};

struct SummaryEditRequest {
  std::string draft_id;
  std::size_t expected_revision{};
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string chapter_title;
  std::string summary;
  std::string event_id;
  std::string notice_id;
  std::string notice_token_id;
  std::string edit_token_id;
  std::string approve_token_id;
  std::string reject_token_id;
  std::string notice_outbox_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::optional<std::string> control_token_id;
  std::int64_t now_ms{};
};

struct SummaryDecisionRequest {
  std::string draft_id;
  std::size_t expected_revision{};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  bool approve{};
  std::string entry_id;
  std::string event_id;
  std::string outbox_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::optional<std::string> control_token_id;
  std::int64_t now_ms{};
};

struct SummaryControlResolution {
  std::string draft_id;
  std::string action;
  std::size_t expected_revision{};
};

struct SummaryJobCompletionRequest {
  struct TitleIds {
    std::string definition_id;
    std::string grant_id;
  };
  ClaimedScheduledJob job;
  ChronicleSummaryValidationContext generation_context;
  std::optional<ChronicleSummaryCandidate> candidate;
  std::optional<std::string> failure_category;
  std::vector<TitleIds> title_ids;
  std::string event_id;
  std::string notice_id;
  std::string notice_token_id;
  std::string edit_token_id;
  std::string approve_token_id;
  std::string reject_token_id;
  std::string notice_outbox_id;
  DiscordSnowflake owner_user_id;
  std::int64_t now_ms{};
};

struct ChronicleTitleGrant {
  std::string grant_id;
  DiscordSnowflake recipient_user_id;
  std::string title;
  std::string description;
  ChronicleTitleProvenance provenance{ChronicleTitleProvenance::owner_curated};
  ChronicleTitleState state{ChronicleTitleState::proposed};
  bool featured{};
  std::size_t revision{1};
};

struct ChronicleTitlePage {
  std::string cursor_id;
  std::size_t page{};
  std::size_t total{};
  std::vector<ChronicleTitleGrant> grants;
};

struct ProposeTitleRequest {
  std::string definition_id;
  std::string grant_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  DiscordSnowflake recipient_user_id;
  std::string title;
  std::string description;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

struct TitleMutationRequest {
  std::string grant_id;
  std::size_t expected_revision{};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  TitleAction action{TitleAction::approve};
  std::string award_entry_id;
  std::string event_id;
  std::string outbox_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

struct TitleMutationResult {
  ChronicleSessionResultCode code{ChronicleSessionResultCode::invalid_state};
  std::optional<ChronicleTitleGrant> grant;
  bool wake_outbox{};
};

struct ChronicleSearchFilter {
  std::string query;
  std::optional<DiscordSnowflake> participant;
  std::optional<std::string> entry_type;
  std::optional<std::int64_t> from_ms;
  std::optional<std::int64_t> to_ms;
  std::string presentation{"recall"};
};

struct ChronicleSearchItem {
  std::string item_id;
  std::string title;
  std::string excerpt;
  std::int64_t occurred_at_ms{};
};

struct ChronicleSearchPage {
  std::string cursor_id;
  std::size_t page{};
  std::size_t total{};
  std::vector<ChronicleSearchItem> items;
  std::optional<std::string> navigation_token_id;
  std::string presentation{"recall"};
};

struct AnniversaryScanResult {
  WorkMutationStatus status{WorkMutationStatus::invalid_state};
  bool wake_outbox{};
  std::optional<std::int64_t> next_due_at_ms;
};

class ChronicleSessionRepository {
public:
  virtual ~ChronicleSessionRepository() = default;
  [[nodiscard]] virtual SessionMutationResult
  start(const StartSessionRequest &request) = 0;
  [[nodiscard]] virtual SessionMutationResult
  close(const CloseSessionRequest &request) = 0;
  [[nodiscard]] virtual std::optional<ChronicleSession>
  status(const DiscordSnowflake &guild_id) = 0;
  virtual bool
  observe_context(const SessionContextObservation &observation) = 0;
  [[nodiscard]] virtual ChronicleSummaryValidationContext
  summary_context(std::string_view session_id) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  complete_summary_job(const SummaryJobCompletionRequest &request) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  purge_context_job(const ClaimedScheduledJob &job, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual SessionMutationResult
  edit_summary(const SummaryEditRequest &request) = 0;
  [[nodiscard]] virtual SessionMutationResult
  decide_summary(const SummaryDecisionRequest &request) = 0;
  [[nodiscard]] virtual std::optional<SummaryControlResolution>
  resolve_summary_control(std::string_view token_id,
                          const DiscordSnowflake &actor_user_id,
                          const DiscordSnowflake &guild_id,
                          const DiscordSnowflake &channel_id,
                          InteractionKind interaction_kind,
                          std::string_view interaction_idempotency_key,
                          std::int64_t now_ms) = 0;
  [[nodiscard]] virtual TitleMutationResult
  propose_title(const ProposeTitleRequest &request) = 0;
  [[nodiscard]] virtual TitleMutationResult
  mutate_title(const TitleMutationRequest &request) = 0;
  [[nodiscard]] virtual ChronicleTitlePage
  list_titles(const DiscordSnowflake &viewer, const DiscordSnowflake &target,
              bool owner_view, std::size_t page) = 0;
  [[nodiscard]] virtual ChronicleTitlePage
  begin_title_list(const DiscordSnowflake &viewer,
                   const DiscordSnowflake &target, bool owner_view,
                   std::string cursor_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual ChronicleTitlePage
  load_title_page(const DiscordSnowflake &viewer, std::string_view cursor_id,
                  std::size_t page, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual ChronicleSearchPage
  begin_search(const DiscordSnowflake &viewer,
               const ChronicleSearchFilter &filter, std::string cursor_id,
               std::int64_t now_ms) = 0;
  [[nodiscard]] virtual ChronicleSearchPage
  search_page(const DiscordSnowflake &viewer, std::string_view cursor_id,
              std::size_t page, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual ChronicleSearchPage
  advance_search(const DiscordSnowflake &viewer,
                 const DiscordSnowflake &guild_id,
                 const DiscordSnowflake &channel_id, std::string_view token_id,
                 std::string next_token_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool
  set_anniversary_reminders(const DiscordSnowflake &user_id, bool enabled,
                            std::int64_t now_ms) = 0;
  [[nodiscard]] virtual AnniversaryScanResult
  run_anniversary_scan(const ClaimedScheduledJob &job,
                       std::string_view timezone, bool test_run,
                       std::int64_t now_ms, PersistentIdGenerator &ids) = 0;
  [[nodiscard]] virtual bool
  queue_anniversary_scan(const ScheduledJobEnqueue &job,
                         const AnniversaryScanJobPayload &payload,
                         std::string_view correlation_id) = 0;
};

class ChronicleSessionService {
public:
  ChronicleSessionService(
      ChronicleSessionRepository &repository, const Clock &clock,
      PersistentIdGenerator &ids, ServerScopeConfiguration scope,
      ControlConfiguration controls, std::function<void()> scheduler_wakeup,
      std::function<void()> outbox_wakeup, std::string timezone,
      const AiClient *ai_client = nullptr, AiWorkService *ai_work = nullptr,
      DurableWorkRepository *durable_work = nullptr,
      Diagnostics *diagnostics = nullptr,
      std::function<void()> domain_event_wakeup = {});

  [[nodiscard]] SessionMutationResult
  start(const IncomingInteraction &interaction);
  [[nodiscard]] SessionMutationResult
  close(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  status(const IncomingInteraction &interaction);
  void observe_message(const IncomingMessage &message);
  [[nodiscard]] InteractionMessage
  edit_summary(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  decide_summary(const IncomingInteraction &interaction, bool approve);
  [[nodiscard]] InteractionMessage
  apply_summary_control(const IncomingInteraction &interaction);
  [[nodiscard]] static ModalPayload summary_edit_modal(std::string token_id);
  [[nodiscard]] InteractionMessage
  propose_title(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  mutate_title(const IncomingInteraction &interaction, TitleAction action);
  [[nodiscard]] InteractionMessage
  list_titles(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  search(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  timeline(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  advance_search(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  set_anniversaries(const IncomingInteraction &interaction, bool enabled);
  [[nodiscard]] AnniversaryScanResult
  handle_anniversary_job(const ClaimedScheduledJob &job, bool test_run = false);
  [[nodiscard]] WorkMutationStatus
  handle_context_purge(const ClaimedScheduledJob &job);
  [[nodiscard]] SubmitResult submit_summary_job(const ClaimedScheduledJob &job);
  [[nodiscard]] bool
  queue_test_anniversary(const IncomingInteraction &interaction);
  void ensure_anniversary_schedule();

private:
  ChronicleSessionRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  ServerScopeConfiguration scope_;
  ControlConfiguration controls_;
  std::function<void()> scheduler_wakeup_;
  std::function<void()> outbox_wakeup_;
  std::string timezone_;
  const AiClient *ai_client_{};
  AiWorkService *ai_work_{};
  DurableWorkRepository *durable_work_{};
  Diagnostics *diagnostics_{};
  std::function<void()> domain_event_wakeup_;

  [[nodiscard]] WorkMutationStatus
  complete_summary_fallback(const ClaimedScheduledJob &job,
                            std::string_view failure_category);
  void wake_domain_event() const;
};

[[nodiscard]] const AiRequest::JsonSchema &chronicle_summary_json_schema();

[[nodiscard]] const char *
chronicle_session_state_name(ChronicleSessionState state) noexcept;
[[nodiscard]] const char *
chronicle_summary_state_name(ChronicleSummaryState state) noexcept;
[[nodiscard]] const char *
chronicle_title_state_name(ChronicleTitleState state) noexcept;
[[nodiscard]] std::string literal_fts_query(std::string_view query);
[[nodiscard]] std::int64_t next_anniversary_scan_ms(std::int64_t now_ms,
                                                    std::string_view timezone);

} // namespace sanguinius
