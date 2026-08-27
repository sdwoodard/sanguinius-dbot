#pragma once

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteDurableWorkRepository final : public DurableWorkRepository {
public:
  explicit SqliteDurableWorkRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] bool append_event(const EventJournalEntry &event) override;
  [[nodiscard]] std::vector<EventJournalEntry>
  recent_events(std::size_t limit) override;
  [[nodiscard]] std::vector<EventJournalEntry>
  events_by_type(std::string_view event_type, std::size_t limit) override;
  [[nodiscard]] std::vector<EventJournalEntry>
  aggregate_history(std::string_view aggregate_type,
                    std::string_view aggregate_id, std::size_t limit) override;
  [[nodiscard]] bool
  enqueue_notice(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const NoticeOutboxPayload &payload) override;
  [[nodiscard]] bool
  schedule_notice(const EventJournalEntry &event,
                  const ScheduledJobEnqueue &job,
                  const NoticeOutboxPayload &payload) override;
  [[nodiscard]] bool
  enqueue_public(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const PublicOutboxPayload &payload) override;

  [[nodiscard]] std::optional<ClaimedScheduledJob>
  claim_due_job(std::int64_t now_ms, std::int64_t lease_until_ms,
                std::string lease_owner, std::string lease_token) override;
  [[nodiscard]] WorkMutationStatus complete_notice_job(
      const ClaimedScheduledJob &job, const EventJournalEntry &event,
      const OutboxEnqueue &outbox, std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus release_job(const ClaimedScheduledJob &job,
                                               std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus defer_job(const ClaimedScheduledJob &job,
                                             std::int64_t now_ms,
                                             std::int64_t retry_at_ms,
                                             std::string error_code) override;
  [[nodiscard]] WorkMutationStatus
  reschedule_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
                 std::int64_t due_at_ms) override;
  [[nodiscard]] WorkMutationStatus
  extend_job_lease(const ClaimedScheduledJob &job, std::int64_t now_ms,
                   std::int64_t lease_until_ms) override;
  [[nodiscard]] WorkMutationStatus fail_job(const ClaimedScheduledJob &job,
                                            std::int64_t now_ms,
                                            std::int64_t retry_at_ms,
                                            std::string error_code,
                                            bool retryable) override;
  [[nodiscard]] WorkMutationStatus cancel_job(std::string_view job_id,
                                              std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus
  cancel_claimed_job(const ClaimedScheduledJob &job,
                     std::int64_t now_ms) override;

  [[nodiscard]] std::optional<ClaimedOutboxMessage>
  claim_due_outbox(std::int64_t now_ms, std::int64_t lease_until_ms,
                   std::string lease_owner, std::string lease_token,
                   bool public_delivery_ready) override;
  [[nodiscard]] WorkMutationStatus
  complete_notice_outbox(const ClaimedOutboxMessage &outbox,
                         const EventJournalEntry &event,
                         std::optional<OutboxEnqueue> public_outbox,
                         std::optional<PublicOutboxPayload> public_payload,
                         std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus
  release_outbox(const ClaimedOutboxMessage &outbox,
                 std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus
  mark_public_outbox_submitted(const ClaimedOutboxMessage &outbox,
                               const DeliveryAttemptStamp &attempt,
                               std::int64_t lease_until_ms) override;
  [[nodiscard]] WorkMutationStatus
  complete_public_outbox(const ClaimedOutboxMessage &outbox,
                         DiscordSnowflake provider_message_id,
                         std::int64_t now_ms) override;
  [[nodiscard]] WorkMutationStatus
  fail_outbox(const ClaimedOutboxMessage &outbox, std::int64_t now_ms,
              std::int64_t retry_at_ms, std::string error_code,
              OutboxFailureMode mode) override;
  [[nodiscard]] WorkMutationStatus cancel_outbox(std::string_view outbox_id,
                                                 std::int64_t now_ms) override;
  [[nodiscard]] std::optional<DiscordSnowflake>
  delivered_provider_message_id(std::string_view outbox_id) override;

  [[nodiscard]] DurableWorkHealth health(std::int64_t now_ms) override;
  [[nodiscard]] std::vector<WorkInspectionEntry>
  recent(std::size_t limit) override;
  [[nodiscard]] std::vector<WorkInspectionEntry>
  dead(std::size_t limit) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
