#include "sanguinius/outbox.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/scheduler.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using sanguinius::ClaimedOutboxMessage;
using sanguinius::ClaimedScheduledJob;
using sanguinius::DeliveryAttemptStamp;
using sanguinius::EventJournalEntry;
using sanguinius::NoticeOutboxPayload;
using sanguinius::OutboxEnqueue;
using sanguinius::OutboxFailureMode;
using sanguinius::ScheduledJobEnqueue;
using sanguinius::WorkMutationStatus;
using sanguinius::persistence::Database;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SqliteCoreIdentityRepository;
using sanguinius::persistence::SqliteDurableWorkRepository;
using sanguinius::persistence::SqlitePendingNoticeRepository;
using sanguinius::persistence::SqliteRepositoryContext;

constexpr std::string_view event_id_1{"00000000-0000-4000-8000-000000000101"};
constexpr std::string_view event_id_2{"00000000-0000-4000-8000-000000000102"};
constexpr std::string_view event_id_3{"00000000-0000-4000-8000-000000000103"};
constexpr std::string_view job_id{"00000000-0000-4000-8000-000000000201"};
constexpr std::string_view notice_id{"00000000-0000-4000-8000-000000000301"};
constexpr std::string_view token_id{"00000000-0000-4000-8000-000000000302"};
constexpr std::string_view outbox_id_1{"00000000-0000-4000-8000-000000000401"};
constexpr std::string_view outbox_id_2{"00000000-0000-4000-8000-000000000402"};
constexpr std::string_view outbox_id_3{"00000000-0000-4000-8000-000000000403"};
constexpr std::string_view lease_1{"00000000-0000-4000-8000-000000000501"};
constexpr std::string_view lease_2{"00000000-0000-4000-8000-000000000502"};
constexpr std::string_view boot_id{"00000000-0000-4000-8000-000000000601"};

[[nodiscard]] DeliveryAttemptStamp attempt(const std::int64_t at_ms) {
  return {.wall_time_ms = at_ms,
          .elapsed_realtime_ms = at_ms,
          .boot_session_id = std::string{boot_id}};
}

class DurableFixture {
public:
  DurableFixture() {
    {
      auto database = Database::open_migration(temporary.path(), 25ms);
      const Migrator migrator{sanguinius::persistence::production_migrations(),
                              {"test", "revision"},
                              clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 14);
    }
    context = std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path(), 25ms));
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
    repository = std::make_unique<SqliteDurableWorkRepository>(context);
    notices = std::make_unique<SqlitePendingNoticeRepository>(context);
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteDurableWorkRepository> repository;
  std::unique_ptr<SqlitePendingNoticeRepository> notices;
};

[[nodiscard]] EventJournalEntry
event(const std::string_view id, std::string key, const std::int64_t at = 100) {
  return EventJournalEntry{
      .event_id = std::string{id},
      .event_type = "owner.test.v1",
      .aggregate_type = "owner_interaction",
      .aggregate_id = "900",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = at,
      .recorded_at_ms = at,
      .correlation_id = "900",
      .causation_id = std::nullopt,
      .idempotency_key = std::move(key),
      .payload_json = "{}",
  };
}

[[nodiscard]] NoticeOutboxPayload
notice_payload(const std::int64_t created_at = 100) {
  return NoticeOutboxPayload{
      .notice =
          sanguinius::CreatePendingNoticeRequest{
              .notice_id = std::string{notice_id},
              .token_id = std::string{token_id},
              .target_user_id = 30,
              .guild_id = 10,
              .channel_id = 20,
              .notice_type = "owner_test.notice.v1",
              .content = {"Private title", "Private body sentinel"},
              .source_aggregate_type = "owner_interaction",
              .source_aggregate_id = "900",
              .expires_at_ms = created_at + 86'400'000,
              .notice_idempotency_key = "notice:900",
              .token_idempotency_key = "token:900",
              .created_at_ms = created_at,
          },
      .announce_publicly = true,
  };
}

[[nodiscard]] ScheduledJobEnqueue job(const std::int64_t due_at = 200) {
  return ScheduledJobEnqueue{
      .job_id = std::string{job_id},
      .job_type = std::string{sanguinius::owner_test_notice_job_type},
      .aggregate_type = "owner_interaction",
      .aggregate_id = "900",
      .due_at_ms = due_at,
      .max_attempts = 5,
      .idempotency_key = "job:900",
      .created_at_ms = 100,
  };
}

[[nodiscard]] OutboxEnqueue notice_outbox(const std::string_view id,
                                          std::string key,
                                          const std::int64_t available = 100) {
  return OutboxEnqueue{
      .outbox_id = std::string{id},
      .kind = std::string{sanguinius::pending_notice_outbox_kind},
      .aggregate_type = "owner_interaction",
      .aggregate_id = "900",
      .target_guild_id = 10,
      .target_channel_id = 20,
      .target_user_id = 30,
      .available_at_ms = available,
      .max_attempts = 5,
      .idempotency_key = std::move(key),
      .provider_nonce = sanguinius::discord_nonce_from_uuid(id),
      .created_at_ms = 100,
  };
}

[[nodiscard]] OutboxEnqueue public_outbox(const std::string_view id,
                                          std::string key,
                                          const std::int64_t available = 200) {
  auto result = notice_outbox(id, std::move(key), available);
  result.kind = std::string{sanguinius::public_discord_outbox_kind};
  result.target_user_id = std::nullopt;
  return result;
}

