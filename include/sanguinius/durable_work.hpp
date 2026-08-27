#pragma once

#include "sanguinius/discord_types.hpp"
#include "sanguinius/repositories.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view owner_test_notice_job_type{
    "owner_test.notice.v1"};
inline constexpr std::string_view pending_notice_outbox_kind{
    "notice.pending.v1"};
inline constexpr std::string_view public_discord_outbox_kind{
    "discord.public.v1"};
inline constexpr std::string_view test_public_retry_outbox_kind{
    "owner_test.discord_public_retry.v1"};

enum class ScheduledJobState {
  pending,
  claimed,
  completed,
  cancelled,
  dead,
};

enum class OutboxState {
  pending,
  claimed,
  delivered,
  failed,
  dead,
  cancelled,
};

struct EventJournalEntry {
  std::string event_id;
  std::string event_type;
  std::string aggregate_type;
  std::string aggregate_id;
  std::optional<DiscordSnowflake> actor_user_id;
  DiscordSnowflake guild_id;
  std::optional<DiscordSnowflake> channel_id;
  std::optional<DiscordSnowflake> source_message_id;
  std::int64_t occurred_at_ms{};
  std::int64_t recorded_at_ms{};
  std::string correlation_id;
  std::optional<std::string> causation_id;
  std::string idempotency_key;
  std::string payload_json{"{}"};
};

struct ScheduledJobEnqueue {
  std::string job_id;
  std::string job_type;
  std::optional<std::string> aggregate_type;
  std::optional<std::string> aggregate_id;
  std::int64_t due_at_ms{};
  std::size_t max_attempts{5};
  std::string idempotency_key;
  std::int64_t created_at_ms{};
};

struct OutboxEnqueue {
  std::string outbox_id;
  std::string kind;
  std::optional<std::string> aggregate_type;
  std::optional<std::string> aggregate_id;
  DiscordSnowflake target_guild_id;
  DiscordSnowflake target_channel_id;
  std::optional<DiscordSnowflake> target_user_id;
  std::int64_t available_at_ms{};
  std::size_t max_attempts{5};
  std::string idempotency_key;
  std::string provider_nonce;
  std::int64_t created_at_ms{};
};

struct NoticeOutboxPayload {
  CreatePendingNoticeRequest notice;
  bool announce_publicly{true};
};

struct PublicOutboxPayload {
  PublicMessageRequest request;
  bool fail_before_first_send{};
};

struct PublicEditOutboxPayload {
  PublicMessageRequest replacement;
  std::string source_outbox_id;
  std::size_t wager_revision{};
};

struct MemoryExpiryJobPayload {
  std::string memory_id;
  std::size_t expected_revision{};
};

struct SessionSummaryJobPayload {
  std::string session_id;
  std::string draft_id;
  std::size_t expected_session_revision{};
  std::size_t expected_draft_revision{};
};

struct SessionContextPurgeJobPayload {
  std::string session_id;
};

struct AnniversaryScanJobPayload {
  std::string local_date;
  bool test_run{};
};

struct AppearanceScanJobPayload {
  std::string policy_version;
};

struct AppearancePurgeJobPayload {
  std::string policy_version;
};

struct WagerDeadlineJobPayload {
  std::string wager_id;
  std::string phase;
  std::size_t expected_revision{};
};

struct HouseDeadlineJobPayload {
  std::string wager_id;
  std::size_t expected_revision{};
};

struct HouseOfferExpiryJobPayload {
  std::string offer_id;
};

struct TarotIntegrationScanJobPayload {
  std::string schedule_key;
};

struct TarotHouseWeeklyOfferJobPayload {
  std::string schedule_key;
  std::string catalog_version;
};

struct VoxTimeoutJobPayload {
  std::string session_id;
  std::size_t expected_revision{};
};

using DurablePayload =
    std::variant<std::monostate, NoticeOutboxPayload, PublicOutboxPayload,
                 PublicEditOutboxPayload, MemoryExpiryJobPayload,
                 SessionSummaryJobPayload, SessionContextPurgeJobPayload,
                 AnniversaryScanJobPayload, AppearanceScanJobPayload,
                 AppearancePurgeJobPayload, WagerDeadlineJobPayload,
                 HouseDeadlineJobPayload, HouseOfferExpiryJobPayload,
                 TarotIntegrationScanJobPayload,
                 TarotHouseWeeklyOfferJobPayload, VoxTimeoutJobPayload>;
// Appearance jobs carry only the policy version; excerpts and generated prose
// never enter durable scheduler payloads.

struct ClaimedScheduledJob {
  std::string job_id;
  std::string job_type;
  std::string lease_owner;
  std::string lease_token;
  std::size_t attempt_count{};
  std::size_t max_attempts{};
  std::int64_t due_at_ms{};
  DurablePayload payload;
  std::string correlation_id;
  std::optional<std::string> causation_event_id;
};

struct ClaimedOutboxMessage {
  std::string outbox_id;
  std::string kind;
  std::string lease_owner;
  std::string lease_token;
  std::size_t attempt_count{};
  std::size_t max_attempts{};
  std::int64_t available_at_ms{};
  std::optional<std::int64_t> first_attempt_at_ms;
  std::optional<std::int64_t> first_attempt_elapsed_ms;
  std::optional<std::string> first_attempt_boot_id;
  std::optional<std::int64_t> submission_started_at_ms;
  std::optional<std::string> last_error_code;
  std::string provider_nonce;
  DurablePayload payload;
  std::string correlation_id;
  std::optional<std::string> causation_event_id;
};

