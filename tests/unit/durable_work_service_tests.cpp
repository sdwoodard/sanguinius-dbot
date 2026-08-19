#include "sanguinius/durable_work_controls.hpp"
#include "sanguinius/outbox.hpp"
#include "sanguinius/scheduler.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_durable_work_repository.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class ReadinessDropsAfterClaim final
    : public sanguinius::DiscordStatusProvider {
public:
  [[nodiscard]] sanguinius::DiscordRuntimeStatus
  status() const noexcept override {
    const auto call = calls_.fetch_add(1, std::memory_order_relaxed);
    return {.ready = call == 0};
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_.load(std::memory_order_relaxed);
  }

private:
  mutable std::atomic<std::size_t> calls_{};
};

class CountingPublicDelivery final : public sanguinius::DiscordPublicDelivery {
public:
  void send_public(const sanguinius::PublicMessageRequest &, std::string_view,
                   sanguinius::PublicDeliveryCallback callback) override {
    sends_.fetch_add(1, std::memory_order_relaxed);
    if (callback) {
      callback(
          {sanguinius::DeliveryResult::success, sanguinius::DiscordId{1'000}});
    }
  }

  [[nodiscard]] std::size_t sends() const noexcept {
    return sends_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::size_t> sends_{};
};

class AlwaysReady final : public sanguinius::DiscordStatusProvider {
public:
  [[nodiscard]] sanguinius::DiscordRuntimeStatus
  status() const noexcept override {
    return {.ready = true};
  }
};

class HoldingPublicDelivery final : public sanguinius::DiscordPublicDelivery {
public:
  void send_public(const sanguinius::PublicMessageRequest &, std::string_view,
                   sanguinius::PublicDeliveryCallback callback) override {
    const std::scoped_lock lock{mutex_};
    ++sends_;
    callback_ = std::move(callback);
    changed_.notify_all();
  }

  [[nodiscard]] bool
  wait_for_send(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return sends_ != 0; });
  }

  [[nodiscard]] std::size_t sends() const {
    const std::scoped_lock lock{mutex_};
    return sends_;
  }

  void release() {
    sanguinius::PublicDeliveryCallback callback;
    {
      const std::scoped_lock lock{mutex_};
      callback = std::move(callback_);
    }
    if (callback) {
      callback(
          {sanguinius::DeliveryResult::success, sanguinius::DiscordId{1'001}});
    }
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  sanguinius::PublicDeliveryCallback callback_;
  std::size_t sends_{};
};

template <typename Predicate>
[[nodiscard]] bool eventually(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

} // namespace

TEST_CASE("durable handler registries are exact immutable and duplicate safe",
          "[durable][registry]") {
  std::size_t job_calls{};
  sanguinius::JobHandlerRegistry jobs;
  jobs.add(
      "owner_test.notice.v1",
      [&job_calls](const sanguinius::ClaimedScheduledJob &) { ++job_calls; });
  REQUIRE_THROWS(jobs.add("owner_test.notice.v1",
                          [](const sanguinius::ClaimedScheduledJob &) {}));
  jobs.freeze();

  sanguinius::ClaimedScheduledJob known_job;
  known_job.job_type = "owner_test.notice.v1";
  REQUIRE(jobs.dispatch(known_job));
  REQUIRE(job_calls == 1);
  known_job.job_type = "owner_test.notice.v2";
  REQUIRE_FALSE(jobs.dispatch(known_job));
  REQUIRE_THROWS(
      jobs.add("another.v1", [](const sanguinius::ClaimedScheduledJob &) {}));

  std::size_t outbox_calls{};
  sanguinius::OutboxHandlerRegistry outbox;
  outbox.add("discord.public.v1",
             [&outbox_calls](const sanguinius::ClaimedOutboxMessage &,
                             std::stop_token) { ++outbox_calls; });
  REQUIRE_THROWS(outbox.add(
      "discord.public.v1",
      [](const sanguinius::ClaimedOutboxMessage &, std::stop_token) {}));
  outbox.freeze();
  sanguinius::ClaimedOutboxMessage known_outbox;
  known_outbox.kind = "discord.public.v1";
  REQUIRE(outbox.dispatch(known_outbox, {}));
  REQUIRE(outbox_calls == 1);
  known_outbox.kind = "discord.public.v2";
  REQUIRE_FALSE(outbox.dispatch(known_outbox, {}));
}

TEST_CASE("durable inspection rendering never includes payload fields",
          "[durable][inspection][privacy]") {
  REQUIRE(sanguinius::shortened_persistent_id(
              "00000000-0000-4000-8000-000000000402") == "00000000...0402");
  const std::vector<sanguinius::WorkInspectionEntry> entries{
      {.category = "outbox",
       .type = "discord.public.v1",
       .state = "failed",
       .shortened_id = "00000000...0402",
       .attempts = 2,
       .at_ms = 123,
       .error_code = "discord_unknown_outcome_stale"}};
  const auto rendered =
      sanguinius::render_work_inspection(entries, "Failed durable work");
  REQUIRE(rendered.find("discord.public.v1") != std::string::npos);
  REQUIRE(rendered.find("discord_unknown_outcome_stale") != std::string::npos);
  REQUIRE(rendered.find("payload") == std::string::npos);
  REQUIRE(rendered.find("private") == std::string::npos);

  std::vector<sanguinius::WorkInspectionEntry> bounded(10, entries.front());
  for (auto &entry : bounded) {
    entry.type = std::string(96, 'a');
    entry.error_code = std::string(96, 'b');
  }
  const auto maximum =
      sanguinius::render_work_inspection(bounded, "Recent durable work");
  REQUIRE(maximum.size() <= 1'900);
}

TEST_CASE("fake scheduler health includes expired claimed jobs in lag",
          "[durable][health][scheduler][lease]") {
  sanguinius::test::FakePendingNoticeRepository notices;
  sanguinius::test::FakeDurableWorkRepository repository{notices};
  const sanguinius::EventJournalEntry event{
      .event_id = "00000000-0000-4000-8000-000000000101",
      .event_type = "owner.test.v1",
      .aggregate_type = "owner_test",
      .aggregate_id = "health-expired-job",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 100,
      .recorded_at_ms = 100,
      .correlation_id = "health-expired-job",
      .causation_id = std::nullopt,
      .idempotency_key = "event:health-expired-job",
      .payload_json = "{}",
  };
  const sanguinius::ScheduledJobEnqueue job{
      .job_id = "00000000-0000-4000-8000-000000000201",
      .job_type = std::string{sanguinius::owner_test_notice_job_type},
      .aggregate_type = "owner_test",
      .aggregate_id = "health-expired-job",
      .due_at_ms = 100,
      .max_attempts = 5,
      .idempotency_key = "job:health-expired-job",
      .created_at_ms = 100,
  };
  REQUIRE(repository.schedule_notice(event, job, {}));
  const auto claimed = repository.claim_due_job(
      100, 200, "instance", "00000000-0000-4000-8000-000000000501");
  REQUIRE(claimed.has_value());

  REQUIRE(repository.health(199).scheduler_lag_ms == 0);
  const auto expired = repository.health(250);
  REQUIRE(expired.claimed_jobs == 1);
  REQUIRE(expired.scheduler_lag_ms == 150);
}

TEST_CASE("readiness loss after claim releases the outbox attempt",
          "[durable][outbox][readiness][lease]") {
  sanguinius::test::FakePendingNoticeRepository notices;
  sanguinius::test::FakeDurableWorkRepository repository{notices};
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  ReadinessDropsAfterClaim readiness;
  CountingPublicDelivery delivery;

  const sanguinius::EventJournalEntry event{
      .event_id = "00000000-0000-4000-8000-000000000101",
      .event_type = "owner.test.v1",
      .aggregate_type = "owner_test",
      .aggregate_id = "readiness-race",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 0,
      .recorded_at_ms = 0,
      .correlation_id = "readiness-race",
      .causation_id = std::nullopt,
      .idempotency_key = "event:readiness-race",
      .payload_json = "{}",
  };
  const std::string outbox_id{"00000000-0000-4000-8000-000000000401"};
  REQUIRE(repository.enqueue_public(
      event,
      {.outbox_id = outbox_id,
       .kind = std::string{sanguinius::public_discord_outbox_kind},
       .aggregate_type = "owner_test",
       .aggregate_id = "readiness-race",
       .target_guild_id = 10,
       .target_channel_id = 20,
       .target_user_id = std::nullopt,
       .available_at_ms = 0,
       .max_attempts = 5,
       .idempotency_key = "outbox:readiness-race",
       .provider_nonce = sanguinius::discord_nonce_from_uuid(outbox_id),
       .created_at_ms = 0},
      {.request = {.guild_id = 10,
                   .channel_id = 20,
                   .message = sanguinius::text_message("neutral")},
       .fail_before_first_send = false}));

  sanguinius::OutboxService service{repository,
                                    clock,
                                    ids,
                                    diagnostics,
                                    delivery,
                                    readiness,
                                    {10, 20, 30},
                                    "00000000-0000-4000-8000-000000000501",
                                    4,
                                    100ms};
  service.start();
  REQUIRE(eventually([&] {
    const auto health = repository.health(0);
    return readiness.calls() >= 2 && health.pending_outbox == 1 &&
           health.claimed_outbox == 0;
  }));
  service.stop();

  const auto health = repository.health(0);
  REQUIRE(health.pending_outbox == 1);
  REQUIRE(health.claimed_outbox == 0);
  REQUIRE(health.outbox_retries == 0);
  REQUIRE(delivery.sends() == 0);
}

TEST_CASE("active public submission outlives the initial claim lease",
          "[durable][outbox][lease][receipt]") {
  sanguinius::test::FakePendingNoticeRepository notices;
  sanguinius::test::FakeDurableWorkRepository repository{notices};
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{100}}};
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  AlwaysReady readiness;
  HoldingPublicDelivery delivery;

  const sanguinius::EventJournalEntry event{
      .event_id = "00000000-0000-4000-8000-000000000102",
      .event_type = "owner.test.v1",
      .aggregate_type = "owner_test",
      .aggregate_id = "receipt-lease",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 100'000,
      .recorded_at_ms = 100'000,
      .correlation_id = "receipt-lease",
      .causation_id = std::nullopt,
      .idempotency_key = "event:receipt-lease",
      .payload_json = "{}",
  };
  const std::string outbox_id{"00000000-0000-4000-8000-000000000402"};
  REQUIRE(repository.enqueue_public(
      event,
      {.outbox_id = outbox_id,
       .kind = std::string{sanguinius::public_discord_outbox_kind},
       .aggregate_type = "owner_test",
       .aggregate_id = "receipt-lease",
       .target_guild_id = 10,
       .target_channel_id = 20,
       .target_user_id = std::nullopt,
       .available_at_ms = 100'000,
       .max_attempts = 5,
       .idempotency_key = "outbox:receipt-lease",
       .provider_nonce = sanguinius::discord_nonce_from_uuid(outbox_id),
       .created_at_ms = 100'000},
      {.request = {.guild_id = 10,
                   .channel_id = 20,
                   .message = sanguinius::text_message("neutral")},
       .fail_before_first_send = false}));

  sanguinius::OutboxService service{repository,
                                    clock,
                                    ids,
                                    diagnostics,
                                    delivery,
                                    readiness,
                                    {10, 20, 30},
                                    "00000000-0000-4000-8000-000000000502",
                                    4,
                                    90s};
  service.start();
  REQUIRE(delivery.wait_for_send(2s));

  clock.set(std::chrono::sys_seconds{std::chrono::seconds{160}});
  REQUIRE_FALSE(repository
                    .claim_due_outbox(160'000, 220'000,
                                      "00000000-0000-4000-8000-000000000503",
                                      "00000000-0000-4000-8000-000000000504",
                                      true)
                    .has_value());
  REQUIRE(repository.health(160'000).outbox_retries == 0);

  delivery.release();
  REQUIRE(repository.wait_for_outbox_idle(2s));
  REQUIRE(delivery.sends() == 1);
  service.stop();
}
