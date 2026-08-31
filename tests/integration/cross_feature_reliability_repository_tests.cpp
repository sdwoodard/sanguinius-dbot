// Persistent cross-feature reliability and recovery coverage.
#include "sanguinius/ai_generation.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_ai_generation_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_provider_circuit_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_retention_repository.hpp"
#include "sanguinius/persistence/sqlite_safety_control_repository.hpp"
#include "sanguinius/provider_circuit.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

struct M18Fixture {
  M18Fixture() {
    {
      auto database = sanguinius::persistence::Database::open_migration(
          temporary.path(), 500ms);
      sanguinius::persistence::Migrator migrator{
          sanguinius::persistence::production_migrations(),
          {"test", "revision"},
          clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 16);
    }
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            sanguinius::persistence::Database::open_runtime(temporary.path(),
                                                            500ms));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 1);
    identities.ensure_user({30, "Owner", "owner", false, 1});
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
};

class FramingUsageAiClient final : public sanguinius::AiClient {
public:
  [[nodiscard]] sanguinius::AiResult generate(
      const sanguinius::AiRequest &, std::stop_token,
      const std::function<void()> &transmission_started = {}) const override {
    if (transmission_started)
      transmission_started();
    return {.text = "safe response",
            .provider_request_id = "provider-request",
            .input_tokens = 100,
            .output_tokens = 5};
  }
};

enum class FenceBehavior { cancel_before_send, fail_after_send };

class FencedFailureAiClient final : public sanguinius::AiClient {
public:
  explicit FencedFailureAiClient(const FenceBehavior behavior)
      : behavior_{behavior} {}

  [[nodiscard]] sanguinius::AiResult generate(
      const sanguinius::AiRequest &, std::stop_token,
      const std::function<void()> &transmission_started = {}) const override {
    if (behavior_ == FenceBehavior::cancel_before_send)
      throw sanguinius::OperationCancelled{};
    if (transmission_started)
      transmission_started();
    throw sanguinius::AiProviderError{
        sanguinius::AiProviderErrorCategory::timeout, "request-timeout-42"};
  }

private:
  FenceBehavior behavior_;
};

[[nodiscard]] sanguinius::AiGenerationReservation
reservation(const std::size_t index, const std::int64_t cost = 200) {
  return {.attempt_id = uuid(100 + index),
          .idempotency_key = "ai:test:" + std::to_string(index),
          .requester_user_id = std::string{"30"},
          .purpose = sanguinius::AiPurpose::direct,
          .priority = sanguinius::AiPriority::direct,
          .model = "audited-test-model",
          .input_rate = 1'000'000,
          .output_rate = 1'000'000,
          .reserved_input_tokens = 100,
          .reserved_output_tokens = 100,
          .reserved_micro_usd = cost,
          .now_ms = 1'000};
}

} // namespace

TEST_CASE("AI budget reservations serialize before provider submission",
          "[cross-feature][ai][budget][concurrency]") {
  M18Fixture fixture;
  auto second_context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          sanguinius::persistence::Database::open_runtime(
              fixture.temporary.path(), 500ms));
  sanguinius::persistence::SqliteAiGenerationRepository first{fixture.context};
  sanguinius::persistence::SqliteAiGenerationRepository second{second_context};
  sanguinius::AiGenerationPolicy policy{
      .rolling_day_micro_usd = 300,
      .calendar_month_micro_usd = 1'000,
      .rolling_day_generations = 300,
      .direct_user_ten_minute_generations = 30,
      .maximum_input_bytes = 16'000,
      .maximum_output_tokens = 500,
      .input_rate_micro_usd_per_million_tokens = 1'000'000,
      .output_rate_micro_usd_per_million_tokens = 1'000'000,
      .model = "audited-test-model"};
  std::barrier ready{2};
  std::array<sanguinius::AiGenerationAdmission, 2> outcomes{};
  std::array<std::exception_ptr, 2> failures{};
  std::jthread left{[&] {
    ready.arrive_and_wait();
    try {
      outcomes[0] = first.reserve(10, reservation(1), policy).status;
    } catch (...) {
      failures[0] = std::current_exception();
    }
  }};
  std::jthread right{[&] {
    ready.arrive_and_wait();
    try {
      outcomes[1] = second.reserve(10, reservation(2), policy).status;
    } catch (...) {
      failures[1] = std::current_exception();
    }
  }};
  left.join();
  right.join();
  REQUIRE_FALSE(failures[0]);
  REQUIRE_FALSE(failures[1]);
  const auto accepted =
      (outcomes[0] == sanguinius::AiGenerationAdmission::accepted ? 1 : 0) +
      (outcomes[1] == sanguinius::AiGenerationAdmission::accepted ? 1 : 0);
  const auto rejected =
      (outcomes[0] == sanguinius::AiGenerationAdmission::daily_cost ? 1 : 0) +
      (outcomes[1] == sanguinius::AiGenerationAdmission::daily_cost ? 1 : 0);
  REQUIRE(accepted == 1);
  REQUIRE(rejected == 1);
}