[[nodiscard]] std::int64_t scalar(SqliteRepositoryContext &context,
                                  const std::string_view sql) {
  auto statement = context.connection().prepare(sql);
  REQUIRE(statement.step());
  return statement.column_int64(0);
}

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

TEST_CASE("durable button styles preserve old primary rows and live secondary rows",
          "[durable][outbox][buttons][compatibility]") {
  DurableFixture fixture;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = {.content = "Styled controls",
                              .embed = std::nullopt,
                              .buttons = {{.custom_id = "old-primary",
                                           .label = "Primary"},
                                          {.custom_id = "live-secondary",
                                           .label = "Secondary",
                                           .disabled = false,
                                           .style = sanguinius::ButtonStyle::secondary}},
                              .allowed_user_mentions = {}}},
      .fail_before_first_send = false};
  REQUIRE(fixture.repository->enqueue_public(
      event(event_id_1, "event:button-styles"),
      public_outbox(outbox_id_1, "outbox:button-styles", 100), payload));
  auto stored = fixture.context->connection().prepare(
      "SELECT payload_json FROM outbox_message WHERE outbox_id=?");
  stored.bind(1, outbox_id_1);
  REQUIRE(stored.step());
  const auto json = nlohmann::json::parse(stored.column_text(0));
  REQUIRE_FALSE(json.at("buttons")[0].contains("style"));
  REQUIRE(json.at("buttons")[1].at("style") == "secondary");
  const auto claimed = fixture.repository->claim_due_outbox(
      100, 200, "style-worker", std::string{lease_1}, true);
  REQUIRE(claimed);
  const auto *decoded =
      std::get_if<sanguinius::PublicOutboxPayload>(&claimed->payload);
  REQUIRE(decoded);
  REQUIRE(decoded->request.message.buttons[0].style ==
          sanguinius::ButtonStyle::primary);
  REQUIRE(decoded->request.message.buttons[1].style ==
          sanguinius::ButtonStyle::secondary);
}

class ImmediateDiscord final : public sanguinius::DiscordPublicDelivery,
                               public sanguinius::DiscordStatusProvider {
public:
  void send_public(const sanguinius::PublicMessageRequest &, std::string_view,
                   sanguinius::PublicDeliveryCallback callback) override {
    const auto next = sends_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (callback) {
      callback({sanguinius::DeliveryResult::success,
                sanguinius::DiscordId{1'000 + next}});
    }
  }

  [[nodiscard]] sanguinius::DiscordRuntimeStatus
  status() const noexcept override {
    return {.ready = true};
  }

  [[nodiscard]] std::size_t sends() const noexcept {
    return sends_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<std::uint64_t> sends_{};
};

class RestartDatabaseFixture {
public:
  RestartDatabaseFixture() {
    {
      auto database = Database::open_migration(temporary.path(), 25ms);
      const Migrator migrator{sanguinius::persistence::production_migrations(),
                              {"test", "revision"},
                              clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 14);
    }
    auto context = open();
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
  }

  [[nodiscard]] std::shared_ptr<SqliteRepositoryContext> open() const {
    return std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path(), 25ms));
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{1}}};
};

} // namespace

TEST_CASE("event journal is immutable replayable and conflict detecting",
          "[durable][event][persistence]") {
  DurableFixture fixture;
  const auto first = event(event_id_1, "event:900");
  REQUIRE(fixture.repository->append_event(first));
  REQUIRE_FALSE(fixture.repository->append_event(first));

  auto conflict = first;
  conflict.event_id = std::string{event_id_2};
  conflict.payload_json = R"({"changed":true})";
  REQUIRE_THROWS(fixture.repository->append_event(conflict));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE event_journal SET payload_json = '{}' WHERE idempotency_key = "
      "'event:900'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM event_journal WHERE idempotency_key = 'event:900'"));

  auto caused = event(event_id_2, "event:901", 200);
  caused.event_type = "owner.other.v1";
  caused.aggregate_id = "901";
  caused.causation_id = std::string{event_id_1};
  REQUIRE(fixture.repository->append_event(caused));
  const auto recent_events = fixture.repository->recent_events(10);
  REQUIRE(recent_events.size() == 2);
  REQUIRE(recent_events[0].event_id == event_id_2);
  REQUIRE(recent_events[0].causation_id ==
          std::optional<std::string>{event_id_1});
  const auto typed = fixture.repository->events_by_type("owner.test.v1", 10);
  REQUIRE(typed.size() == 1);
  REQUIRE(typed[0].event_id == event_id_1);
  const auto aggregate =
      fixture.repository->aggregate_history("owner_interaction", "900", 10);
  REQUIRE(aggregate.size() == 1);
  REQUIRE(aggregate[0].event_id == event_id_1);

  const auto recent = fixture.repository->recent(10);
  REQUIRE(recent.size() == 2);
  REQUIRE(recent[1].type == "owner.test.v1");
  REQUIRE(recent[0].shortened_id.find("Private") == std::string::npos);
}

TEST_CASE("Discord nonces are stable bounded and distinguish UUID suffixes",
          "[durable][outbox][nonce]") {
  const auto first = sanguinius::discord_nonce_from_uuid(outbox_id_1);
  const auto second = sanguinius::discord_nonce_from_uuid(outbox_id_2);
  REQUIRE(first.size() == 25);
  REQUIRE(second.size() == 25);
  REQUIRE(first != second);
  REQUIRE(first == sanguinius::discord_nonce_from_uuid(outbox_id_1));
}

