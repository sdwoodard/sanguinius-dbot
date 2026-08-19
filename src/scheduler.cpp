#include "sanguinius/scheduler.hpp"

#include "sanguinius/chronicle.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr auto poll_interval = std::chrono::seconds{1};
constexpr std::int64_t lease_duration_ms = 120'000;
constexpr std::size_t maximum_cycle_submissions = 16;

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t retry_delay_ms(const std::size_t attempt) {
  constexpr std::int64_t delays[]{5'000, 10'000, 20'000, 40'000};
  const auto index =
      attempt == 0 ? 0 : std::min(attempt - 1, std::size(delays) - 1);
  return delays[index];
}

} // namespace

void JobHandlerRegistry::add(std::string type, Handler handler) {
  if (frozen_) {
    throw std::logic_error{"Job handler registry is frozen."};
  }
  if (type.empty() || !handler ||
      !handlers_.emplace(std::move(type), std::move(handler)).second) {
    throw std::invalid_argument{
        "Job handler registration is invalid or duplicate."};
  }
}

void JobHandlerRegistry::freeze() { frozen_ = true; }

bool JobHandlerRegistry::dispatch(const ClaimedScheduledJob &job) const {
  if (!frozen_) {
    throw std::logic_error{"Job handler registry is not frozen."};
  }
  const auto found = handlers_.find(job.job_type);
  if (found == handlers_.end()) {
    return false;
  }
  found->second(job);
  return true;
}

SchedulerService::SchedulerService(DurableWorkRepository &repository,
                                   const Clock &clock,
                                   PersistentIdGenerator &ids,
                                   Diagnostics &diagnostics,
                                   std::string instance_id,
                                   std::function<void()> outbox_wakeup,
                                   const std::size_t queue_capacity,
                                   JobHandlerRegistry::Handler chronicle_expiry_handler)
    : repository_{repository}, clock_{clock}, ids_{ids},
      diagnostics_{diagnostics}, instance_id_{std::move(instance_id)},
      outbox_wakeup_{std::move(outbox_wakeup)}, workers_{queue_capacity, 1} {
  if (instance_id_.empty() || !outbox_wakeup_) {
    throw std::invalid_argument{"Scheduler dependencies are incomplete."};
  }
  handlers_.add(
      std::string{owner_test_notice_job_type},
      [this](const ClaimedScheduledJob &job) { handle_test_notice(job); });
  if (chronicle_expiry_handler) {
    handlers_.add(std::string{memory_expiry_job_type},
                  std::move(chronicle_expiry_handler));
  }
}

SchedulerService::~SchedulerService() { stop(); }

void SchedulerService::start() {
  if (started_) {
    throw std::logic_error{"Scheduler may only be started once."};
  }
  handlers_.freeze();
  workers_.start();
  started_ = true;
  poller_ = std::jthread{
      [this](const std::stop_token stop_token) { poll(stop_token); }};
}

void SchedulerService::add_handler(std::string type,
                                   JobHandlerRegistry::Handler handler) {
  if (started_)
    throw std::logic_error{"Scheduler handlers must be registered before start."};
  handlers_.add(std::move(type), std::move(handler));
}

void SchedulerService::stop() noexcept {
  try {
    if (!started_) {
      return;
    }
    poller_.request_stop();
    poll_wakeup_.notify_all();
    if (poller_.joinable()) {
      poller_.join();
    }
    workers_.stop();
    started_ = false;
  } catch (...) {
  }
}

void SchedulerService::wake() noexcept { poll_wakeup_.notify_all(); }

void SchedulerService::run_one_cycle() {
  const auto snapshot = workers_.snapshot();
  if (!snapshot.accepting ||
      snapshot.queued + snapshot.active >= snapshot.capacity) {
    return;
  }
  const auto available = snapshot.capacity - snapshot.queued - snapshot.active;
  const auto count = std::min(available, maximum_cycle_submissions);
  for (std::size_t index = 0; index < count; ++index) {
    const auto result = workers_.try_submit(
        [this](const std::stop_token stop_token) { process_one(stop_token); });
    if (result != SubmitResult::accepted) {
      break;
    }
  }
}

