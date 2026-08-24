#include "sanguinius/outbox.hpp"

#include "sanguinius/pending_notice.hpp"
#include "sanguinius/wagers.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr auto poll_interval = std::chrono::seconds{1};
constexpr std::int64_t lease_duration_ms = 60'000;
constexpr std::int64_t nonce_retry_window_ms = 90'000;
constexpr std::int64_t submission_lease_margin_ms = 10'000;
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

[[nodiscard]] std::int64_t
submission_lease_until(const std::int64_t current,
                       const std::chrono::milliseconds receipt_wait_timeout) {
  const auto extension = static_cast<std::int64_t>(
      receipt_wait_timeout.count() + submission_lease_margin_ms);
  if (current > std::numeric_limits<std::int64_t>::max() - extension) {
    throw std::overflow_error{"Public submission lease timestamp overflow."};
  }
  return current + extension;
}

[[nodiscard]] DeliveryAttemptStamp attempt_stamp(const Clock &clock) {
  return DeliveryAttemptStamp{
      .wall_time_ms = now_ms(clock),
      .elapsed_realtime_ms = clock.elapsed_realtime_ms(),
      .boot_session_id = std::string{clock.boot_session_id()},
  };
}

[[nodiscard]] bool
within_nonce_retry_window(const ClaimedOutboxMessage &outbox,
                          const DeliveryAttemptStamp &current) {
  if (!outbox.first_attempt_elapsed_ms.has_value() ||
      !outbox.first_attempt_boot_id.has_value() ||
      *outbox.first_attempt_boot_id != current.boot_session_id ||
      current.elapsed_realtime_ms < *outbox.first_attempt_elapsed_ms) {
    return false;
  }
  return current.elapsed_realtime_ms - *outbox.first_attempt_elapsed_ms <=
         nonce_retry_window_ms;
}

struct AwaitedReceipt {
  std::mutex mutex;
  std::condition_variable_any changed;
  std::optional<PublicDeliveryReceipt> receipt;
};

} // namespace

void OutboxHandlerRegistry::add(std::string kind, Handler handler) {
  if (frozen_) {
    throw std::logic_error{"Outbox handler registry is frozen."};
  }
  if (kind.empty() || !handler ||
      !handlers_.emplace(std::move(kind), std::move(handler)).second) {
    throw std::invalid_argument{
        "Outbox handler registration is invalid or duplicate."};
  }
}

void OutboxHandlerRegistry::freeze() { frozen_ = true; }

bool OutboxHandlerRegistry::dispatch(const ClaimedOutboxMessage &outbox,
                                     const std::stop_token stop_token) const {
  if (!frozen_) {
    throw std::logic_error{"Outbox handler registry is not frozen."};
  }
  const auto found = handlers_.find(outbox.kind);
  if (found == handlers_.end()) {
    return false;
  }
  found->second(outbox, stop_token);
  return true;
}

OutboxService::OutboxService(
    DurableWorkRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, Diagnostics &diagnostics,
    DiscordPublicDelivery &delivery,
    const DiscordStatusProvider &discord_status,
    const ServerScopeConfiguration scope, std::string instance_id,
    const std::size_t queue_capacity,
    const std::chrono::milliseconds receipt_wait_timeout)
    : repository_{repository}, clock_{clock}, ids_{ids},
      diagnostics_{diagnostics}, delivery_{delivery},
      discord_status_{discord_status}, scope_{scope},
      instance_id_{std::move(instance_id)}, workers_{queue_capacity, 2},
      receipt_wait_timeout_{receipt_wait_timeout} {
  if (instance_id_.empty() || !scope_.guild_id.is_set() ||
      !scope_.primary_channel_id.is_set() ||
      receipt_wait_timeout_.count() <= 0 ||
      receipt_wait_timeout_ > std::chrono::seconds{90}) {
    throw std::invalid_argument{"Outbox dependencies are incomplete."};
  }
  handlers_.add(std::string{pending_notice_outbox_kind},
                [this](const ClaimedOutboxMessage &outbox,
                       const std::stop_token stop_token) {
                  handle_notice(outbox, stop_token);
                });
  handlers_.add(std::string{public_discord_outbox_kind},
                [this](const ClaimedOutboxMessage &outbox,
                       const std::stop_token stop_token) {
                  handle_public(outbox, stop_token);
                });
  handlers_.add(std::string{test_public_retry_outbox_kind},
                [this](const ClaimedOutboxMessage &outbox,
                       const std::stop_token stop_token) {
                  handle_public(outbox, stop_token);
                });
  handlers_.add(std::string{wager_public_edit_outbox_kind},
                [this](const ClaimedOutboxMessage &outbox,
                       const std::stop_token stop_token) {
                  handle_public_edit(outbox, stop_token);
                });
  handlers_.freeze();
}