TEST_CASE("AI service reserves protocol framing at the full input ceiling",
          "[cross-feature][ai][budget][framing]") {
  M18Fixture fixture;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::AiGenerationService service{
      std::make_unique<FramingUsageAiClient>(),
      std::make_unique<sanguinius::persistence::SqliteAiGenerationRepository>(
          fixture.context),
      fixture.clock,
      ids,
      {10, 20, 30},
      {.input_rate_micro_usd_per_million_tokens = 1'000'000,
       .output_rate_micro_usd_per_million_tokens = 1'000'000,
       .model = "audited-test-model"}};

  const auto result =
      service.generate({.instructions = "x",
                        .conversation = {},
                        .max_output_tokens = 10,
                        .json_schema = std::nullopt,
                        .purpose = sanguinius::AiPurpose::direct,
                        .priority = sanguinius::AiPriority::direct,
                        .requester_user_id = std::string{"30"},
                        .idempotency_key = "ai:framing-reservation"},
                       std::stop_token{});
  REQUIRE(result.text == "safe response");
  auto attempt = fixture.context->connection().prepare(
      "SELECT reserved_input_tokens,actual_input_tokens,reserved_micro_usd,"
      "actual_micro_usd,state FROM ai_generation_attempt WHERE "
      "idempotency_key='ai:framing-reservation'");
  REQUIRE(attempt.step());
  REQUIRE(attempt.column_int64(0) == 16'000);
  REQUIRE(attempt.column_int64(1) == 100);
  REQUIRE(attempt.column_int64(2) == 16'010);
  REQUIRE(attempt.column_int64(3) == 105);
  REQUIRE(attempt.column_text(4) == "succeeded");
}

TEST_CASE("AI submission fence releases only work never sent to the provider",
          "[cross-feature][ai][budget][submission-fence]") {
  M18Fixture fixture;
  sanguinius::test::FakePersistentIdGenerator ids;
  const auto request = [](std::string idempotency_key) {
    return sanguinius::AiRequest{.instructions = "x",
                                 .conversation = {},
                                 .max_output_tokens = 10,
                                 .json_schema = std::nullopt,
                                 .purpose = sanguinius::AiPurpose::direct,
                                 .priority = sanguinius::AiPriority::direct,
                                 .requester_user_id = std::string{"30"},
                                 .idempotency_key = std::move(idempotency_key)};
  };
  const sanguinius::AiGenerationPolicy policy{
      .input_rate_micro_usd_per_million_tokens = 1'000'000,
      .output_rate_micro_usd_per_million_tokens = 1'000'000,
      .model = "audited-test-model"};
  {
    sanguinius::AiGenerationService service{
        std::make_unique<FencedFailureAiClient>(
            FenceBehavior::cancel_before_send),
        std::make_unique<sanguinius::persistence::SqliteAiGenerationRepository>(
            fixture.context),
        fixture.clock,
        ids,
        {10, 20, 30},
        policy};
    REQUIRE_THROWS_AS(service.generate(request("ai:fence:unsent"), {}),
                      sanguinius::OperationCancelled);
  }
  {
    sanguinius::AiGenerationService service{
        std::make_unique<FencedFailureAiClient>(FenceBehavior::fail_after_send),
        std::make_unique<sanguinius::persistence::SqliteAiGenerationRepository>(
            fixture.context),
        fixture.clock,
        ids,
        {10, 20, 30},
        policy};
    REQUIRE_THROWS_AS(service.generate(request("ai:fence:sent"), {}),
                      sanguinius::AiProviderError);
  }

  auto attempts = fixture.context->connection().prepare(
      "SELECT idempotency_key,state,provider_sent,provider_request_id FROM "
      "ai_generation_attempt "
      "WHERE idempotency_key IN ('ai:fence:unsent','ai:fence:sent') ORDER BY "
      "idempotency_key");
  REQUIRE(attempts.step());
  REQUIRE(attempts.column_text(0) == "ai:fence:sent");
  REQUIRE(attempts.column_text(1) == "failed");
  REQUIRE(attempts.column_int64(2) == 1);
  REQUIRE(attempts.column_text(3) == "request-timeout-42");
  REQUIRE(attempts.step());
  REQUIRE(attempts.column_text(0) == "ai:fence:unsent");
  REQUIRE(attempts.column_text(1) == "cancelled");
  REQUIRE(attempts.column_int64(2) == 0);
  REQUIRE(attempts.column_is_null(3));
}

TEST_CASE("persistent provider circuits open probe recover and restart auth",
          "[cross-feature][provider][circuit]") {
  M18Fixture fixture;
  sanguinius::persistence::SqliteProviderCircuitRepository circuit{
      fixture.context};
  for (std::size_t index = 0; index < 3; ++index) {
    circuit.failed("openai_tts", sanguinius::ProviderCircuitFailure::retryable,
                   "timeout", static_cast<std::int64_t>(index * 1'000),
                   uuid(200 + index));
  }
  REQUIRE(circuit.state("openai_tts") == "open");
  REQUIRE_FALSE(circuit.admit("openai_tts", 3'000, uuid(210)));
  REQUIRE(circuit.admit("openai_tts", 5 * 60 * 1'000 + 2'000, uuid(211)));
  REQUIRE_FALSE(circuit.admit("openai_tts", 5 * 60 * 1'000 + 2'001, uuid(212)));
  circuit.failed("openai_tts", sanguinius::ProviderCircuitFailure::ignored,
                 "cancelled", 5 * 60 * 1'000 + 2'002, uuid(213));
  REQUIRE(circuit.admit("openai_tts", 5 * 60 * 1'000 + 2'003, uuid(214)));
  circuit.restart("openai_tts", 5 * 60 * 1'000 + 2'004, uuid(215));
  REQUIRE(circuit.admit("openai_tts", 5 * 60 * 1'000 + 2'005, uuid(216)));
  circuit.succeeded("openai_tts", 5 * 60 * 1'000 + 2'006, uuid(217));
  REQUIRE(circuit.state("openai_tts") == "closed");

  circuit.failed("openai_transcription",
                 sanguinius::ProviderCircuitFailure::authentication,
                 "authentication", 1'000, uuid(220));
  circuit.failed("openai_transcription",
                 sanguinius::ProviderCircuitFailure::retryable, "timeout",
                 2'000, uuid(221));
  REQUIRE_FALSE(circuit.admit("openai_transcription", 99'000'000, uuid(222)));
  circuit.restart("openai_transcription", 99'000'001, uuid(223));
  REQUIRE(circuit.admit("openai_transcription", 99'000'002, uuid(224)));
}

TEST_CASE("text authentication circuit cannot be downgraded in flight",
          "[cross-feature][ai][provider][circuit][authentication]") {
  M18Fixture fixture;
  sanguinius::persistence::SqliteAiGenerationRepository repository{
      fixture.context};
  repository.provider_failed(
      sanguinius::AiProviderErrorCategory::authentication, 1'000, uuid(225));
  repository.provider_failed(sanguinius::AiProviderErrorCategory::timeout,
                             2'000, uuid(226));
  REQUIRE(repository.admit_provider(99'000'000, uuid(227)) ==
          sanguinius::ProviderCircuitAdmission::open);
  auto state = fixture.context->connection().prepare(
      "SELECT state,indefinite,retry_after_ms,last_error_code FROM "
      "provider_circuit_state WHERE provider='openai_text'");
  REQUIRE(state.step());
  REQUIRE(state.column_text(0) == "open");
  REQUIRE(state.column_int64(1) == 1);
  REQUIRE(state.column_is_null(2));
  REQUIRE(state.column_text(3) == "authentication");
}

TEST_CASE("AI startup releases unsent reservations and abandoned probes",
          "[cross-feature][ai][recovery]") {
  M18Fixture fixture;
  sanguinius::persistence::SqliteAiGenerationRepository repository{
      fixture.context};
  sanguinius::AiGenerationPolicy policy{
      .rolling_day_micro_usd = 300,
      .calendar_month_micro_usd = 1'000,
      .rolling_day_generations = 300,
      .direct_user_ten_minute_generations = 30,
      .maximum_input_bytes = 16'000,
      .maximum_output_tokens = 500,
      .input_rate_micro_usd_per_million_tokens = 1'000'000,
      .output_rate_micro_usd_per_million_tokens = 1'000'000,
      .model = "audited-test-model"};
  REQUIRE(repository.reserve(10, reservation(40), policy).status ==
          sanguinius::AiGenerationAdmission::accepted);
  REQUIRE(repository.recover_reserved(2'000) == 1);
  auto attempt = fixture.context->connection().prepare(
      "SELECT state,provider_sent FROM ai_generation_attempt WHERE "
      "attempt_id=?");
  attempt.bind(1, uuid(140));
  REQUIRE(attempt.step());
  REQUIRE(attempt.column_text(0) == "cancelled");
  REQUIRE(attempt.column_int64(1) == 0);
  REQUIRE(repository.reserve(10, reservation(41), policy).status ==
          sanguinius::AiGenerationAdmission::accepted);

  for (std::size_t index = 0; index < 3; ++index) {
    repository.provider_failed(sanguinius::AiProviderErrorCategory::timeout,
                               static_cast<std::int64_t>(3'000 + index),
                               uuid(240 + index));
  }
  REQUIRE(repository.admit_provider(5 * 60 * 1'000 + 4'000, uuid(243)) ==
          sanguinius::ProviderCircuitAdmission::allowed);
  repository.release_provider_probe(5 * 60 * 1'000 + 4'001, uuid(244));
  REQUIRE(repository.admit_provider(5 * 60 * 1'000 + 4'002, uuid(245)) ==
          sanguinius::ProviderCircuitAdmission::allowed);
  repository.restart_provider(5 * 60 * 1'000 + 4'003, uuid(246));
  REQUIRE(repository.admit_provider(5 * 60 * 1'000 + 4'004, uuid(247)) ==
          sanguinius::ProviderCircuitAdmission::allowed);
}

TEST_CASE("no-op runtime safety receipts cannot undo a later transition",
          "[cross-feature][safety][idempotency]") {
  M18Fixture fixture;
  sanguinius::persistence::SqliteRuntimeFeatureControlRepository controls{
      fixture.context};
  REQUIRE(controls.set("tts", false, 30, uuid(250), "safety:no-op", 1'000) ==
          sanguinius::RuntimeControlMutation::unchanged);
  REQUIRE(controls.set("tts", true, 30, uuid(251), "safety:disable", 1'001) ==
          sanguinius::RuntimeControlMutation::applied);
  REQUIRE(controls.set("tts", false, 30, uuid(252), "safety:no-op", 1'002) ==
          sanguinius::RuntimeControlMutation::duplicate);
  const auto snapshot = controls.snapshot();
  const auto tts =
      std::ranges::find(snapshot, std::string_view{"tts"},
                        &sanguinius::RuntimeFeatureControl::feature);
  REQUIRE(tts != snapshot.end());
  REQUIRE(tts->disabled);
}

TEST_CASE(
    "retention tombstones terminal sealed notice prose but keeps identity",
    "[cross-feature][retention][privacy]") {
  M18Fixture fixture;
  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};
  const auto created = notices.create_with_token(
      {.notice_id = uuid(300),
       .token_id = uuid(301),
       .target_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .notice_type = "test_notice",
       .content = {.title = "Sealed secret",
                   .body = "Private prose must expire."},
       .source_aggregate_type = "owner_test",
       .source_aggregate_id = "retention",
       .expires_at_ms = 10'000,
       .notice_idempotency_key = "notice:retention:create",
       .token_idempotency_key = "notice:retention:token",
       .created_at_ms = 1});
  REQUIRE(created.created);
  REQUIRE(notices.cancel(uuid(300), 30, 2) ==
          sanguinius::PendingNoticeMutationStatus::applied);

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto notice_payload = [](const std::size_t value, std::string body) {
    return sanguinius::NoticeOutboxPayload{
        .notice = {.notice_id = uuid(value),
                   .token_id = uuid(value + 1),
                   .target_user_id = 30,
                   .guild_id = 10,
                   .channel_id = 20,
                   .notice_type = "test_notice",
                   .content = {.title = "Sealed durable secret",
                               .body = std::move(body)},
                   .source_aggregate_type = "owner_test",
                   .source_aggregate_id = "retention",
                   .expires_at_ms = 10'000,
                   .notice_idempotency_key =
                       "notice:retention:" + std::to_string(value),
                   .token_idempotency_key =
                       "token:retention:" + std::to_string(value),
                   .created_at_ms = 1},
        .announce_publicly = false};
  };
  const auto event = [](const std::size_t value, std::string type) {
    return sanguinius::EventJournalEntry{
        .event_id = uuid(value),
        .event_type = std::move(type),
        .aggregate_type = "owner_test",
        .aggregate_id = "retention",
        .actor_user_id = 30,
        .guild_id = 10,
        .channel_id = 20,
        .source_message_id = std::nullopt,
        .occurred_at_ms = 1,
        .recorded_at_ms = 1,
        .correlation_id = "retention-test",
        .causation_id = std::nullopt,
        .idempotency_key = "event:retention:" + std::to_string(value)};
  };
  const auto outbox = [](const std::size_t value, std::string key) {
    const auto id = uuid(value);
    return sanguinius::OutboxEnqueue{
        .outbox_id = id,
        .kind = std::string{sanguinius::pending_notice_outbox_kind},
        .aggregate_type = "owner_test",
        .aggregate_id = "retention",
        .target_guild_id = 10,
        .target_channel_id = 20,
        .target_user_id = 30,
        .available_at_ms = 1,
        .max_attempts = 5,
        .idempotency_key = std::move(key),
        .provider_nonce = sanguinius::discord_nonce_from_uuid(id),
        .created_at_ms = 1};
  };

  REQUIRE(durable.enqueue_notice(
      event(310, "owner.test_notice_queued.v1"),
      outbox(311, "outbox:retention:delivered"),
      notice_payload(312, "Delivered durable private prose.")));
  auto deliver = fixture.context->connection().prepare(
      "UPDATE outbox_message SET state='delivered',delivered_at_ms=2,"
      "terminal_at_ms=2,updated_at_ms=2 WHERE outbox_id=?");
  deliver.bind(1, uuid(311));
  deliver.execute();

  REQUIRE(durable.schedule_notice(
      event(314, "owner.test_notice_scheduled.v1"),
      {.job_id = uuid(315),
       .job_type = std::string{sanguinius::owner_test_notice_job_type},
       .aggregate_type = "owner_test",
       .aggregate_id = "retention",
       .due_at_ms = 1,
       .max_attempts = 5,
       .idempotency_key = "job:retention:completed",
       .created_at_ms = 1},
      notice_payload(316, "Scheduled durable private prose.")));
  auto complete_job = fixture.context->connection().prepare(
      "UPDATE scheduled_job SET state='completed',completed_at_ms=2,"
      "terminal_at_ms=2,updated_at_ms=2 WHERE job_id=?");
  complete_job.bind(1, uuid(315));
  complete_job.execute();

  REQUIRE(durable.enqueue_notice(
      event(318, "owner.test_notice_queued.v1"),
      outbox(319, "outbox:retention:unknown"),
      notice_payload(320, "Unknown-outcome private prose must remain.")));
  auto fail_unknown = fixture.context->connection().prepare(
      "UPDATE outbox_message SET state='failed',terminal_at_ms=2,"
      "updated_at_ms=2,last_error_code='discord_unknown_outcome_stale' "
      "WHERE outbox_id=?");
  fail_unknown.bind(1, uuid(319));
  fail_unknown.execute();

  auto snapshot = fixture.context->connection().prepare(
      "INSERT INTO interaction_list_snapshot(snapshot_id,snapshot_kind,"
      "viewer_user_id,subject_user_id,owner_view,item_count,created_at_ms,"
      "expires_at_ms) VALUES(?,'chronicle_titles','30','30',0,1,1,10000)");
  snapshot.bind(1, uuid(303));
  snapshot.execute();
  auto item = fixture.context->connection().prepare(
      "INSERT INTO interaction_list_snapshot_item(snapshot_id,position,"
      "item_id) VALUES(?,0,?)");
  item.bind(1, uuid(303));
  item.bind(2, uuid(304));
  item.execute();

  sanguinius::persistence::SqliteRetentionRepository retention{fixture.context};
  const auto counts =
      retention.run(31LL * 24 * 60 * 60 * 1'000, uuid(302),
                    sanguinius::RetentionCounts{.tts_cache_removals = 2});
  REQUIRE(counts.notice_payloads == 3);
  REQUIRE(counts.interaction_snapshots == 1);
  REQUIRE(counts.tts_cache_removals == 2);
  auto run_audit = fixture.context->connection().prepare(
      "SELECT state,counts_json,error_code FROM retention_run WHERE run_id=?");
  run_audit.bind(1, uuid(302));
  REQUIRE(run_audit.step());
  REQUIRE(run_audit.column_text(0) == "completed");
  const auto run_counts = nlohmann::json::parse(run_audit.column_text(1));
  REQUIRE(run_counts.at("tts_cache_removals") == 2);
  REQUIRE(run_counts.at("tts_cache_failures") == 0);
  REQUIRE(run_audit.column_is_null(2));
  auto query = fixture.context->connection().prepare(
      "SELECT state,payload_json FROM pending_notice WHERE notice_id=?");
  query.bind(1, uuid(300));
  REQUIRE(query.step());
  REQUIRE(query.column_text(0) == "cancelled");
  REQUIRE(query.column_text(1).find("Private prose") == std::string::npos);
  REQUIRE(query.column_text(1).find("Content removed by retention") !=
          std::string::npos);
  auto durable_payloads = fixture.context->connection().prepare(
      "SELECT payload_json FROM outbox_message WHERE outbox_id=? UNION ALL "
      "SELECT payload_json FROM scheduled_job WHERE job_id=?");
  durable_payloads.bind(1, uuid(311));
  durable_payloads.bind(2, uuid(315));
  std::size_t durable_count{};
  while (durable_payloads.step()) {
    ++durable_count;
    REQUIRE(durable_payloads.column_text(0).find("private prose") ==
            std::string::npos);
    REQUIRE(durable_payloads.column_text(0).find(
                "Content removed by retention") != std::string::npos);
  }
  REQUIRE(durable_count == 2);
  auto unresolved = fixture.context->connection().prepare(
      "SELECT payload_json FROM outbox_message WHERE outbox_id=?");
  unresolved.bind(1, uuid(319));
  REQUIRE(unresolved.step());
  REQUIRE(unresolved.column_text(0).find("Unknown-outcome private prose") !=
          std::string::npos);
  auto snapshots = fixture.context->connection().prepare(
      "SELECT COUNT(*) FROM interaction_list_snapshot WHERE snapshot_id=?");
  snapshots.bind(1, uuid(303));
  REQUIRE(snapshots.step());
  REQUIRE(snapshots.column_int64(0) == 0);

  const auto replayed_notice = notices.create_with_token(
      {.notice_id = uuid(300),
       .token_id = uuid(301),
       .target_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .notice_type = "test_notice",
       .content = {.title = "Sealed secret",
                   .body = "Private prose must expire."},
       .source_aggregate_type = "owner_test",
       .source_aggregate_id = "retention",
       .expires_at_ms = 10'000,
       .notice_idempotency_key = "notice:retention:create",
       .token_idempotency_key = "notice:retention:token",
       .created_at_ms = 1});
  REQUIRE_FALSE(replayed_notice.created);
  REQUIRE_FALSE(durable.enqueue_notice(
      event(310, "owner.test_notice_queued.v1"),
      outbox(311, "outbox:retention:delivered"),
      notice_payload(312, "Delivered durable private prose.")));
  REQUIRE_FALSE(durable.schedule_notice(
      event(314, "owner.test_notice_scheduled.v1"),
      {.job_id = uuid(315),
       .job_type = std::string{sanguinius::owner_test_notice_job_type},
       .aggregate_type = "owner_test",
       .aggregate_id = "retention",
       .due_at_ms = 1,
       .max_attempts = 5,
       .idempotency_key = "job:retention:completed",
       .created_at_ms = 1},
      notice_payload(316, "Scheduled durable private prose.")));
  REQUIRE_THROWS(notices.create_with_token(
      {.notice_id = uuid(300),
       .token_id = uuid(301),
       .target_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .notice_type = "test_notice",
       .content = {.title = "Sealed secret", .body = "Different prose."},
       .source_aggregate_type = "owner_test",
       .source_aggregate_id = "retention",
       .expires_at_ms = 10'000,
       .notice_idempotency_key = "notice:retention:create",
       .token_idempotency_key = "notice:retention:token",
       .created_at_ms = 1}));
  REQUIRE_THROWS(
      durable.enqueue_notice(event(310, "owner.test_notice_queued.v1"),
                             outbox(311, "outbox:retention:delivered"),
                             notice_payload(312, "Different durable prose.")));
}

TEST_CASE("retention records failed database runs after atomic rollback",
          "[cross-feature][retention][audit][failure]") {
  M18Fixture fixture;
  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};
  static_cast<void>(notices.create_with_token(
      {.notice_id = uuid(321),
       .token_id = uuid(322),
       .target_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .notice_type = "test_notice",
       .content = {.title = "Audit secret", .body = "Must remain on rollback."},
       .source_aggregate_type = "owner_test",
       .source_aggregate_id = "retention-failure",
       .expires_at_ms = 10'000,
       .notice_idempotency_key = "notice:retention:failed-run",
       .token_idempotency_key = "token:retention:failed-run",
       .created_at_ms = 1}));
  REQUIRE(notices.cancel(uuid(321), 30, 2) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  fixture.context->connection().execute(
      "CREATE TRIGGER inject_retention_failure BEFORE UPDATE OF payload_json "
      "ON pending_notice BEGIN SELECT RAISE(ABORT,'injected'); END");

  sanguinius::persistence::SqliteRetentionRepository retention{fixture.context};
  const sanguinius::RetentionCounts initial{.tts_cache_failures = 1};
  REQUIRE_THROWS(
      retention.run(31LL * 24 * 60 * 60 * 1'000, uuid(323), initial));

  auto audit = fixture.context->connection().prepare(
      "SELECT state,counts_json,error_code FROM retention_run WHERE run_id=?");
  audit.bind(1, uuid(323));
  REQUIRE(audit.step());
  REQUIRE(audit.column_text(0) == "failed");
  const auto counts = nlohmann::json::parse(audit.column_text(1));
  REQUIRE(counts.at("tts_cache_failures") == 1);
  REQUIRE(counts.at("notice_payloads") == 0);
  REQUIRE(audit.column_text(2) == "database_retention_failed");
  auto payload = fixture.context->connection().prepare(
      "SELECT payload_json FROM pending_notice WHERE notice_id=?");
  payload.bind(1, uuid(321));
  REQUIRE(payload.step());
  REQUIRE(payload.column_text(0).find("Must remain on rollback") !=
          std::string::npos);
}

TEST_CASE("daily retention purges speech and terminal TTS usage without Vox",
          "[cross-feature][retention][speech][tts]") {
  M18Fixture fixture;
  auto application = fixture.context->connection().prepare(
      "INSERT INTO application_instance(instance_id,application_version,"
      "git_revision,hostname,process_id,started_at_ms,stopped_at_ms,"
      "stop_reason) VALUES(?,'test','test','test-host',1,0,1,"
      "'clean_shutdown')");
  application.bind(1, uuid(330));
  application.execute();
  auto session = fixture.context->connection().prepare(
      "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
      "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
      "state_version,connection_generation,started_at_ms,last_active_at_ms,"
      "ended_at_ms,end_reason) VALUES(?,'10','20','21','30',?,'inactive',1,"
      "1,0,1,1,'test_complete')");
  session.bind(1, uuid(331));
  session.bind(2, uuid(330));
  session.execute();
  auto speech = fixture.context->connection().prepare(
      "INSERT INTO speech_item(speech_id,voice_session_id,source_kind,text,"
      "text_hash,scalar_count,provider,model,voice_id,priority,narration_rank,"
      "state,state_version,earliest_at_ms,interruptible,deduplication_key,"
      "created_at_ms,terminal_at_ms,last_error_code) VALUES(?,?,'test',NULL,"
      "?,1,'static','static-v1','onyx',300,0,'failed',1,0,0,"
      "'speech:retention:terminal',0,1,'test_complete')");
  speech.bind(1, uuid(332));
  speech.bind(2, uuid(331));
  speech.bind(3, std::string(64, 'a'));
  speech.execute();
  const auto insert_usage = [&](const std::size_t value,
                                const std::string_view state) {
    auto usage = fixture.context->connection().prepare(
        "INSERT INTO tts_usage_attempt(attempt_id,speech_id,attempt_number,"
        "provider,model,voice_id,scalar_count,estimated_micro_usd,state,"
        "submitted_at_ms,completed_at_ms) VALUES(?,?,1,'openai','tts-1',"
        "'onyx',1,15,?,1,CASE WHEN ?='submitted' THEN NULL ELSE 1 END)");
    usage.bind(1, uuid(value));
    usage.bind(2, uuid(value + 20));
    usage.bind(3, state);
    usage.bind(4, state);
    usage.execute();
  };
  insert_usage(333, "succeeded");
  insert_usage(334, "submitted");

  constexpr std::int64_t now = 500LL * 24 * 60 * 60 * 1'000;
  sanguinius::persistence::SqliteRetentionRepository retention{fixture.context};
  const auto counts = retention.run(now, uuid(335));
  REQUIRE(counts.speech_items == 1);
  REQUIRE(counts.provider_usage == 1);
  auto remaining = fixture.context->connection().prepare(
      "SELECT (SELECT COUNT(*) FROM speech_item),"
      "(SELECT COUNT(*) FROM tts_usage_attempt WHERE state='succeeded'),"
      "(SELECT COUNT(*) FROM tts_usage_attempt WHERE state='submitted')");
  REQUIRE(remaining.step());
  REQUIRE(remaining.column_int64(0) == 0);
  REQUIRE(remaining.column_int64(1) == 0);
  REQUIRE(remaining.column_int64(2) == 1);
}

TEST_CASE("retention preserves unresolved Discord outcome diagnostics",
          "[cross-feature][retention][outbox][unknown-outcome]") {
  M18Fixture fixture;
  auto insert = fixture.context->connection().prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,target_user_id,payload_json,state,"
      "attempt_count,max_attempts,idempotency_key,provider_nonce,created_at_ms,"
      "available_at_ms,terminal_at_ms,updated_at_ms,last_error_code) VALUES("
      "?,'discord.public.v1','test','unknown','10','20',NULL,'{}','failed',1,"
      "5,'outbox:retention:unknown','0000000000004000000000500',1,1,1,1,"
      "'discord_unknown_outcome_stale')");
  insert.bind(1, uuid(305));
  insert.execute();
  auto definitive = fixture.context->connection().prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,target_user_id,payload_json,state,"
      "attempt_count,max_attempts,idempotency_key,provider_nonce,created_at_ms,"
      "available_at_ms,terminal_at_ms,updated_at_ms,last_error_code) VALUES("
      "?,'discord.public.v1','test','rejected','10','20',NULL,'{}','failed',1,"
      "5,'outbox:retention:rejected','0000000000004000000000501',1,1,1,1,"
      "'discord_rejected')");
  definitive.bind(1, uuid(307));
  definitive.execute();

  sanguinius::persistence::SqliteRetentionRepository retention{fixture.context};
  const auto counts = retention.run(31LL * 24 * 60 * 60 * 1'000, uuid(306));
  REQUIRE(counts.diagnostics == 1);
  auto retained = fixture.context->connection().prepare(
      "SELECT state,last_error_code FROM outbox_message WHERE outbox_id=?");
  retained.bind(1, uuid(305));
  REQUIRE(retained.step());
  REQUIRE(retained.column_text(0) == "failed");
  REQUIRE(retained.column_text(1) == "discord_unknown_outcome_stale");
  auto redacted = fixture.context->connection().prepare(
      "SELECT last_error_code FROM outbox_message WHERE outbox_id=?");
  redacted.bind(1, uuid(307));
  REQUIRE(redacted.step());
  REQUIRE(redacted.column_is_null(0));
}
