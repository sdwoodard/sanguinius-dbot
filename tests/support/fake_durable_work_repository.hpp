#pragma once

#include "sanguinius/durable_work.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeDurableWorkRepository final : public DurableWorkRepository {
public:
  explicit FakeDurableWorkRepository(PendingNoticeRepository &notices)
      : notices_{notices} {}

  [[nodiscard]] bool append_event(const EventJournalEntry &event) override {
    const std::scoped_lock lock{mutex_};
    return append_event_unlocked(event);
  }

  [[nodiscard]] std::vector<EventJournalEntry>
  recent_events(const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    return filtered_events(limit,
                           [](const EventJournalEntry &) { return true; });
  }

  [[nodiscard]] std::vector<EventJournalEntry>
  events_by_type(const std::string_view event_type,
                 const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    return filtered_events(limit, [event_type](const EventJournalEntry &event) {
      return event.event_type == event_type;
    });
  }

  [[nodiscard]] std::vector<EventJournalEntry>
  aggregate_history(const std::string_view aggregate_type,
                    const std::string_view aggregate_id,
                    const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    return filtered_events(
        limit, [aggregate_type, aggregate_id](const EventJournalEntry &event) {
          return event.aggregate_type == aggregate_type &&
                 event.aggregate_id == aggregate_id;
        });
  }

  [[nodiscard]] bool
  enqueue_notice(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const NoticeOutboxPayload &payload) override {
    const std::scoped_lock lock{mutex_};
    if (!append_event_unlocked(event)) {
      return false;
    }
    outbox_.push_back({outbox, payload, event.correlation_id, event.event_id});
    changed_.notify_all();
    return true;
  }

  [[nodiscard]] bool
  schedule_notice(const EventJournalEntry &event,
                  const ScheduledJobEnqueue &job,
                  const NoticeOutboxPayload &payload) override {
    const std::scoped_lock lock{mutex_};
    if (!append_event_unlocked(event)) {
      return false;
    }
    jobs_.push_back({job, payload, event.correlation_id, event.event_id});
    changed_.notify_all();
    return true;
  }

  [[nodiscard]] bool
  enqueue_public(const EventJournalEntry &event, const OutboxEnqueue &outbox,
                 const PublicOutboxPayload &payload) override {
    const std::scoped_lock lock{mutex_};
    if (!append_event_unlocked(event)) {
      return false;
    }
    outbox_.push_back({outbox, payload, event.correlation_id, event.event_id});
    changed_.notify_all();
    return true;
  }

  void seed_job(ScheduledJobEnqueue job, DurablePayload payload,
                std::string correlation_id = "seeded-job") {
    const std::scoped_lock lock{mutex_};
    jobs_.push_back(
        {std::move(job), std::move(payload), std::move(correlation_id), {}});
    changed_.notify_all();
  }

  [[nodiscard]] std::optional<ClaimedScheduledJob>
  claim_due_job(const std::int64_t now_ms, const std::int64_t lease_until_ms,
                std::string lease_owner, std::string lease_token) override {
    const std::scoped_lock lock{mutex_};
    JobRow *selected{};
    for (auto &row : jobs_) {
      if ((row.state == ScheduledJobState::pending &&
           row.enqueue.due_at_ms <= now_ms) ||
          (row.state == ScheduledJobState::claimed &&
           row.lease_until_ms <= now_ms)) {
        if (row.attempts >= row.enqueue.max_attempts) {
          row.state = ScheduledJobState::dead;
          row.error = "attempts_exhausted";
          continue;
        }
        if (selected == nullptr ||
            std::pair{row.enqueue.due_at_ms, row.enqueue.job_id} <
                std::pair{selected->enqueue.due_at_ms,
                          selected->enqueue.job_id}) {
          selected = &row;
        }
      }
    }
    if (selected == nullptr) {
      return std::nullopt;
    }
    selected->state = ScheduledJobState::claimed;
    selected->lease_owner = lease_owner;
    selected->lease_token = lease_token;
    selected->lease_until_ms = lease_until_ms;
    ++selected->attempts;
    return ClaimedScheduledJob{
        .job_id = selected->enqueue.job_id,
        .job_type = selected->enqueue.job_type,
        .lease_owner = std::move(lease_owner),
        .lease_token = std::move(lease_token),
        .attempt_count = selected->attempts,
        .max_attempts = selected->enqueue.max_attempts,
        .due_at_ms = selected->enqueue.due_at_ms,
        .payload = selected->payload,
        .correlation_id = selected->correlation_id,
        .causation_event_id = selected->causation_event_id,
    };
  }

  [[nodiscard]] WorkMutationStatus complete_notice_job(
      const ClaimedScheduledJob &job, const EventJournalEntry &event,
      const OutboxEnqueue &outbox, const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    if (event.correlation_id != job.correlation_id ||
        event.causation_id != job.causation_event_id) {
      return WorkMutationStatus::invalid_state;
    }
    auto *row = find_job(job.job_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, job.lease_token)) {
      return row->state == ScheduledJobState::completed
                 ? WorkMutationStatus::unchanged
                 : WorkMutationStatus::stale_claim;
    }
    static_cast<void>(append_event_unlocked(event));
    const auto payload = std::get<NoticeOutboxPayload>(row->payload);
    row->state = ScheduledJobState::completed;
    outbox_.push_back({outbox, payload, job.correlation_id, event.event_id});
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus fail_job(const ClaimedScheduledJob &job,
                                            const std::int64_t,
                                            const std::int64_t retry_at_ms,
                                            std::string error_code,
                                            const bool retryable) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, job.lease_token)) {
      return WorkMutationStatus::stale_claim;
    }
    row->error = std::move(error_code);
    row->enqueue.due_at_ms = retry_at_ms;
    row->state = retryable && row->attempts < row->enqueue.max_attempts
                     ? ScheduledJobState::pending
                     : ScheduledJobState::dead;
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus release_job(const ClaimedScheduledJob &job,
                                               const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, job.lease_token)) {
      return WorkMutationStatus::stale_claim;
    }
    row->state = ScheduledJobState::pending;
    if (row->attempts > 0) {
      --row->attempts;
    }
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus defer_job(const ClaimedScheduledJob &job,
                                             const std::int64_t now_ms,
                                             const std::int64_t retry_at_ms,
                                             std::string error_code) override {
    if (retry_at_ms <= now_ms) {
      throw std::invalid_argument{"A deferred job must move into the future."};
    }
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr)
      return WorkMutationStatus::not_found;
    if (!current_claim(*row, job.lease_token))
      return WorkMutationStatus::stale_claim;
    row->state = ScheduledJobState::pending;
    row->enqueue.due_at_ms = retry_at_ms;
    row->error = std::move(error_code);
    row->lease_owner.clear();
    row->lease_token.clear();
    row->lease_until_ms = 0;
    if (row->attempts > 0)
      --row->attempts;
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  reschedule_job(const ClaimedScheduledJob &job, const std::int64_t now_ms,
                 const std::int64_t due_at_ms) override {
    if (due_at_ms <= now_ms) {
      throw std::invalid_argument{
          "A rescheduled job must move into the future."};
    }
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr)
      return WorkMutationStatus::not_found;
    if (!current_claim(*row, job.lease_token))
      return WorkMutationStatus::stale_claim;
    row->state = ScheduledJobState::pending;
    row->enqueue.due_at_ms = due_at_ms;
    row->error.clear();
    row->lease_owner.clear();
    row->lease_token.clear();
    row->lease_until_ms = 0;
    if (row->attempts > 0)
      --row->attempts;
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  extend_job_lease(const ClaimedScheduledJob &job, const std::int64_t now_ms,
                   const std::int64_t lease_until_ms) override {
    if (lease_until_ms <= now_ms) {
      throw std::invalid_argument{
          "A renewed job lease must remain in the future."};
    }
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr)
      return WorkMutationStatus::not_found;
    if (!current_claim(*row, job.lease_token))
      return WorkMutationStatus::stale_claim;
    row->lease_until_ms = std::max(row->lease_until_ms, lease_until_ms);
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus cancel_job(const std::string_view job_id,
                                              const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    row->state = ScheduledJobState::cancelled;
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  cancel_claimed_job(const ClaimedScheduledJob &job,
                     const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_job(job.job_id);
    if (row == nullptr)
      return WorkMutationStatus::not_found;
    if (!current_claim(*row, job.lease_token))
      return WorkMutationStatus::stale_claim;
    row->state = ScheduledJobState::cancelled;
    row->lease_owner.clear();
    row->lease_token.clear();
    row->lease_until_ms = 0;
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] std::optional<ClaimedOutboxMessage>
  claim_due_outbox(const std::int64_t now_ms, const std::int64_t lease_until_ms,
                   std::string lease_owner, std::string lease_token,
                   const bool public_delivery_ready) override {
    const std::scoped_lock lock{mutex_};
    OutboxRow *selected{};
    for (auto &row : outbox_) {
      const bool known_public =
          row.enqueue.kind == public_discord_outbox_kind ||
          row.enqueue.kind == test_public_retry_outbox_kind;
      const bool due =
          (row.state == OutboxState::pending &&
           row.enqueue.available_at_ms <= now_ms) ||
          (row.state == OutboxState::claimed && row.lease_until_ms <= now_ms);
      if (due && known_public && row.state == OutboxState::claimed &&
          row.attempts >= row.enqueue.max_attempts &&
          row.submission_started.has_value()) {
        row.state = OutboxState::failed;
        row.submission_started = std::nullopt;
        row.error = "discord_unknown_outcome_stale";
        changed_.notify_all();
        continue;
      }
      if (!public_delivery_ready && known_public) {
        continue;
      }
      if (due) {
        if (row.attempts >= row.enqueue.max_attempts) {
          row.state = OutboxState::dead;
          row.error = "attempts_exhausted";
          continue;
        }
        if (selected == nullptr ||
            std::pair{row.enqueue.available_at_ms, row.enqueue.outbox_id} <
                std::pair{selected->enqueue.available_at_ms,
                          selected->enqueue.outbox_id}) {
          selected = &row;
        }
      }
    }
    if (selected == nullptr) {
      return std::nullopt;
    }
    selected->state = OutboxState::claimed;
    selected->lease_owner = lease_owner;
    selected->lease_token = lease_token;
    selected->lease_until_ms = lease_until_ms;
    ++selected->attempts;
    return ClaimedOutboxMessage{
        .outbox_id = selected->enqueue.outbox_id,
        .kind = selected->enqueue.kind,
        .lease_owner = std::move(lease_owner),
        .lease_token = std::move(lease_token),
        .attempt_count = selected->attempts,
        .max_attempts = selected->enqueue.max_attempts,
        .available_at_ms = selected->enqueue.available_at_ms,
        .first_attempt_at_ms = selected->first_attempt,
        .first_attempt_elapsed_ms = selected->first_attempt_elapsed,
        .first_attempt_boot_id = selected->first_attempt_boot,
        .submission_started_at_ms = selected->submission_started,
        .last_error_code = selected->error.empty()
                               ? std::nullopt
                               : std::optional<std::string>{selected->error},
        .provider_nonce = selected->enqueue.provider_nonce,
        .payload = selected->payload,
        .correlation_id = selected->correlation_id,
        .causation_event_id = selected->causation_event_id,
    };
  }

  [[nodiscard]] WorkMutationStatus
  complete_notice_outbox(const ClaimedOutboxMessage &outbox,
                         const EventJournalEntry &event,
                         std::optional<OutboxEnqueue> public_outbox,
                         std::optional<PublicOutboxPayload> public_payload,
                         const std::int64_t) override {
    if (event.correlation_id != outbox.correlation_id ||
        event.causation_id != outbox.causation_event_id) {
      return WorkMutationStatus::invalid_state;
    }
    NoticeOutboxPayload notice_payload;
    {
      const std::scoped_lock lock{mutex_};
      auto *row = find_outbox(outbox.outbox_id);
      if (row == nullptr) {
        return WorkMutationStatus::not_found;
      }
      if (!current_claim(*row, outbox.lease_token)) {
        return row->state == OutboxState::delivered
                   ? WorkMutationStatus::unchanged
                   : WorkMutationStatus::stale_claim;
      }
      notice_payload = std::get<NoticeOutboxPayload>(row->payload);
      static_cast<void>(append_event_unlocked(event));
      if (public_outbox.has_value() && public_payload.has_value()) {
        outbox_.push_back({*public_outbox, *public_payload,
                           outbox.correlation_id, event.event_id});
      }
      row = find_outbox(outbox.outbox_id);
      row->state = OutboxState::delivered;
      row->submission_started = std::nullopt;
    }
    static_cast<void>(notices_.create_with_token(notice_payload.notice));
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  complete_public_outbox(const ClaimedOutboxMessage &outbox,
                         const DiscordSnowflake provider_message_id,
                         const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_outbox(outbox.outbox_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, outbox.lease_token)) {
      return row->state == OutboxState::delivered
                 ? WorkMutationStatus::unchanged
                 : WorkMutationStatus::stale_claim;
    }
    if (!row->submission_started.has_value()) {
      return WorkMutationStatus::stale_claim;
    }
    row->provider_message_id = provider_message_id;
    row->state = OutboxState::delivered;
    row->submission_started = std::nullopt;
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  mark_public_outbox_submitted(const ClaimedOutboxMessage &outbox,
                               const DeliveryAttemptStamp &attempt,
                               const std::int64_t lease_until_ms) override {
    if (lease_until_ms <= attempt.wall_time_ms) {
      throw std::invalid_argument{
          "A submitted outbox lease must extend beyond its attempt time."};
    }
    const std::scoped_lock lock{mutex_};
    auto *row = find_outbox(outbox.outbox_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, outbox.lease_token)) {
      return row->state == OutboxState::delivered
                 ? WorkMutationStatus::unchanged
                 : WorkMutationStatus::stale_claim;
    }
    if (row->enqueue.kind != public_discord_outbox_kind &&
        row->enqueue.kind != test_public_retry_outbox_kind) {
      return WorkMutationStatus::stale_claim;
    }
    if (!row->first_attempt.has_value()) {
      row->first_attempt = attempt.wall_time_ms;
      row->first_attempt_elapsed = attempt.elapsed_realtime_ms;
      row->first_attempt_boot = attempt.boot_session_id;
    }
    row->submission_started = attempt.wall_time_ms;
    row->lease_until_ms = std::max(row->lease_until_ms, lease_until_ms);
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  release_outbox(const ClaimedOutboxMessage &outbox,
                 const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_outbox(outbox.outbox_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, outbox.lease_token)) {
      return WorkMutationStatus::stale_claim;
    }
    if (row->submission_started.has_value()) {
      return WorkMutationStatus::stale_claim;
    }
    row->state = OutboxState::pending;
    if (row->attempts > 0) {
      --row->attempts;
    }
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  fail_outbox(const ClaimedOutboxMessage &outbox, const std::int64_t,
              const std::int64_t retry_at_ms, std::string error_code,
              const OutboxFailureMode mode) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_outbox(outbox.outbox_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (!current_claim(*row, outbox.lease_token)) {
      return WorkMutationStatus::stale_claim;
    }
    row->error = std::move(error_code);
    row->enqueue.available_at_ms = retry_at_ms;
    row->submission_started = std::nullopt;
    if (mode == OutboxFailureMode::failed) {
      row->state = OutboxState::failed;
    } else if (mode == OutboxFailureMode::dead ||
               row->attempts >= row->enqueue.max_attempts) {
      row->state = OutboxState::dead;
    } else {
      row->state = OutboxState::pending;
    }
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] WorkMutationStatus
  cancel_outbox(const std::string_view outbox_id, const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    auto *row = find_outbox(outbox_id);
    if (row == nullptr) {
      return WorkMutationStatus::not_found;
    }
    if (row->state == OutboxState::cancelled) {
      return WorkMutationStatus::unchanged;
    }
    if (row->state != OutboxState::pending) {
      return WorkMutationStatus::invalid_state;
    }
    row->state = OutboxState::cancelled;
    changed_.notify_all();
    return WorkMutationStatus::applied;
  }

  [[nodiscard]] DurableWorkHealth health(const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    DurableWorkHealth result;
    for (const auto &row : jobs_) {
      result.pending_jobs += row.state == ScheduledJobState::pending ? 1U : 0U;
      result.claimed_jobs += row.state == ScheduledJobState::claimed ? 1U : 0U;
      result.dead_jobs += row.state == ScheduledJobState::dead ? 1U : 0U;
      result.job_retries += row.attempts > 0 ? row.attempts - 1 : 0;
      const bool due = row.state == ScheduledJobState::pending &&
                       row.enqueue.due_at_ms <= now_ms;
      const bool expired = row.state == ScheduledJobState::claimed &&
                           row.lease_until_ms <= now_ms;
      if (due || expired) {
        result.scheduler_lag_ms =
            std::max(result.scheduler_lag_ms, now_ms - row.enqueue.due_at_ms);
      }
      if (!row.error.empty()) {
        result.last_job_error = row.error;
      }
    }
    for (const auto &row : outbox_) {
      result.pending_outbox += row.state == OutboxState::pending ? 1U : 0U;
      result.claimed_outbox += row.state == OutboxState::claimed ? 1U : 0U;
      result.failed_outbox += row.state == OutboxState::failed ? 1U : 0U;
      result.dead_outbox += row.state == OutboxState::dead ? 1U : 0U;
      result.outbox_retries += row.attempts > 0 ? row.attempts - 1 : 0;
      if (row.state == OutboxState::pending &&
          row.enqueue.available_at_ms < now_ms) {
        result.outbox_lag_ms = std::max(result.outbox_lag_ms,
                                        now_ms - row.enqueue.available_at_ms);
      }
      if (!row.error.empty()) {
        result.last_outbox_error = row.error;
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<WorkInspectionEntry>
  recent(const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    std::vector<WorkInspectionEntry> result;
    for (auto iterator = events_.rbegin();
         iterator != events_.rend() && result.size() < limit; ++iterator) {
      result.push_back({"event", iterator->event_type, "recorded",
                        shortened_persistent_id(iterator->event_id), 0,
                        iterator->recorded_at_ms, std::nullopt});
    }
    return result;
  }

  [[nodiscard]] std::vector<WorkInspectionEntry>
  dead(const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    std::vector<WorkInspectionEntry> result;
    for (const auto &row : jobs_) {
      if (row.state == ScheduledJobState::dead && result.size() < limit) {
        result.push_back({"job", row.enqueue.job_type, "dead",
                          shortened_persistent_id(row.enqueue.job_id),
                          row.attempts, row.enqueue.due_at_ms, row.error});
      }
    }
    for (const auto &row : outbox_) {
      if ((row.state == OutboxState::failed ||
           row.state == OutboxState::dead) &&
          result.size() < limit) {
        result.push_back({"outbox", row.enqueue.kind,
                          row.state == OutboxState::failed ? "failed" : "dead",
                          shortened_persistent_id(row.enqueue.outbox_id),
                          row.attempts, row.enqueue.available_at_ms,
                          row.error});
      }
    }
    return result;
  }

  [[nodiscard]] std::size_t event_count() const {
    const std::scoped_lock lock{mutex_};
    return events_.size();
  }

  [[nodiscard]] std::optional<std::int64_t>
  job_due_at(const std::string_view job_type) const {
    const std::scoped_lock lock{mutex_};
    const auto found =
        std::ranges::find(jobs_, job_type, [](const JobRow &row) {
          return std::string_view{row.enqueue.job_type};
        });
    return found == jobs_.end()
               ? std::nullopt
               : std::optional<std::int64_t>{found->enqueue.due_at_ms};
  }

  [[nodiscard]] bool
  wait_for_job_due(const std::string_view job_type,
                   const std::int64_t due_at_ms,
                   const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, job_type, due_at_ms] {
      return std::ranges::any_of(jobs_, [&](const JobRow &row) {
        return row.enqueue.job_type == job_type &&
               row.enqueue.due_at_ms == due_at_ms &&
               row.state == ScheduledJobState::pending;
      });
    });
  }

  [[nodiscard]] bool
  wait_for_outbox_error(const std::string_view error,
                        const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, error] {
      return std::ranges::any_of(outbox_, [error](const OutboxRow &row) {
        return row.error == error;
      });
    });
  }

  [[nodiscard]] bool
  wait_for_job_error(const std::string_view error,
                     const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, error] {
      return std::ranges::any_of(
          jobs_, [error](const JobRow &row) { return row.error == error; });
    });
  }

  [[nodiscard]] bool
  wait_for_outbox_idle(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] {
      return std::ranges::none_of(outbox_, [](const OutboxRow &row) {
        return row.state == OutboxState::pending ||
               row.state == OutboxState::claimed;
      });
    });
  }