OutboxService::~OutboxService() { stop(); }

void OutboxService::start() {
  if (started_) {
    throw std::logic_error{"Outbox service may only be started once."};
  }
  workers_.start();
  started_ = true;
  poller_ = std::jthread{
      [this](const std::stop_token stop_token) { poll(stop_token); }};
}

void OutboxService::stop() noexcept {
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

void OutboxService::wake() noexcept { poll_wakeup_.notify_all(); }

void OutboxService::run_one_cycle() {
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

QueueSnapshot OutboxService::queue_snapshot() const {
  return workers_.snapshot();
}

void OutboxService::poll(const std::stop_token stop_token) noexcept {
  while (!stop_token.stop_requested()) {
    try {
      run_one_cycle();
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "outbox.poll", error.what(),
                         std::nullopt});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "outbox.poll",
                         "Unknown outbox polling failure.", std::nullopt});
    }
    std::unique_lock lock{poll_mutex_};
    poll_wakeup_.wait_for(lock, poll_interval);
  }
}

void OutboxService::process_one(const std::stop_token stop_token) noexcept {
  if (stop_token.stop_requested()) {
    return;
  }
  std::optional<ClaimedOutboxMessage> claimed;
  try {
    const auto current = now_ms(clock_);
    const bool ready = discord_status_.status().ready;
    claimed = repository_.claim_due_outbox(current, current + lease_duration_ms,
                                           instance_id_, ids_.next_id(), ready);
    if (!claimed.has_value()) {
      return;
    }
    if (stop_token.stop_requested()) {
      static_cast<void>(repository_.release_outbox(*claimed, current));
      return;
    }
    if (!handlers_.dispatch(*claimed, stop_token)) {
      static_cast<void>(repository_.fail_outbox(*claimed, current, current,
                                                "handler_unknown_type",
                                                OutboxFailureMode::dead));
      diagnostics_.emit({DiagnosticSeverity::error, "outbox.unknown_type",
                         "An outbox row was dead-lettered because its "
                         "versioned handler type is unknown.",
                         shortened_persistent_id(claimed->outbox_id)});
    }
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "outbox.handler",
                       error.what(),
                       claimed.has_value()
                           ? std::optional<std::string>{shortened_persistent_id(
                                 claimed->outbox_id)}
                           : std::nullopt});
    if (claimed.has_value()) {
      try {
        const auto current = now_ms(clock_);
        static_cast<void>(repository_.fail_outbox(
            *claimed, current, current + retry_delay_ms(claimed->attempt_count),
            "handler_exception", OutboxFailureMode::retryable));
      } catch (...) {
      }
    }
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error, "outbox.handler",
                       "Unknown outbox handling failure.",
                       claimed.has_value()
                           ? std::optional<std::string>{shortened_persistent_id(
                                 claimed->outbox_id)}
                           : std::nullopt});
    if (claimed.has_value()) {
      try {
        const auto current = now_ms(clock_);
        static_cast<void>(repository_.fail_outbox(
            *claimed, current, current + retry_delay_ms(claimed->attempt_count),
            "handler_unknown_exception", OutboxFailureMode::retryable));
      } catch (...) {
      }
    }
  }
}