TEST_CASE("job leases are ordered fenced reclaimed and completed atomically",
          "[durable][scheduler][lease][restart]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:schedule"), job(), notice_payload()));
  auto competing_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 25ms));
  SqliteDurableWorkRepository competitor{competing_context};
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_job(199, 1'000, "instance-a", std::string{lease_1})
          .has_value());

  const auto first = fixture.repository->claim_due_job(200, 1'000, "instance-a",
                                                       std::string{lease_1});
  REQUIRE(first.has_value());
  REQUIRE(first->attempt_count == 1);
  REQUIRE(first->correlation_id == "900");
  REQUIRE(first->causation_event_id == std::optional<std::string>{event_id_1});
  REQUIRE_FALSE(
      competitor.claim_due_job(999, 2'000, "instance-b", std::string{lease_2})
          .has_value());
  const auto reclaimed = competitor.claim_due_job(1'000, 2'000, "instance-b",
                                                  std::string{lease_2});
  REQUIRE(reclaimed.has_value());
  REQUIRE(reclaimed->attempt_count == 2);

  auto effect = event(event_id_2, "event:effect", 1'000);
  effect.correlation_id = reclaimed->correlation_id;
  effect.causation_id = reclaimed->causation_event_id;
  auto wrong_trace = effect;
  wrong_trace.correlation_id = "different-correlation";
  REQUIRE(competitor.complete_notice_job(
              *reclaimed, wrong_trace,
              notice_outbox(outbox_id_1, "outbox:effect", 1'000),
              1'000) == WorkMutationStatus::invalid_state);
  REQUIRE(fixture.repository->complete_notice_job(
              *first, effect,
              notice_outbox(outbox_id_1, "outbox:effect", 1'000),
              1'000) == WorkMutationStatus::stale_claim);
  REQUIRE(competitor.complete_notice_job(
              *reclaimed, effect,
              notice_outbox(outbox_id_1, "outbox:effect", 1'000),
              1'000) == WorkMutationStatus::applied);
  REQUIRE(competitor.complete_notice_job(
              *reclaimed, effect,
              notice_outbox(outbox_id_1, "outbox:effect", 1'000),
              1'000) == WorkMutationStatus::unchanged);
  REQUIRE(fixture.repository->health(1'000).pending_outbox == 1);
  const auto effect_outbox = fixture.repository->claim_due_outbox(
      1'000, 2'000, "instance-a", std::string{lease_1}, false);
  REQUIRE(effect_outbox.has_value());
  REQUIRE(effect_outbox->correlation_id == "900");
  REQUIRE(effect_outbox->causation_event_id ==
          std::optional<std::string>{event_id_2});
}

TEST_CASE("notice and public outbox transitions preserve privacy and receipts",
          "[durable][outbox][transaction][privacy]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_1, "event:notice"),
      notice_outbox(outbox_id_1, "outbox:notice"), notice_payload()));
  const auto local = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_1}, false);
  REQUIRE(local.has_value());
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(999, 2'000, "other", std::string{lease_2}, false)
          .has_value());
  const auto recovered = fixture.repository->claim_due_outbox(
      1'000, 2'000, "other", std::string{lease_2}, false);
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->correlation_id == "900");
  REQUIRE(recovered->causation_event_id ==
          std::optional<std::string>{event_id_1});

  const sanguinius::PublicOutboxPayload public_payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral public card")},
      .fail_before_first_send = false,
  };
  auto materialized = event(event_id_2, "event:materialized", 1'000);
  materialized.correlation_id = recovered->correlation_id;
  materialized.causation_id = recovered->causation_event_id;
  REQUIRE(fixture.repository->complete_notice_outbox(
              *local, materialized,
              public_outbox(outbox_id_2, "outbox:public", 1'000),
              public_payload, 1'000) == WorkMutationStatus::stale_claim);
  REQUIRE(fixture.repository->complete_notice_outbox(
              *recovered, materialized,
              public_outbox(outbox_id_2, "outbox:public", 1'000),
              public_payload, 1'000) == WorkMutationStatus::applied);
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);
  REQUIRE_FALSE(fixture.repository
                    ->claim_due_outbox(1'000, 2'000, "instance",
                                       std::string{lease_1}, false)
                    .has_value());

  const auto first_public = fixture.repository->claim_due_outbox(
      1'000, 2'000, "instance", std::string{lease_1}, true);
  REQUIRE(first_public.has_value());
  REQUIRE(first_public->provider_nonce.size() == 25);
  REQUIRE(first_public->correlation_id == "900");
  REQUIRE(first_public->causation_event_id ==
          std::optional<std::string>{event_id_2});
  REQUIRE_FALSE(first_public->first_attempt_at_ms.has_value());
  REQUIRE_FALSE(first_public->submission_started_at_ms.has_value());
  REQUIRE(fixture.repository->complete_public_outbox(
              *first_public, 777, 1'000) == WorkMutationStatus::stale_claim);
  REQUIRE(fixture.repository->mark_public_outbox_submitted(
              *first_public, attempt(1'000), 2'000) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.repository->fail_outbox(
              *first_public, 1'000, 1'005, "discord_unknown_outcome",
              OutboxFailureMode::retryable) == WorkMutationStatus::applied);
  REQUIRE_FALSE(fixture.repository
                    ->claim_due_outbox(1'004, 2'000, "instance",
                                       std::string{lease_2}, true)
                    .has_value());
  const auto retried = fixture.repository->claim_due_outbox(
      1'005, 2'000, "instance", std::string{lease_2}, true);
  REQUIRE(retried.has_value());
  REQUIRE(retried->provider_nonce == first_public->provider_nonce);
  REQUIRE(retried->first_attempt_at_ms == std::optional<std::int64_t>{1'000});
  REQUIRE(retried->first_attempt_elapsed_ms ==
          std::optional<std::int64_t>{1'000});
  REQUIRE(retried->first_attempt_boot_id ==
          std::optional<std::string>{boot_id});
  REQUIRE_FALSE(retried->submission_started_at_ms.has_value());
  REQUIRE(fixture.repository->mark_public_outbox_submitted(
              *retried, attempt(1'005), 2'000) == WorkMutationStatus::applied);
  REQUIRE(fixture.repository->complete_public_outbox(*retried, 777, 1'005) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.repository->health(1'005).outbox_retries == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT provider_message_id = '777' FROM outbox_message "
                 "WHERE outbox_id = '00000000-0000-4000-8000-000000000402'") ==
          1);

  auto payload = fixture.context->connection().prepare(
      "SELECT payload_json FROM outbox_message WHERE outbox_id = ?");
  payload.bind(1, std::string{outbox_id_2});
  REQUIRE(payload.step());
  REQUIRE(payload.column_text(0).find("Private body sentinel") ==
          std::string::npos);
}

TEST_CASE("compound durable insertion rolls back on the second write",
          "[durable][transaction][rollback]") {
  DurableFixture fixture;
  auto invalid = notice_outbox(outbox_id_1, "outbox:invalid");
  invalid.provider_nonce = "invalid";
  const auto audit = event(event_id_1, "event:rollback");
  REQUIRE_THROWS(
      fixture.repository->enqueue_notice(audit, invalid, notice_payload()));
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM event_journal") == 0);
  REQUIRE(fixture.repository->append_event(audit));
}

TEST_CASE("notice materialization rolls back every compound effect",
          "[durable][transaction][failure-injection]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_1, "event:materialize-source"),
      notice_outbox(outbox_id_1, "outbox:materialize-source"),
      notice_payload()));
  const auto claimed = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_1}, false);
  REQUIRE(claimed.has_value());

  auto invalid_public =
      public_outbox(outbox_id_2, "outbox:materialize-public", 100);
  invalid_public.provider_nonce = "invalid";
  const sanguinius::PublicOutboxPayload public_payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral card")},
      .fail_before_first_send = false,
  };
  auto effect = event(event_id_2, "event:materialize-effect", 100);
  effect.correlation_id = claimed->correlation_id;
  effect.causation_id = claimed->causation_event_id;
  REQUIRE_THROWS(fixture.repository->complete_notice_outbox(
      *claimed, effect, invalid_public, public_payload, 100));
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM interaction_token") ==
          0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM event_journal") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT state = 'claimed' FROM outbox_message WHERE "
                 "outbox_id = '00000000-0000-4000-8000-000000000401'") == 1);

  const auto valid_public =
      public_outbox(outbox_id_2, "outbox:materialize-public", 100);
  REQUIRE(fixture.repository->complete_notice_outbox(
              *claimed, effect, valid_public, public_payload, 100) ==
          WorkMutationStatus::applied);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM interaction_token") ==
          1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM event_journal") == 2);
}

TEST_CASE("notice materialization rejects conflicting token routing metadata",
          "[durable][idempotency][conflict][token]") {
  DurableFixture fixture;
  fixture.context->connection().execute(
      "INSERT INTO interaction_token "
      "(token_id, token_version, interaction_kind, action, entity_type, "
      "entity_id, expected_user_id, guild_id, channel_id, state, "
      "expires_at_ms, idempotency_key, created_at_ms) VALUES "
      "('00000000-0000-4000-8000-000000000302', 1, 'select', "
      "'notice.dismiss', 'future_notice', "
      "'00000000-0000-4000-8000-000000000301', '30', '10', '20', "
      "'active', 86400100, 'token:900', 100)");

  auto payload = notice_payload();
  payload.announce_publicly = false;
  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_1, "event:token-conflict"),
      notice_outbox(outbox_id_1, "outbox:token-conflict"), payload));
  const auto claimed = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_1}, false);
  REQUIRE(claimed.has_value());

  auto effect = event(event_id_2, "event:token-conflict-effect", 100);
  effect.correlation_id = claimed->correlation_id;
  effect.causation_id = claimed->causation_event_id;
  REQUIRE_THROWS(fixture.repository->complete_notice_outbox(
      *claimed, effect, std::nullopt, std::nullopt, 100));
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM event_journal") == 1);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT interaction_kind = 'select' "
             "AND action = 'notice.dismiss' "
             "AND entity_type = 'future_notice' "
             "FROM interaction_token WHERE idempotency_key = 'token:900'") ==
      1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT state = 'claimed' FROM outbox_message WHERE "
                 "idempotency_key = 'outbox:token-conflict'") == 1);
}

TEST_CASE("compound replay verifies immutable business content",
          "[durable][idempotency][conflict]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_1, "event:compound-replay"),
      notice_outbox(outbox_id_1, "outbox:compound-replay"), notice_payload()));

  auto replay_event = event(event_id_2, "event:compound-replay", 200);
  auto replay_outbox =
      notice_outbox(outbox_id_2, "outbox:compound-replay", 200);
  replay_outbox.created_at_ms = 200;
  auto replay_payload = notice_payload(200);
  replay_payload.notice.notice_id = std::string{event_id_2};
  replay_payload.notice.token_id = std::string{lease_2};
  REQUIRE_FALSE(fixture.repository->enqueue_notice(replay_event, replay_outbox,
                                                   replay_payload));

  replay_payload.notice.content.body = "Different private body";
  REQUIRE_THROWS(fixture.repository->enqueue_notice(replay_event, replay_outbox,
                                                    replay_payload));

  DurableFixture job_fixture;
  REQUIRE(job_fixture.repository->schedule_notice(
      event(event_id_1, "event:job-replay"), job(), notice_payload()));
  auto replay_job = job(300);
  replay_job.job_id = std::string{outbox_id_1};
  replay_job.created_at_ms = 200;
  auto replay_job_payload = notice_payload(300);
  replay_job_payload.notice.notice_id = std::string{event_id_2};
  replay_job_payload.notice.token_id = std::string{lease_2};
  REQUIRE_FALSE(job_fixture.repository->schedule_notice(
      event(event_id_2, "event:job-replay", 200), replay_job,
      replay_job_payload));
  replay_job.max_attempts = 6;
  REQUIRE_THROWS(job_fixture.repository->schedule_notice(
      event(event_id_2, "event:job-replay", 200), replay_job,
      replay_job_payload));
}

TEST_CASE("retry exhaustion and explicit quarantine remain inspectable",
          "[durable][dead-letter]") {
  DurableFixture fixture;
  auto limited = job();
  limited.max_attempts = 1;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:limited"), limited, notice_payload()));
  const auto claimed = fixture.repository->claim_due_job(200, 1'000, "instance",
                                                         std::string{lease_1});
  REQUIRE(claimed.has_value());
  REQUIRE(fixture.repository->fail_job(*claimed, 200, 205, "handler_exception",
                                       true) == WorkMutationStatus::applied);
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_job(205, 1'000, "instance", std::string{lease_2})
          .has_value());
  REQUIRE(fixture.repository->health(205).dead_jobs == 1);
  const auto dead = fixture.repository->dead(10);
  REQUIRE(dead.size() == 1);
  REQUIRE(dead[0].error_code.has_value());
  REQUIRE(dead[0].error_code == "handler_exception");
}

TEST_CASE("scheduler health includes expired claimed jobs in lag",
          "[durable][health][scheduler][lease]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:health-expired-job"), job(100),
      notice_payload()));
  const auto claimed = fixture.repository->claim_due_job(100, 200, "instance",
                                                         std::string{lease_1});
  REQUIRE(claimed.has_value());

  const auto active = fixture.repository->health(199);
  REQUIRE(active.claimed_jobs == 1);
  REQUIRE(active.scheduler_lag_ms == 0);

  const auto expired = fixture.repository->health(250);
  REQUIRE(expired.claimed_jobs == 1);
  REQUIRE(expired.scheduler_lag_ms == 150);
}

TEST_CASE("unstarted claims release without consuming an attempt",
          "[durable][shutdown][lease]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:release-job"), job(100), notice_payload()));
  const auto claimed_job = fixture.repository->claim_due_job(
      100, 1'000, "instance", std::string{lease_1});
  REQUIRE(claimed_job.has_value());
  REQUIRE(fixture.repository->release_job(*claimed_job, 100) ==
          WorkMutationStatus::applied);
  const auto reclaimed_job = fixture.repository->claim_due_job(
      100, 1'000, "instance", std::string{lease_2});
  REQUIRE(reclaimed_job.has_value());
  REQUIRE(reclaimed_job->attempt_count == 1);

  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_2, "event:release-outbox"),
      notice_outbox(outbox_id_1, "outbox:release"), notice_payload()));
  const auto claimed_outbox = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_1}, false);
  REQUIRE(claimed_outbox.has_value());
  REQUIRE(fixture.repository->release_outbox(*claimed_outbox, 100) ==
          WorkMutationStatus::applied);
  const auto reclaimed_outbox = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_2}, false);
  REQUIRE(reclaimed_outbox.has_value());
  REQUIRE(reclaimed_outbox->attempt_count == 1);
  REQUIRE_FALSE(reclaimed_outbox->first_attempt_at_ms.has_value());
}