QueueSnapshot SchedulerService::queue_snapshot() const {
  return workers_.snapshot();
}

void SchedulerService::poll(const std::stop_token stop_token) noexcept {
  while (!stop_token.stop_requested()) {
    try {
      run_one_cycle();
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "scheduler.poll",
                         error.what(), std::nullopt});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "scheduler.poll",
                         "Unknown scheduler polling failure.", std::nullopt});
    }
    std::unique_lock lock{poll_mutex_};
    poll_wakeup_.wait_for(lock, poll_interval);
  }
}

void SchedulerService::process_one(const std::stop_token stop_token) noexcept {
  if (stop_token.stop_requested()) {
    return;
  }
  std::optional<ClaimedScheduledJob> claimed;
  try {
    const auto current = now_ms(clock_);
    claimed = repository_.claim_due_job(current, current + lease_duration_ms,
                                        instance_id_, ids_.next_id());
    if (!claimed.has_value()) {
      return;
    }
    if (stop_token.stop_requested()) {
      static_cast<void>(repository_.release_job(*claimed, current));
      return;
    }
    if (!handlers_.dispatch(*claimed)) {
      static_cast<void>(repository_.fail_job(*claimed, current, current,
                                             "handler_unknown_type", false));
      diagnostics_.emit({DiagnosticSeverity::error, "scheduler.unknown_type",
                         "A scheduled job was dead-lettered because its "
                         "versioned handler type is unknown.",
                         claimed->correlation_id});
    }
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "scheduler.handler",
                       error.what(),
                       claimed.has_value()
                           ? std::optional<std::string>{claimed->correlation_id}
                           : std::nullopt});
    if (claimed.has_value()) {
      try {
        const auto current = now_ms(clock_);
        static_cast<void>(repository_.fail_job(
            *claimed, current, current + retry_delay_ms(claimed->attempt_count),
            "handler_exception", true));
      } catch (...) {
      }
    }
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error, "scheduler.handler",
                       "Unknown scheduled-job failure.",
                       claimed.has_value()
                           ? std::optional<std::string>{claimed->correlation_id}
                           : std::nullopt});
    if (claimed.has_value()) {
      try {
        const auto current = now_ms(clock_);
        static_cast<void>(repository_.fail_job(
            *claimed, current, current + retry_delay_ms(claimed->attempt_count),
            "handler_unknown_exception", true));
      } catch (...) {
      }
    }
  }
}

void SchedulerService::handle_test_notice(const ClaimedScheduledJob &job) {
  const auto *payload = std::get_if<NoticeOutboxPayload>(&job.payload);
  const auto current = now_ms(clock_);
  if (payload == nullptr) {
    static_cast<void>(
        repository_.fail_job(job, current, current, "payload_invalid", false));
    return;
  }
  const auto event_id = ids_.next_id();
  const auto outbox_id = ids_.next_id();
  const EventJournalEntry event{
      .event_id = event_id,
      .event_type = "owner_test.notice_job_fired.v1",
      .aggregate_type = "owner_test",
      .aggregate_id = job.job_id,
      .actor_user_id = payload->notice.target_user_id,
      .guild_id = payload->notice.guild_id,
      .channel_id = payload->notice.channel_id,
      .source_message_id = std::nullopt,
      .occurred_at_ms = current,
      .recorded_at_ms = current,
      .correlation_id = job.correlation_id,
      .causation_id = job.causation_event_id,
      .idempotency_key = "event:job-fired:" + job.job_id,
      .payload_json = "{}",
  };
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{pending_notice_outbox_kind},
      .aggregate_type = "owner_test",
      .aggregate_id = job.job_id,
      .target_guild_id = payload->notice.guild_id,
      .target_channel_id = payload->notice.channel_id,
      .target_user_id = payload->notice.target_user_id,
      .available_at_ms = current,
      .max_attempts = 5,
      .idempotency_key = "outbox:job-notice:" + job.job_id,
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = current,
  };
  const auto status =
      repository_.complete_notice_job(job, event, outbox, current);
  if (status == WorkMutationStatus::applied ||
      status == WorkMutationStatus::unchanged) {
    outbox_wakeup_();
  }
}

} // namespace sanguinius