void OutboxService::handle_notice(const ClaimedOutboxMessage &outbox,
                                  const std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    static_cast<void>(repository_.release_outbox(outbox, now_ms(clock_)));
    return;
  }
  const auto *payload = std::get_if<NoticeOutboxPayload>(&outbox.payload);
  const auto current = now_ms(clock_);
  if (payload == nullptr) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current, "payload_invalid", OutboxFailureMode::dead));
    return;
  }
  const auto event_id = ids_.next_id();
  const EventJournalEntry event{
      .event_id = event_id,
      .event_type = "notice.queued.v1",
      .aggregate_type = "pending_notice",
      .aggregate_id = payload->notice.notice_id,
      .actor_user_id = payload->notice.target_user_id,
      .guild_id = payload->notice.guild_id,
      .channel_id = payload->notice.channel_id,
      .source_message_id = std::nullopt,
      .occurred_at_ms = current,
      .recorded_at_ms = current,
      .correlation_id = outbox.correlation_id,
      .causation_id = outbox.causation_event_id,
      .idempotency_key = "event:notice-queued:" + payload->notice.notice_id,
      .payload_json = "{}",
  };

  std::optional<OutboxEnqueue> public_outbox;
  std::optional<PublicOutboxPayload> public_payload;
  if (payload->announce_publicly) {
    const auto public_id = ids_.next_id();
    public_outbox = OutboxEnqueue{
        .outbox_id = public_id,
        .kind = std::string{public_discord_outbox_kind},
        .aggregate_type = "pending_notice",
        .aggregate_id = payload->notice.notice_id,
        .target_guild_id = payload->notice.guild_id,
        .target_channel_id = payload->notice.channel_id,
        .target_user_id = std::nullopt,
        .available_at_ms = current,
        .max_attempts = 5,
        .idempotency_key = "outbox:notice-card:" + payload->notice.notice_id,
        .provider_nonce = discord_nonce_from_uuid(public_id),
        .created_at_ms = current,
    };
    public_payload = PublicOutboxPayload{
        .request = make_neutral_notice_card(payload->notice),
        .fail_before_first_send = false,
    };
  }
  const auto status = repository_.complete_notice_outbox(
      outbox, event, std::move(public_outbox), std::move(public_payload),
      current);
  if (status == WorkMutationStatus::applied ||
      status == WorkMutationStatus::unchanged) {
    wake();
  }
}