TEST_CASE(
    "delegated jobs renew and disabled jobs defer without consuming attempts",
    "[durable][scheduler][lease][defer]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:defer-job"), job(100), notice_payload()));
  const auto claimed = fixture.repository->claim_due_job(100, 200, "instance",
                                                         std::string{lease_1});
  REQUIRE(claimed);
  REQUIRE(fixture.repository->extend_job_lease(*claimed, 100, 500) ==
          WorkMutationStatus::applied);
  REQUIRE_FALSE(fixture.repository->claim_due_job(200, 300, "competitor",
                                                  std::string{lease_2}));
  REQUIRE(
      fixture.repository->defer_job(*claimed, 200, 600, "feature_disabled") ==
      WorkMutationStatus::applied);
  REQUIRE_FALSE(fixture.repository->claim_due_job(599, 700, "competitor",
                                                  std::string{lease_2}));
  const auto reclaimed = fixture.repository->claim_due_job(
      600, 700, "competitor", std::string{lease_2});
  REQUIRE(reclaimed);
  REQUIRE(reclaimed->attempt_count == 1);
  REQUIRE(fixture.repository->reschedule_job(*reclaimed, 600, 900) ==
          WorkMutationStatus::applied);
  REQUIRE_FALSE(fixture.repository->health(600).last_job_error);
  REQUIRE_FALSE(fixture.repository->claim_due_job(899, 1'000, "competitor",
                                                  std::string{lease_1}));
  const auto recurring = fixture.repository->claim_due_job(
      900, 1'000, "competitor", std::string{lease_1});
  REQUIRE(recurring);
  REQUIRE(recurring->attempt_count == 1);
}