private:
  struct JobRow {
    JobRow(ScheduledJobEnqueue enqueue_value, DurablePayload payload_value,
           std::string correlation_value, std::string causation_value)
        : enqueue{std::move(enqueue_value)}, payload{std::move(payload_value)},
          correlation_id{std::move(correlation_value)},
          causation_event_id{std::move(causation_value)} {}

    ScheduledJobEnqueue enqueue;
    DurablePayload payload;
    std::string correlation_id;
    std::optional<std::string> causation_event_id;
    ScheduledJobState state{ScheduledJobState::pending};
    std::size_t attempts{};
    std::string lease_owner;
    std::string lease_token;
    std::int64_t lease_until_ms{};
    std::string error;
  };

  struct OutboxRow {
    OutboxRow(OutboxEnqueue enqueue_value, DurablePayload payload_value,
              std::string correlation_value, std::string causation_value)
        : enqueue{std::move(enqueue_value)}, payload{std::move(payload_value)},
          correlation_id{std::move(correlation_value)},
          causation_event_id{std::move(causation_value)} {}

    OutboxEnqueue enqueue;
    DurablePayload payload;
    std::string correlation_id;
    std::optional<std::string> causation_event_id;
    OutboxState state{OutboxState::pending};
    std::size_t attempts{};
    std::string lease_owner;
    std::string lease_token;
    std::int64_t lease_until_ms{};
    std::optional<std::int64_t> first_attempt;
    std::optional<std::int64_t> first_attempt_elapsed;
    std::optional<std::string> first_attempt_boot;
    std::optional<std::int64_t> submission_started;
    std::optional<DiscordSnowflake> provider_message_id;
    std::string error;
  };

  [[nodiscard]] bool append_event_unlocked(const EventJournalEntry &event) {
    if (event_keys_.contains(event.idempotency_key)) {
      return false;
    }
    event_keys_.emplace(event.idempotency_key, event.event_id);
    events_.push_back(event);
    return true;
  }

  template <typename Predicate>
  [[nodiscard]] std::vector<EventJournalEntry>
  filtered_events(const std::size_t limit, Predicate predicate) const {
    std::vector<EventJournalEntry> result;
    for (auto iterator = events_.rbegin();
         iterator != events_.rend() && result.size() < limit; ++iterator) {
      if (predicate(*iterator)) {
        result.push_back(*iterator);
      }
    }
    return result;
  }

  [[nodiscard]] JobRow *find_job(const std::string_view id) {
    for (auto &row : jobs_) {
      if (row.enqueue.job_id == id) {
        return &row;
      }
    }
    return nullptr;
  }

  [[nodiscard]] OutboxRow *find_outbox(const std::string_view id) {
    for (auto &row : outbox_) {
      if (row.enqueue.outbox_id == id) {
        return &row;
      }
    }
    return nullptr;
  }

  [[nodiscard]] static bool current_claim(const JobRow &row,
                                          const std::string_view token) {
    return row.state == ScheduledJobState::claimed && row.lease_token == token;
  }

  [[nodiscard]] static bool current_claim(const OutboxRow &row,
                                          const std::string_view token) {
    return row.state == OutboxState::claimed && row.lease_token == token;
  }

  PendingNoticeRepository &notices_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<EventJournalEntry> events_;
  std::unordered_map<std::string, std::string> event_keys_;
  std::vector<JobRow> jobs_;
  std::vector<OutboxRow> outbox_;
};

} // namespace sanguinius::test