void OutboxService::handle_public(const ClaimedOutboxMessage &outbox,
                                  const std::stop_token stop_token) {
  const auto *payload = std::get_if<PublicOutboxPayload>(&outbox.payload);
  const auto current = now_ms(clock_);
  if (payload == nullptr) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current, "payload_invalid", OutboxFailureMode::dead));
    return;
  }
  if (payload->request.guild_id != scope_.guild_id ||
      payload->request.channel_id != scope_.primary_channel_id) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current, "scope_rejected", OutboxFailureMode::dead));
    return;
  }
  if (outbox.kind == test_public_retry_outbox_kind &&
      payload->fail_before_first_send && outbox.attempt_count == 1) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current + retry_delay_ms(outbox.attempt_count),
        "test_injected_transient", OutboxFailureMode::retryable));
    wake();
    return;
  }
  if (!discord_status_.status().ready) {
    static_cast<void>(repository_.release_outbox(outbox, current));
    wake();
    return;
  }
  const auto current_attempt = attempt_stamp(clock_);
  if (outbox.first_attempt_at_ms.has_value() && outbox.attempt_count > 1 &&
      !within_nonce_retry_window(outbox, current_attempt)) {
    const bool ambiguous =
        outbox.submission_started_at_ms.has_value() ||
        outbox.last_error_code ==
            std::optional<std::string>{"discord_unknown_outcome"};
    static_cast<void>(
        repository_.fail_outbox(outbox, current, current,
                                ambiguous ? "discord_unknown_outcome_stale"
                                          : "discord_retry_window_expired",
                                OutboxFailureMode::failed));
    return;
  }
  if (stop_token.stop_requested()) {
    static_cast<void>(repository_.release_outbox(outbox, current));
    return;
  }

  auto submitted_outbox = outbox;
  const auto receipt_lease_until = submission_lease_until(
      current_attempt.wall_time_ms, receipt_wait_timeout_);
  const auto marked = repository_.mark_public_outbox_submitted(
      outbox, current_attempt, receipt_lease_until);
  if (marked != WorkMutationStatus::applied) {
    return;
  }
  submitted_outbox.first_attempt_at_ms =
      outbox.first_attempt_at_ms.value_or(current_attempt.wall_time_ms);
  submitted_outbox.first_attempt_elapsed_ms =
      outbox.first_attempt_elapsed_ms.value_or(
          current_attempt.elapsed_realtime_ms);
  submitted_outbox.first_attempt_boot_id =
      outbox.first_attempt_boot_id.value_or(current_attempt.boot_session_id);
  submitted_outbox.submission_started_at_ms = current_attempt.wall_time_ms;

  const auto awaited = std::make_shared<AwaitedReceipt>();
  try {
    delivery_.send_public(payload->request, outbox.provider_nonce,
                          [awaited](PublicDeliveryReceipt receipt) mutable {
                            const std::scoped_lock lock{awaited->mutex};
                            if (!awaited->receipt.has_value()) {
                              awaited->receipt = std::move(receipt);
                              awaited->changed.notify_all();
                            }
                          });
  } catch (...) {
    handle_receipt(std::move(submitted_outbox),
                   {DeliveryResult::unknown_outcome, std::nullopt});
    return;
  }

  std::optional<PublicDeliveryReceipt> receipt;
  {
    std::unique_lock lock{awaited->mutex};
    const bool received = awaited->changed.wait_for(
        lock, stop_token, receipt_wait_timeout_,
        [&awaited] { return awaited->receipt.has_value(); });
    if (received) {
      receipt = std::move(awaited->receipt);
    }
  }
  if (receipt.has_value()) {
    handle_receipt(std::move(submitted_outbox), std::move(*receipt));
    return;
  }
  if (!stop_token.stop_requested()) {
    handle_receipt(std::move(submitted_outbox),
                   {DeliveryResult::unknown_outcome, std::nullopt});
  }
}