TEST_CASE("submitted claims retain ambiguity and recover after lease expiry",
          "[durable][outbox][lease][unknown]") {
  DurableFixture fixture;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral card")},
      .fail_before_first_send = false,
  };
  REQUIRE(fixture.repository->enqueue_public(
      event(event_id_1, "event:submitted"),
      public_outbox(outbox_id_1, "outbox:submitted", 100), payload));
  const auto claimed = fixture.repository->claim_due_outbox(
      100, 200, "instance-a", std::string{lease_1}, true);
  REQUIRE(claimed.has_value());
  REQUIRE(fixture.repository->mark_public_outbox_submitted(
              *claimed, attempt(100), 300) == WorkMutationStatus::applied);
  REQUIRE(fixture.repository->release_outbox(*claimed, 100) ==
          WorkMutationStatus::stale_claim);

  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(200, 300, "instance-a", std::string{lease_2}, true)
          .has_value());
  const auto reclaimed = fixture.repository->claim_due_outbox(
      300, 400, "instance-a", std::string{lease_2}, true);
  REQUIRE(reclaimed.has_value());
  REQUIRE(reclaimed->attempt_count == 2);
  REQUIRE(reclaimed->first_attempt_at_ms == std::optional<std::int64_t>{100});
  REQUIRE(reclaimed->submission_started_at_ms ==
          std::optional<std::int64_t>{100});
  REQUIRE(fixture.repository->fail_outbox(
              *claimed, 300, 305, "discord_transient",
              OutboxFailureMode::retryable) == WorkMutationStatus::stale_claim);
}