struct DeliveryAttemptStamp {
  std::int64_t wall_time_ms{};
  std::int64_t elapsed_realtime_ms{};
  std::string boot_session_id;
};

enum class WorkMutationStatus {
  applied,
  unchanged,
  stale_claim,
  not_found,
  invalid_state,
};

enum class OutboxFailureMode {
  retryable,
  failed,
  dead,
};

struct DurableWorkHealth {
  std::size_t pending_jobs{};
  std::size_t claimed_jobs{};
  std::size_t dead_jobs{};
  std::size_t pending_outbox{};
  std::size_t claimed_outbox{};
  std::size_t failed_outbox{};
  std::size_t dead_outbox{};
  std::size_t job_retries{};
  std::size_t outbox_retries{};
  std::int64_t scheduler_lag_ms{};
  std::int64_t outbox_lag_ms{};
  std::optional<std::string> last_job_error;
  std::optional<std::string> last_outbox_error;
};

struct WorkInspectionEntry {
  std::string category;
  std::string type;
  std::string state;
  std::string shortened_id;
  std::size_t attempts{};
  std::int64_t at_ms{};
  std::optional<std::string> error_code;
};

class DurableWorkRepository {
public:
  virtual ~DurableWorkRepository() = default;

  [[nodiscard]] virtual bool append_event(const EventJournalEntry &event) = 0;
  [[nodiscard]] virtual std::vector<EventJournalEntry>
  recent_events(std::size_t limit) = 0;
  [[nodiscard]] virtual std::vector<EventJournalEntry>
  events_by_type(std::string_view event_type, std::size_t limit) = 0;
  [[nodiscard]] virtual std::vector<EventJournalEntry>
  aggregate_history(std::string_view aggregate_type,
                    std::string_view aggregate_id, std::size_t limit) = 0;
  [[nodiscard]] virtual bool
  enqueue_notice(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const NoticeOutboxPayload &payload) = 0;
  [[nodiscard]] virtual bool
  schedule_notice(const EventJournalEntry &event,
                  const ScheduledJobEnqueue &job,
                  const NoticeOutboxPayload &payload) = 0;
  [[nodiscard]] virtual bool
  enqueue_public(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const PublicOutboxPayload &payload) = 0;

  [[nodiscard]] virtual std::optional<ClaimedScheduledJob>
  claim_due_job(std::int64_t now_ms, std::int64_t lease_until_ms,
                std::string lease_owner, std::string lease_token) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  complete_notice_job(const ClaimedScheduledJob &job,
                      const EventJournalEntry &event,
                      const OutboxEnqueue &outbox, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  release_job(const ClaimedScheduledJob &job, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  defer_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
            std::int64_t retry_at_ms, std::string error_code) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  reschedule_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
                 std::int64_t due_at_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  extend_job_lease(const ClaimedScheduledJob &job, std::int64_t now_ms,
                   std::int64_t lease_until_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  fail_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
           std::int64_t retry_at_ms, std::string error_code,
           bool retryable) = 0;
  [[nodiscard]] virtual WorkMutationStatus cancel_job(std::string_view job_id,
                                                      std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  cancel_claimed_job(const ClaimedScheduledJob &job, std::int64_t now_ms) = 0;

  [[nodiscard]] virtual std::optional<ClaimedOutboxMessage>
  claim_due_outbox(std::int64_t now_ms, std::int64_t lease_until_ms,
                   std::string lease_owner, std::string lease_token,
                   bool public_delivery_ready) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  complete_notice_outbox(const ClaimedOutboxMessage &outbox,
                         const EventJournalEntry &event,
                         std::optional<OutboxEnqueue> public_outbox,
                         std::optional<PublicOutboxPayload> public_payload,
                         std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  release_outbox(const ClaimedOutboxMessage &outbox, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  mark_public_outbox_submitted(const ClaimedOutboxMessage &outbox,
                               const DeliveryAttemptStamp &attempt,
                               std::int64_t lease_until_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  complete_public_outbox(const ClaimedOutboxMessage &outbox,
                         DiscordSnowflake provider_message_id,
                         std::int64_t now_ms) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  fail_outbox(const ClaimedOutboxMessage &outbox, std::int64_t now_ms,
              std::int64_t retry_at_ms, std::string error_code,
              OutboxFailureMode mode) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  cancel_outbox(std::string_view outbox_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::optional<DiscordSnowflake>
  delivered_provider_message_id(std::string_view) {
    return std::nullopt;
  }

  [[nodiscard]] virtual DurableWorkHealth health(std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::vector<WorkInspectionEntry>
  recent(std::size_t limit) = 0;
  [[nodiscard]] virtual std::vector<WorkInspectionEntry>
  dead(std::size_t limit) = 0;
};

[[nodiscard]] const char *
scheduled_job_state_name(ScheduledJobState state) noexcept;
[[nodiscard]] const char *outbox_state_name(OutboxState state) noexcept;
[[nodiscard]] std::string discord_nonce_from_uuid(std::string_view uuid);
[[nodiscard]] std::string shortened_persistent_id(std::string_view value);

} // namespace sanguinius