void OutboxService::handle_public_edit(const ClaimedOutboxMessage &outbox,
                                       const std::stop_token stop_token) {
  const auto *payload = std::get_if<PublicEditOutboxPayload>(&outbox.payload);
  const auto current = now_ms(clock_);
  if (payload == nullptr) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current, "payload_invalid", OutboxFailureMode::dead));
    return;
  }
  if (payload->replacement.guild_id != scope_.guild_id ||
      payload->replacement.channel_id != scope_.primary_channel_id) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current, "scope_rejected", OutboxFailureMode::dead));
    return;
  }
  if (!discord_status_.status().ready) {
    static_cast<void>(repository_.release_outbox(outbox, current));
    return;
  }
  const auto provider_message =
      repository_.delivered_provider_message_id(payload->source_outbox_id);
  if (!provider_message) {
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current + retry_delay_ms(outbox.attempt_count),
        "edit_target_pending", OutboxFailureMode::retryable));
    wake();
    return;
  }
  if (stop_token.stop_requested()) {
    static_cast<void>(repository_.release_outbox(outbox, current));
    return;
  }
  const auto stamp = attempt_stamp(clock_);
  if (repository_.mark_public_outbox_submitted(
          outbox, stamp,
          submission_lease_until(stamp.wall_time_ms, receipt_wait_timeout_)) !=
      WorkMutationStatus::applied)
    return;

  const auto awaited = std::make_shared<AwaitedReceipt>();
  try {
    delivery_.edit_public(
        PublicMessageEditRequest{
            .guild_id = payload->replacement.guild_id,
            .channel_id = payload->replacement.channel_id,
            .message_id = *provider_message,
            .message = payload->replacement.message,
        },
        [awaited, provider_message](const DeliveryResult result) {
          const std::scoped_lock lock{awaited->mutex};
          if (!awaited->receipt)
            awaited->receipt = PublicDeliveryReceipt{result, provider_message};
          awaited->changed.notify_all();
        });
  } catch (...) {
    const auto retry_at = current + retry_delay_ms(outbox.attempt_count);
    static_cast<void>(repository_.fail_outbox(
        outbox, current, retry_at, "discord_edit_unknown",
        OutboxFailureMode::retryable));
    wake();
    return;
  }
  std::optional<PublicDeliveryReceipt> receipt;
  {
    std::unique_lock lock{awaited->mutex};
    if (awaited->changed.wait_for(lock, stop_token, receipt_wait_timeout_,
                                  [&awaited] { return awaited->receipt.has_value(); }))
      receipt = awaited->receipt;
  }
  if (!receipt && stop_token.stop_requested())
    return;
  const auto result = receipt ? receipt->result : DeliveryResult::unknown_outcome;
  if (result == DeliveryResult::success) {
    static_cast<void>(repository_.complete_public_outbox(
        outbox, *provider_message, now_ms(clock_)));
    return;
  }
  const auto completion_time = now_ms(clock_);
  if (result == DeliveryResult::permanent_failure) {
    static_cast<void>(repository_.fail_outbox(
        outbox, completion_time, completion_time, "discord_edit_permanent",
        OutboxFailureMode::failed));
    return;
  }
  static_cast<void>(repository_.fail_outbox(
      outbox, completion_time,
      completion_time + retry_delay_ms(outbox.attempt_count),
      result == DeliveryResult::unknown_outcome ? "discord_edit_unknown"
                                                : "discord_edit_transient",
      OutboxFailureMode::retryable));
  wake();
}

void OutboxService::handle_receipt(ClaimedOutboxMessage outbox,
                                   PublicDeliveryReceipt receipt) noexcept {
  try {
    const auto current_attempt = attempt_stamp(clock_);
    const auto current = current_attempt.wall_time_ms;
    if (receipt.result == DeliveryResult::success &&
        receipt.provider_message_id.has_value()) {
      static_cast<void>(repository_.complete_public_outbox(
          outbox, *receipt.provider_message_id, current));
      return;
    }
    const auto outcome = receipt.result == DeliveryResult::success
                             ? DeliveryResult::unknown_outcome
                             : receipt.result;
    const bool within_window =
        outbox.first_attempt_at_ms.has_value() &&
        within_nonce_retry_window(outbox, current_attempt);
    if ((outcome == DeliveryResult::transient_failure ||
         outcome == DeliveryResult::unknown_outcome) &&
        within_window) {
      static_cast<void>(repository_.fail_outbox(
          outbox, current, current + retry_delay_ms(outbox.attempt_count),
          outcome == DeliveryResult::unknown_outcome ? "discord_unknown_outcome"
                                                     : "discord_transient",
          OutboxFailureMode::retryable));
      wake();
      return;
    }
    static_cast<void>(repository_.fail_outbox(
        outbox, current, current,
        outcome == DeliveryResult::permanent_failure ? "discord_permanent"
        : outcome == DeliveryResult::unknown_outcome
            ? "discord_unknown_outcome_stale"
            : "discord_retry_window_expired",
        OutboxFailureMode::failed));
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "outbox.completion",
                       error.what(),
                       shortened_persistent_id(outbox.outbox_id)});
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error, "outbox.completion",
                       "Unknown durable delivery completion failure.",
                       shortened_persistent_id(outbox.outbox_id)});
  }
}

} // namespace sanguinius