TEST_CASE("unknown outbox kinds claim without Discord readiness",
          "[durable][outbox][unknown][readiness]") {
  DurableFixture fixture;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral card")},
      .fail_before_first_send = false,
  };
  REQUIRE(fixture.repository->enqueue_public(
      event(event_id_1, "event:known"),
      public_outbox(outbox_id_1, "outbox:known", 100), payload));
  auto unknown = public_outbox(outbox_id_3, "outbox:unknown", 100);
  unknown.kind = "future.unknown.v1";
  REQUIRE(fixture.repository->enqueue_public(event(event_id_3, "event:unknown"),
                                             unknown, payload));

  const auto claimed = fixture.repository->claim_due_outbox(
      100, 200, "instance", std::string{lease_1}, false);
  REQUIRE(claimed.has_value());
  REQUIRE(claimed->kind == "future.unknown.v1");
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(100, 200, "instance", std::string{lease_2}, false)
          .has_value());
}

TEST_CASE("Chronicle public effects preserve aggregate causal order",
          "[durable][outbox][chronicle][ordering]") {
  DurableFixture fixture;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Chronicle card")},
      .fail_before_first_send = false,
  };
  auto canon = public_outbox(outbox_id_1, "outbox:chronicle:canon", 200);
  canon.aggregate_type = "chronicle_entry";
  canon.aggregate_id = "entry-1";
  canon.created_at_ms = 100;
  auto retraction =
      public_outbox(outbox_id_2, "outbox:chronicle:retraction", 101);
  retraction.aggregate_type = "chronicle_entry";
  retraction.aggregate_id = "entry-1";
  retraction.created_at_ms = 101;
  REQUIRE(fixture.repository->enqueue_public(
      event(event_id_1, "event:chronicle:canon"), canon, payload));
  REQUIRE(fixture.repository->enqueue_public(
      event(event_id_2, "event:chronicle:retraction", 101), retraction,
      payload));

  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(101, 300, "instance", std::string{lease_1}, true)
          .has_value());

  const auto first = fixture.repository->claim_due_outbox(
      200, 300, "instance", std::string{lease_1}, true);
  REQUIRE(first.has_value());
  REQUIRE(first->outbox_id == outbox_id_1);
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(200, 300, "other", std::string{lease_2}, true)
          .has_value());
  REQUIRE(fixture.repository->mark_public_outbox_submitted(
              *first, attempt(200), 300) == WorkMutationStatus::applied);
  REQUIRE(fixture.repository->complete_public_outbox(*first, 777, 200) ==
          WorkMutationStatus::applied);

  const auto second = fixture.repository->claim_due_outbox(
      200, 300, "instance", std::string{lease_2}, true);
  REQUIRE(second.has_value());
  REQUIRE(second->outbox_id == outbox_id_2);
}

TEST_CASE("wall clock rollback preserves durable transition invariants",
          "[durable][clock][retry]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:clock-job"), job(100), notice_payload()));
  const auto claimed_job = fixture.repository->claim_due_job(
      100, 1'000, "instance", std::string{lease_1});
  REQUIRE(claimed_job.has_value());
  REQUIRE(fixture.repository->fail_job(*claimed_job, 50, 60, "clock_retry",
                                       true) == WorkMutationStatus::applied);
  const auto retried_job = fixture.repository->claim_due_job(
      60, 1'000, "instance", std::string{lease_2});
  REQUIRE(retried_job.has_value());
  REQUIRE(fixture.repository->fail_job(*retried_job, 50, 60, "clock_dead",
                                       false) == WorkMutationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT updated_at_ms = 100 AND terminal_at_ms = 100 "
                 "FROM scheduled_job") == 1);

  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_2, "event:clock-outbox"),
      notice_outbox(outbox_id_1, "outbox:clock"), notice_payload()));
  const auto claimed_outbox = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_1}, false);
  REQUIRE(claimed_outbox.has_value());
  REQUIRE(fixture.repository->fail_outbox(
              *claimed_outbox, 50, 60, "clock_retry",
              OutboxFailureMode::retryable) == WorkMutationStatus::applied);
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(99, 1'000, "instance", std::string{lease_2}, false)
          .has_value());
  const auto retried_outbox = fixture.repository->claim_due_outbox(
      100, 1'000, "instance", std::string{lease_2}, false);
  REQUIRE(retried_outbox.has_value());
  REQUIRE(fixture.repository->fail_outbox(
              *retried_outbox, 50, 60, "clock_failed",
              OutboxFailureMode::failed) == WorkMutationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT available_at_ms = 100 AND updated_at_ms = 100 "
                 "AND terminal_at_ms = 100 FROM outbox_message") == 1);
}

TEST_CASE("pending jobs and outbox rows cancel idempotently",
          "[durable][cancel][state]") {
  DurableFixture fixture;
  REQUIRE(fixture.repository->schedule_notice(
      event(event_id_1, "event:cancel-job"), job(200), notice_payload()));
  REQUIRE(fixture.repository->cancel_job(job_id, 50) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.repository->cancel_job(job_id, 50) ==
          WorkMutationStatus::unchanged);
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_job(200, 1'000, "instance", std::string{lease_1})
          .has_value());

  REQUIRE(fixture.repository->enqueue_notice(
      event(event_id_2, "event:cancel-outbox"),
      notice_outbox(outbox_id_1, "outbox:cancel", 200), notice_payload()));
  REQUIRE(fixture.repository->cancel_outbox(outbox_id_1, 50) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.repository->cancel_outbox(outbox_id_1, 50) ==
          WorkMutationStatus::unchanged);
  REQUIRE_FALSE(
      fixture.repository
          ->claim_due_outbox(200, 1'000, "instance", std::string{lease_1}, true)
          .has_value());
  REQUIRE(
      fixture.repository->cancel_outbox("00000000-0000-4000-8000-000000000999",
                                        50) == WorkMutationStatus::not_found);
}

TEST_CASE("scheduler and outbox recover a job persisted before restart",
          "[durable][restart][scheduler][sqlite]") {
  RestartDatabaseFixture fixture;
  ImmediateDiscord discord;
  sanguinius::test::FakeDiagnostics diagnostics;

  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    REQUIRE(repository.schedule_notice(event(event_id_1, "event:restart-due"),
                                       job(2'000), notice_payload()));

    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   fixture.clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000701",
        32,           100ms};
    sanguinius::SchedulerService scheduler{
        repository,
        fixture.clock,
        ids,
        diagnostics,
        "00000000-0000-4000-8000-000000000701",
        [&outbox] { outbox.wake(); }};
    outbox.start();
    scheduler.start();
    scheduler.stop();
    outbox.stop();
    REQUIRE(repository.health(1'000).pending_jobs == 1);
    REQUIRE(discord.sends() == 0);
  }

  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{2}});
  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    SqlitePendingNoticeRepository notices{context};
    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   fixture.clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000702",
        32,           100ms};
    sanguinius::SchedulerService scheduler{
        repository,
        fixture.clock,
        ids,
        diagnostics,
        "00000000-0000-4000-8000-000000000702",
        [&outbox] { outbox.wake(); }};
    outbox.start();
    scheduler.start();
    scheduler.wake();
    REQUIRE(eventually([&] {
      return notices.pending_count(30, 2'000) == 1 && discord.sends() == 1;
    }));
    scheduler.stop();
    outbox.stop();

    const auto health = repository.health(2'000);
    REQUIRE(health.pending_jobs == 0);
    REQUIRE(health.claimed_jobs == 0);
    REQUIRE(health.pending_outbox == 0);
    REQUIRE(health.claimed_outbox == 0);
  }
}

TEST_CASE("scheduler reclaims an expired job after SQLite restart",
          "[durable][restart][scheduler][lease][sqlite]") {
  RestartDatabaseFixture fixture;
  ImmediateDiscord discord;
  sanguinius::test::FakeDiagnostics diagnostics;

  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    REQUIRE(repository.schedule_notice(
        event(event_id_1, "event:restart-claimed-job"), job(1'000),
        notice_payload()));
    const auto claimed = repository.claim_due_job(
        1'000, 2'000, "00000000-0000-4000-8000-000000000711",
        std::string{lease_1});
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->attempt_count == 1);
  }

  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{2}});
  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    SqlitePendingNoticeRepository notices{context};
    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   fixture.clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000712",
        32,           100ms};
    sanguinius::SchedulerService scheduler{
        repository,
        fixture.clock,
        ids,
        diagnostics,
        "00000000-0000-4000-8000-000000000712",
        [&outbox] { outbox.wake(); }};
    outbox.start();
    scheduler.start();
    scheduler.wake();
    REQUIRE(eventually([&] {
      return notices.pending_count(30, 2'000) == 1 && discord.sends() == 1;
    }));
    scheduler.stop();
    outbox.stop();

    const auto health = repository.health(2'000);
    REQUIRE(health.pending_jobs == 0);
    REQUIRE(health.claimed_jobs == 0);
    REQUIRE(health.job_retries == 1);
  }
}

TEST_CASE("outbox reclaims an unstarted claim after SQLite restart",
          "[durable][restart][outbox][lease][sqlite]") {
  RestartDatabaseFixture fixture;
  ImmediateDiscord discord;
  sanguinius::test::FakeDiagnostics diagnostics;

  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    REQUIRE(repository.enqueue_notice(
        event(event_id_1, "event:restart-claimed-outbox"),
        notice_outbox(outbox_id_1, "outbox:restart-claimed", 1'000),
        notice_payload()));
    const auto claimed = repository.claim_due_outbox(
        1'000, 2'000, "00000000-0000-4000-8000-000000000721",
        std::string{lease_1}, false);
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->attempt_count == 1);
    REQUIRE_FALSE(claimed->submission_started_at_ms.has_value());
  }

  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{2}});
  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    SqlitePendingNoticeRepository notices{context};
    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   fixture.clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000722",
        32,           100ms};
    outbox.start();
    outbox.wake();
    REQUIRE(eventually([&] {
      return notices.pending_count(30, 2'000) == 1 && discord.sends() == 1;
    }));
    outbox.stop();

    const auto health = repository.health(2'000);
    REQUIRE(health.pending_outbox == 0);
    REQUIRE(health.claimed_outbox == 0);
    REQUIRE(health.outbox_retries == 1);
  }
}

TEST_CASE("outbox quarantines ambiguous delivery after a boot restart",
          "[durable][restart][outbox][clock][sqlite]") {
  RestartDatabaseFixture fixture;
  ImmediateDiscord discord;
  sanguinius::test::FakeDiagnostics diagnostics;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral card")},
      .fail_before_first_send = false,
  };

  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    REQUIRE(repository.enqueue_public(
        event(event_id_1, "event:restart-ambiguous"),
        public_outbox(outbox_id_1, "outbox:restart-ambiguous", 1'000),
        payload));
    const auto claimed = repository.claim_due_outbox(
        1'000, 2'000, "00000000-0000-4000-8000-000000000731",
        std::string{lease_1}, true);
    REQUIRE(claimed.has_value());
    REQUIRE(repository.mark_public_outbox_submitted(
                *claimed,
                {.wall_time_ms = 1'000,
                 .elapsed_realtime_ms = 1'000,
                 .boot_session_id = "00000000-0000-4000-8000-000000000601"},
                2'000) == WorkMutationStatus::applied);
  }

  sanguinius::test::FakeClock restarted_clock{
      std::chrono::sys_seconds{std::chrono::seconds{2}},
      "00000000-0000-4000-8000-000000000602"};
  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   restarted_clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000732",
        32,           100ms};
    outbox.start();
    outbox.wake();
    REQUIRE(eventually([&] {
      const auto health = repository.health(2'000);
      return health.failed_outbox == 1 &&
             health.last_outbox_error ==
                 std::optional<std::string>{"discord_unknown_outcome_stale"};
    }));
    outbox.stop();

    REQUIRE(discord.sends() == 0);
    REQUIRE(repository.health(2'000).outbox_retries == 1);
  }
}

TEST_CASE("outbox quarantines an exhausted submitted attempt after restart",
          "[durable][restart][outbox][clock][sqlite][exhaustion]") {
  RestartDatabaseFixture fixture;
  ImmediateDiscord discord;
  sanguinius::test::FakeDiagnostics diagnostics;
  const sanguinius::PublicOutboxPayload payload{
      .request = {.guild_id = 10,
                  .channel_id = 20,
                  .message = sanguinius::text_message("Neutral card")},
      .fail_before_first_send = false,
  };

  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    auto enqueue =
        public_outbox(outbox_id_1, "outbox:restart-final-ambiguous", 1'000);
    enqueue.max_attempts = 1;
    REQUIRE(repository.enqueue_public(
        event(event_id_1, "event:restart-final-ambiguous"), enqueue, payload));
    const auto claimed = repository.claim_due_outbox(
        1'000, 2'000, "00000000-0000-4000-8000-000000000741",
        std::string{lease_1}, true);
    REQUIRE(claimed.has_value());
    REQUIRE(claimed->attempt_count == claimed->max_attempts);
    REQUIRE(repository.mark_public_outbox_submitted(
                *claimed,
                {.wall_time_ms = 1'000,
                 .elapsed_realtime_ms = 1'000,
                 .boot_session_id = "00000000-0000-4000-8000-000000000611"},
                2'000) == WorkMutationStatus::applied);
  }

  sanguinius::test::FakeClock restarted_clock{
      std::chrono::sys_seconds{std::chrono::seconds{2}},
      "00000000-0000-4000-8000-000000000612"};
  {
    auto context = fixture.open();
    SqliteDurableWorkRepository repository{context};
    sanguinius::UuidV4Generator ids;
    sanguinius::OutboxService outbox{
        repository,   restarted_clock,
        ids,          diagnostics,
        discord,      discord,
        {10, 20, 30}, "00000000-0000-4000-8000-000000000742",
        32,           100ms};
    outbox.start();
    outbox.wake();
    REQUIRE(eventually([&] {
      const auto health = repository.health(2'000);
      return health.failed_outbox == 1 && health.dead_outbox == 0 &&
             health.last_outbox_error ==
                 std::optional<std::string>{"discord_unknown_outcome_stale"};
    }));
    outbox.stop();

    REQUIRE(discord.sends() == 0);
    REQUIRE(repository.health(2'000).outbox_retries == 0);
  }
}
