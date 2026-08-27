#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_vox_repository.hpp"
#include "sanguinius/vox.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace {

using namespace std::chrono_literals;
using sanguinius::VoxCommandContext;
using sanguinius::VoxFixtureState;
using sanguinius::VoxResultCode;
using sanguinius::VoxState;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "40000000-0000-4000-8000-" + suffix;
}

struct VoxFixture {
  VoxFixture()
      : temporary{}, clock{},
        database{sanguinius::persistence::Database::open_migration(
            temporary.path(), 25ms)} {
    sanguinius::persistence::Migrator migrator{
        sanguinius::persistence::production_migrations(),
        {"test-version", "test-revision"},
        clock};
    REQUIRE(migrator.apply(database.connection()).current_version == 16);
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 10);
    identities.ensure_user({30, "Owner", "owner", false, 10});
    identities.ensure_user({31, "Summoner", "summoner", false, 10});
    identities.ensure_user({32, "Other", "other", false, 10});
    sanguinius::persistence::SqliteApplicationInstanceRepository instances{
        context};
    instances.record_start({instance_id, "test", "revision", "host", 1, 10});
    repository =
        std::make_unique<sanguinius::persistence::SqliteVoxRepository>(context);
    work =
        std::make_unique<sanguinius::persistence::SqliteDurableWorkRepository>(
            context);
  }

  [[nodiscard]] VoxCommandContext command(const std::uint64_t actor,
                                          std::string key,
                                          const std::int64_t now = 100) const {
    return {.guild_id = 10,
            .text_channel_id = 20,
            .actor_user_id = actor,
            .owner_user_id = 30,
            .interaction_idempotency_key = std::move(key),
            .correlation_id = "vox-test",
            .now_ms = now};
  }

  [[nodiscard]] sanguinius::VoxCommandResult
  begin_summon(const std::size_t base = 1, const std::uint64_t actor = 31,
               const std::int64_t now = 100) {
    return repository->start(
        {.context = command(actor, "summon:" + std::to_string(base), now),
         .voice_channel_id = 40,
         .session_id = uuid(base),
         .event_id = uuid(base + 100),
         .timeout_job_id = uuid(base + 200),
         .deployment_instance_id = instance_id});
  }

  [[nodiscard]] sanguinius::VoxCommandResult
  summon(const std::size_t base = 1, const std::uint64_t actor = 31,
         const std::int64_t now = 100) {
    const auto command_context =
        command(actor, "summon:" + std::to_string(base), now);
    auto started = begin_summon(base, actor, now);
    if (started.code != VoxResultCode::accepted || !started.session)
      return started;
    const auto wake_scheduler = started.wake_scheduler;
    auto finalized = repository->finalize_summon(
        command_context, started.session->session_id, started.session->revision,
        true, uuid(base + 300));
    finalized.wake_scheduler = finalized.wake_scheduler || wake_scheduler;
    return finalized;
  }

  [[nodiscard]] sanguinius::VoxCommandResult
  ready(const sanguinius::VoxSession &session, const std::size_t base = 400,
        const std::int64_t now = 200) {
    return repository->transition(
        {.session_id = session.session_id,
         .expected_revision = session.revision,
         .target = VoxState::ready,
         .reason = "voice_ready",
         .actor_user_id = std::nullopt,
         .event_id = uuid(base),
         .idempotency_key = "ready:" + session.session_id,
         .correlation_id = "vox-ready",
         .now_ms = now,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = std::nullopt,
         .public_card = true},
        uuid(base + 1));
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  sanguinius::persistence::Database database;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteVoxRepository> repository;
  std::unique_ptr<sanguinius::persistence::SqliteDurableWorkRepository> work;
  std::string instance_id{uuid(900)};
};

[[nodiscard]] std::int64_t
count(sanguinius::persistence::SqliteRepositoryContext &context,
      const std::string_view table) {
  auto query = context.connection().prepare("SELECT count(*) FROM " +
                                            std::string{table});
  REQUIRE(query.step());
  return query.column_int64(0);
}

} // namespace

TEST_CASE("Vox summon validation rejections persist their exact replay",
          "[vox][persistence][receipt]") {
  VoxFixture fixture;
  const auto context = fixture.command(31, "summon:no-voice", 50);
  const auto rejected = fixture.repository->record_summon_rejection(
      context, VoxResultCode::no_voice,
      "Join a voice channel before summoning Vox Sanguinius.");
  REQUIRE(rejected.code == VoxResultCode::no_voice);
  REQUIRE_FALSE(rejected.session);
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 1);
  REQUIRE(count(*fixture.context, "voice_session") == 0);
  REQUIRE(count(*fixture.context, "voice_session_transition") == 0);
  REQUIRE(count(*fixture.context, "scheduled_job") == 0);

  const auto replay = fixture.repository->preflight_summon(context);
  REQUIRE(replay.code == VoxResultCode::replay);
  REQUIRE(replay.message == rejected.message);
  REQUIRE_FALSE(replay.session);
}

TEST_CASE("Vox repository atomically starts one session and replays receipts",
          "[vox][persistence]") {
  VoxFixture fixture;
  const auto started = fixture.summon();
  REQUIRE(started.code == VoxResultCode::accepted);
  REQUIRE(started.session->state == VoxState::connecting);
  REQUIRE(started.session->revision == 1);
  REQUIRE(started.wake_scheduler);
  REQUIRE(count(*fixture.context, "voice_session") == 1);
  REQUIRE(count(*fixture.context, "voice_session_transition") == 1);
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 1);
  REQUIRE(count(*fixture.context, "event_journal") == 1);
  REQUIRE(count(*fixture.context, "scheduled_job") == 1);
  auto narration_floor = fixture.context->connection().prepare(
      "SELECT narration_event_rowid_floor,(SELECT max(rowid) FROM "
      "event_journal) FROM voice_session WHERE session_id=?");
  narration_floor.bind(1, started.session->session_id);
  REQUIRE(narration_floor.step());
  REQUIRE(narration_floor.column_int64(0) == narration_floor.column_int64(1));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_session SET narration_event_rowid_floor=0 WHERE "
      "session_id='" +
      started.session->session_id + "'"));

  const auto stale =
      fixture.repository->transition({.session_id = started.session->session_id,
                                      .expected_revision = 0,
                                      .target = VoxState::ready,
                                      .reason = "voice_ready",
                                      .actor_user_id = std::nullopt,
                                      .event_id = uuid(350),
                                      .idempotency_key = "ready:stale",
                                      .correlation_id = "vox-stale",
                                      .now_ms = 101,
                                      .timeout_job_id = std::nullopt,
                                      .timeout_due_at_ms = std::nullopt,
                                      .failure_category = std::nullopt,
                                      .public_card = false},
                                     std::nullopt);
  REQUIRE(stale.code == VoxResultCode::invalid_state);
  REQUIRE(count(*fixture.context, "voice_session_transition") == 1);

  const auto duplicate = fixture.summon();
  REQUIRE(duplicate.code == VoxResultCode::replay);
  REQUIRE(duplicate.session->session_id == started.session->session_id);
  REQUIRE(count(*fixture.context, "voice_session") == 1);

  const auto competing = fixture.summon(2, 30, 101);
  REQUIRE(competing.code == VoxResultCode::active_session);
  REQUIRE(competing.session->session_id == started.session->session_id);
  REQUIRE(count(*fixture.context, "voice_session") == 1);
  auto mismatched_receipt = fixture.command(32, "summon:1", 102);
  REQUIRE_THROWS(fixture.repository->preflight_summon(mismatched_receipt));

  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO voice_session SELECT '" + uuid(3) +
      "',guild_id,text_channel_id,voice_channel_id,summoner_user_id,"
      "deployment_instance_id,state,state_version,connection_generation,"
      "reconnect_count,fixture_state,fixture_marker,fixture_queued_at_ms,"
      "fixture_played_at_ms,empty_since_ms,timeout_job_id,started_at_ms,"
      "last_active_at_ms,ended_at_ms,end_reason,last_failure_category FROM "
      "voice_session"));

  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_session_transition SET reason='tampered'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM voice_session_transition"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_interaction_receipt SET result_json='{}'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM voice_interaction_receipt"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_session SET guild_id='11',state_version=state_version+1"));
  REQUIRE_THROWS(
      fixture.context->connection().execute("DELETE FROM voice_session"));
}

TEST_CASE("Vox summon receipt records the final gateway dispatch outcome",
          "[vox][persistence][receipt][gateway]") {
  VoxFixture fixture;
  const auto command_context = fixture.command(31, "summon:dispatch", 100);
  const auto started = fixture.repository->start(
      {.context = command_context,
       .voice_channel_id = 40,
       .session_id = uuid(25),
       .event_id = uuid(125),
       .timeout_job_id = uuid(225),
       .deployment_instance_id = fixture.instance_id});
  REQUIRE(started.code == VoxResultCode::accepted);
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 0);

  const auto rejected = fixture.repository->finalize_summon(
      command_context, started.session->session_id, started.session->revision,
      false, uuid(325));
  REQUIRE(rejected.code == VoxResultCode::unavailable);
  REQUIRE(rejected.message == "Vox could not begin a new session.");
  REQUIRE(rejected.session->state == VoxState::failed);
  REQUIRE(rejected.session->last_failure_category == "gateway_unavailable");
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 1);

  const auto replay = fixture.repository->preflight_summon(command_context);
  REQUIRE(replay.code == VoxResultCode::replay);
  REQUIRE(replay.message == rejected.message);
  REQUIRE(replay.session->state == VoxState::failed);
}

TEST_CASE("Vox restart recovery seals the committed summon crash window",
          "[vox][persistence][receipt][recovery]") {
  VoxFixture fixture;
  const auto started = fixture.begin_summon(26, 31, 100);
  REQUIRE(started.code == VoxResultCode::accepted);
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 0);

  REQUIRE(fixture.repository->recover(uuid(901), 300, uuid(326), uuid(327)) ==
          1);
  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 1);
  const auto original_context = fixture.command(31, "summon:26", 301);
  const auto replay = fixture.repository->preflight_summon(original_context);
  REQUIRE(replay.code == VoxResultCode::replay);
  REQUIRE(replay.session->state == VoxState::inactive);
  REQUIRE(replay.message ==
          "The prior Vox summon was interrupted by a restart.");

  const auto repeated_start =
      fixture.repository->start({.context = original_context,
                                 .voice_channel_id = 40,
                                 .session_id = uuid(27),
                                 .event_id = uuid(127),
                                 .timeout_job_id = uuid(227),
                                 .deployment_instance_id = uuid(901)});
  REQUIRE(repeated_start.code == VoxResultCode::replay);
  REQUIRE(repeated_start.session->session_id == started.session->session_id);
  REQUIRE(count(*fixture.context, "voice_session") == 1);
  REQUIRE(count(*fixture.context, "voice_session_transition") == 2);
}

TEST_CASE("Vox interrupted proof can fail after a disconnect transition",
          "[vox][persistence][fixture][disconnect]") {
  SECTION("reconnecting") {
    VoxFixture fixture;
    const auto ready = fixture.ready(*fixture.summon().session);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-reconnecting",
         .event_id = uuid(530),
         .idempotency_key = "fixture:queued:reconnecting",
         .correlation_id = "fixture-reconnecting",
         .now_ms = 201,
         .failure_category = std::nullopt});
    const auto reconnecting = fixture.repository->command_test_disconnect(
        fixture.command(30, "test-disconnect:fixture", 202), uuid(531),
        uuid(532));
    REQUIRE(reconnecting.session->state == VoxState::reconnecting);
    const auto failed = fixture.repository->fixture(
        {.session_id = reconnecting.session->session_id,
         .expected_revision = reconnecting.session->revision,
         .target = VoxFixtureState::failed,
         .marker = *queued.session->fixture_marker,
         .event_id = uuid(533),
         .idempotency_key = "fixture:failed:reconnecting",
         .correlation_id = "fixture-reconnecting",
         .now_ms = 203,
         .failure_category = "playback_interrupted"});
    REQUIRE(failed.code == VoxResultCode::accepted);
    REQUIRE(failed.session->fixture_state == VoxFixtureState::failed);
  }

  SECTION("leaving") {
    VoxFixture fixture;
    const auto ready = fixture.ready(*fixture.summon().session);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-leaving",
         .event_id = uuid(540),
         .idempotency_key = "fixture:queued:leaving",
         .correlation_id = "fixture-leaving",
         .now_ms = 201,
         .failure_category = std::nullopt});
    const auto leaving = fixture.repository->command_leave(
        fixture.command(31, "leave:fixture", 202), uuid(541), uuid(542));
    REQUIRE(leaving.session->state == VoxState::leaving);
    const auto failed = fixture.repository->fixture(
        {.session_id = leaving.session->session_id,
         .expected_revision = leaving.session->revision,
         .target = VoxFixtureState::failed,
         .marker = *queued.session->fixture_marker,
         .event_id = uuid(543),
         .idempotency_key = "fixture:failed:leaving",
         .correlation_id = "fixture-leaving",
         .now_ms = 203,
         .failure_category = "playback_interrupted"});
    REQUIRE(failed.code == VoxResultCode::accepted);
    REQUIRE(failed.session->fixture_state == VoxFixtureState::failed);
  }
}

TEST_CASE("Vox ready fixture occupancy and leave transitions are durable",
          "[vox][persistence]") {
  VoxFixture fixture;
  const auto started = fixture.summon();
  const auto ready = fixture.ready(*started.session);
  REQUIRE(ready.code == VoxResultCode::accepted);
  REQUIRE(ready.session->state == VoxState::ready);
  REQUIRE(ready.wake_outbox);

  const auto status_context = fixture.command(31, "status:ready", 200);
  const auto status = fixture.repository->command_status(status_context);
  REQUIRE(status.code == VoxResultCode::accepted);
  const auto status_message = status.message;

  const auto queued =
      fixture.repository->fixture({.session_id = ready.session->session_id,
                                   .expected_revision = ready.session->revision,
                                   .target = VoxFixtureState::queued,
                                   .marker = "proof-marker",
                                   .event_id = uuid(500),
                                   .idempotency_key = "fixture:queued",
                                   .correlation_id = "fixture",
                                   .now_ms = 201,
                                   .failure_category = std::nullopt});
  REQUIRE(queued.code == VoxResultCode::accepted);
  REQUIRE(queued.session->fixture_state == VoxFixtureState::queued);
  const auto played = fixture.repository->fixture(
      {.session_id = queued.session->session_id,
       .expected_revision = queued.session->revision,
       .target = VoxFixtureState::played,
       .marker = "proof-marker",
       .event_id = uuid(501),
       .idempotency_key = "fixture:played",
       .correlation_id = "fixture",
       .now_ms = 202,
       .failure_category = std::nullopt});
  REQUIRE(played.session->fixture_state == VoxFixtureState::played);
  const auto queued_replay =
      fixture.repository->fixture({.session_id = ready.session->session_id,
                                   .expected_revision = ready.session->revision,
                                   .target = VoxFixtureState::queued,
                                   .marker = "proof-marker",
                                   .event_id = uuid(500),
                                   .idempotency_key = "fixture:queued",
                                   .correlation_id = "fixture",
                                   .now_ms = 201,
                                   .failure_category = std::nullopt});
  REQUIRE(queued_replay.code == VoxResultCode::replay);
  const auto status_replay = fixture.repository->command_status(status_context);
  REQUIRE(status_replay.code == VoxResultCode::replay);
  REQUIRE(status_replay.message == status_message);

  const auto empty = fixture.repository->occupancy(
      {.session_id = played.session->session_id,
       .expected_revision = played.session->revision,
       .human_count = 0,
       .now_ms = 203,
       .empty_job_id = uuid(600),
       .event_id = uuid(601),
       .idempotency_key = "occupancy:empty",
       .correlation_id = "occupancy"});
  REQUIRE(empty.code == VoxResultCode::accepted);
  REQUIRE(empty.session->empty_since_ms == 203);
  REQUIRE(empty.wake_scheduler);

  const auto occupied = fixture.repository->occupancy(
      {.session_id = empty.session->session_id,
       .expected_revision = empty.session->revision,
       .human_count = 1,
       .now_ms = 204,
       .empty_job_id = std::nullopt,
       .event_id = uuid(602),
       .idempotency_key = "occupancy:joined",
       .correlation_id = "occupancy"});
  REQUIRE(occupied.code == VoxResultCode::accepted);
  REQUIRE_FALSE(occupied.session->empty_since_ms);

  const auto unauthorized = fixture.repository->command_leave(
      fixture.command(32, "leave:other", 205), uuid(700), uuid(701));
  REQUIRE(unauthorized.code == VoxResultCode::unauthorized);
  const auto leaving = fixture.repository->command_leave(
      fixture.command(31, "leave:summoner", 206), uuid(702), uuid(703));
  REQUIRE(leaving.code == VoxResultCode::accepted);
  REQUIRE(leaving.session->state == VoxState::leaving);
  const auto inactive = fixture.repository->transition(
      {.session_id = leaving.session->session_id,
       .expected_revision = leaving.session->revision,
       .target = VoxState::inactive,
       .reason = "commanded_leave_complete",
       .actor_user_id = 31,
       .event_id = uuid(704),
       .idempotency_key = "leave:inactive",
       .correlation_id = "leave",
       .now_ms = 207,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = std::nullopt,
       .public_card = true},
      uuid(705));
  REQUIRE(inactive.code == VoxResultCode::accepted);
  REQUIRE(inactive.session->state == VoxState::inactive);
  REQUIRE(inactive.session->ended_at_ms == 207);
  REQUIRE(count(*fixture.context, "outbox_message") == 2);
  const auto terminal_transition = fixture.repository->transition(
      {.session_id = inactive.session->session_id,
       .expected_revision = inactive.session->revision,
       .target = VoxState::ready,
       .reason = "voice_ready",
       .actor_user_id = std::nullopt,
       .event_id = uuid(706),
       .idempotency_key = "terminal:ready",
       .correlation_id = "terminal",
       .now_ms = 208,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = std::nullopt,
       .public_card = false},
      std::nullopt);
  REQUIRE(terminal_transition.code == VoxResultCode::invalid_state);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_session SET state_version=state_version+1 WHERE "
      "session_id='" +
      inactive.session->session_id + "'"));

  const auto second = fixture.summon(20, 31, 209);
  REQUIRE(second.code == VoxResultCode::accepted);
  const auto owner_leave = fixture.repository->command_leave(
      fixture.command(30, "leave:owner", 210), uuid(720), uuid(721));
  REQUIRE(owner_leave.code == VoxResultCode::accepted);
  REQUIRE(owner_leave.session->state == VoxState::leaving);
}

TEST_CASE("Vox timed mute is authorized durable and revision fenced",
          "[vox][persistence][mute]") {
  VoxFixture fixture;
  const auto ready = fixture.ready(*fixture.summon().session);
  REQUIRE(ready.session->state == VoxState::ready);

  const auto unauthorized = fixture.repository->command_mute(
      fixture.command(32, "mute:unauthorized", 250), false, std::nullopt,
      uuid(610), std::nullopt);
  REQUIRE(unauthorized.code == VoxResultCode::unauthorized);
  REQUIRE(unauthorized.session->state == VoxState::ready);

  constexpr std::int64_t expiry = 3'600'300;
  const auto context = fixture.command(31, "mute:timed", 300);
  const auto muted = fixture.repository->command_mute(context, false, expiry,
                                                      uuid(611), uuid(612));
  REQUIRE(muted.code == VoxResultCode::accepted);
  REQUIRE(muted.session->state == VoxState::muted);
  REQUIRE(muted.session->muted_at_ms == 300);
  REQUIRE(muted.session->mute_until_ms == expiry);
  REQUIRE(muted.session->mute_job_id == uuid(612));
  REQUIRE(muted.wake_scheduler);

  const auto replay = fixture.repository->command_mute(context, false, expiry,
                                                       uuid(613), uuid(614));
  REQUIRE(replay.code == VoxResultCode::replay);
  REQUIRE(replay.session->mute_job_id == uuid(612));

  const auto reconnecting = fixture.repository->transition(
      {.session_id = muted.session->session_id,
       .expected_revision = muted.session->revision,
       .target = VoxState::reconnecting,
       .reason = "unexpected_disconnect",
       .actor_user_id = std::nullopt,
       .event_id = uuid(620),
       .idempotency_key = "mute:reconnecting",
       .correlation_id = "mute-reconnect",
       .now_ms = 400,
       .timeout_job_id = uuid(621),
       .timeout_due_at_ms = 10'400,
       .failure_category = std::nullopt,
       .public_card = false},
      std::nullopt);
  REQUIRE(reconnecting.code == VoxResultCode::accepted);
  REQUIRE(reconnecting.session->state == VoxState::reconnecting);
  REQUIRE(reconnecting.session->muted_at_ms == 300);
  REQUIRE(reconnecting.session->mute_until_ms == expiry);
  REQUIRE(reconnecting.session->mute_job_id == uuid(612));
  const auto reconnected = fixture.repository->transition(
      {.session_id = muted.session->session_id,
       .expected_revision = reconnecting.session->revision,
       .target = VoxState::muted,
       .reason = "voice_reconnected",
       .actor_user_id = std::nullopt,
       .event_id = uuid(622),
       .idempotency_key = "mute:reconnected",
       .correlation_id = "mute-reconnect",
       .now_ms = 500,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = std::nullopt,
       .public_card = false},
      std::nullopt);
  REQUIRE(reconnected.code == VoxResultCode::accepted);
  REQUIRE(reconnected.session->state == VoxState::muted);
  REQUIRE(reconnected.session->muted_at_ms == 300);
  REQUIRE(reconnected.session->mute_job_id == uuid(612));

  const auto claimed =
      fixture.work->claim_due_job(expiry, expiry + 10'000, "worker", uuid(615));
  REQUIRE(claimed.has_value());
  REQUIRE(claimed->job_type == sanguinius::vox_mute_expiry_job_type);
  const auto expired = fixture.repository->handle_timeout(
      *claimed, expiry, uuid(616), uuid(617), uuid(618), std::nullopt);
  REQUIRE(expired.code == VoxResultCode::accepted);
  REQUIRE(expired.session->state == VoxState::ready);
  REQUIRE_FALSE(expired.session->muted_at_ms);
  REQUIRE_FALSE(expired.session->mute_until_ms);
  REQUIRE_FALSE(expired.session->mute_job_id);
}

TEST_CASE("Vox auxiliary command receipts audit fingerprints and replay",
          "[vox][persistence][receipt][tts]") {
  VoxFixture fixture;
  const auto ready = fixture.ready(*fixture.summon().session);
  REQUIRE(ready.session->state == VoxState::ready);

  const std::array commands{
      std::pair{
          "say",
          "sha256:"
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
      std::pair{"voice", "select:onyx"},
      std::pair{"speech_test", "scenario:provider-failure"}};
  std::int64_t now = 700;
  for (const auto &[operation, fingerprint] : commands) {
    const auto context =
        fixture.command(30, "receipt:" + std::string{operation}, now++);
    REQUIRE_FALSE(
        fixture.repository->command_receipt(context, operation, fingerprint));
    auto result =
        sanguinius::VoxCommandResult{.code = VoxResultCode::accepted,
                                     .session = ready.session,
                                     .message = "Audited command accepted."};
    const auto recorded = fixture.repository->record_command_receipt(
        context, operation, fingerprint, result);
    REQUIRE(recorded.code == VoxResultCode::accepted);

    sanguinius::persistence::SqliteVoxRepository restarted{fixture.context};
    const auto replay =
        restarted.command_receipt(context, operation, fingerprint);
    REQUIRE(replay);
    REQUIRE(replay->code == VoxResultCode::replay);
    REQUIRE(replay->message == "Audited command accepted.");
    REQUIRE_THROWS(
        restarted.command_receipt(context, operation, "altered:fingerprint"));
  }

  REQUIRE(count(*fixture.context, "voice_interaction_receipt") == 4);
  auto stored = fixture.context->connection().prepare(
      "SELECT operation,json_extract(request_json,'$.request_fingerprint') "
      "FROM voice_interaction_receipt WHERE operation IN "
      "('say','voice','speech_test') ORDER BY operation");
  std::size_t rows{};
  while (stored.step()) {
    REQUIRE_FALSE(stored.column_text(1).empty());
    ++rows;
  }
  REQUIRE(rows == commands.size());
  REQUIRE_THROWS(fixture.repository->command_receipt(
      fixture.command(30, "invalid:receipt", now), "status", "inspect"));
}

TEST_CASE("Vox mute preserves empty cleanup and reconnect fencing",
          "[vox][persistence][mute][timeout][reconnect]") {
  SECTION("empty cleanup survives mute and unmute") {
    VoxFixture fixture;
    const auto ready = fixture.ready(*fixture.summon().session);
    const auto empty = fixture.repository->occupancy(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .human_count = 0,
         .now_ms = 300,
         .empty_job_id = uuid(630),
         .event_id = uuid(631),
         .idempotency_key = "mute:empty",
         .correlation_id = "mute-empty"});
    REQUIRE(empty.code == VoxResultCode::accepted);
    const auto muted = fixture.repository->command_mute(
        fixture.command(31, "mute:empty:on", 400), false, std::nullopt,
        uuid(632), std::nullopt);
    REQUIRE(muted.code == VoxResultCode::accepted);
    REQUIRE(muted.session->state == VoxState::muted);
    REQUIRE(muted.session->empty_since_ms == 300);
    REQUIRE(muted.session->timeout_job_id == uuid(630));
    const auto unmuted = fixture.repository->command_mute(
        fixture.command(31, "mute:empty:off", 500), true, std::nullopt,
        uuid(633), std::nullopt);
    REQUIRE(unmuted.code == VoxResultCode::accepted);
    REQUIRE(unmuted.session->state == VoxState::ready);
    REQUIRE(unmuted.session->empty_since_ms == 300);
    REQUIRE(unmuted.session->timeout_job_id == uuid(630));
    const auto claimed =
        fixture.work->claim_due_job(60'300, 30'000, "worker", uuid(634));
    REQUIRE(claimed);
    REQUIRE(claimed->job_type == sanguinius::vox_empty_timeout_job_type);
    const auto closed = fixture.repository->handle_timeout(
        *claimed, 60'300, uuid(635), uuid(636), uuid(637), 0);
    REQUIRE(closed.code == VoxResultCode::accepted);
    REQUIRE(closed.session->state == VoxState::inactive);
  }

  SECTION("mute expiry cannot complete an unfinished reconnect") {
    VoxFixture fixture;
    const auto ready = fixture.ready(*fixture.summon().session);
    const auto muted = fixture.repository->command_mute(
        fixture.command(31, "mute:reconnect:on", 300), false, 500, uuid(640),
        uuid(641));
    REQUIRE(muted.code == VoxResultCode::accepted);
    const auto reconnecting = fixture.repository->transition(
        {.session_id = ready.session->session_id,
         .expected_revision = muted.session->revision,
         .target = VoxState::reconnecting,
         .reason = "unexpected_disconnect",
         .actor_user_id = std::nullopt,
         .event_id = uuid(642),
         .idempotency_key = "mute:expiry:reconnecting",
         .correlation_id = "mute-expiry-reconnect",
         .now_ms = 400,
         .timeout_job_id = uuid(643),
         .timeout_due_at_ms = 10'400,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    REQUIRE(reconnecting.code == VoxResultCode::accepted);
    const auto claimed =
        fixture.work->claim_due_job(500, 30'000, "worker", uuid(644));
    REQUIRE(claimed);
    REQUIRE(claimed->job_type == sanguinius::vox_mute_expiry_job_type);
    const auto expired = fixture.repository->handle_timeout(
        *claimed, 500, uuid(645), uuid(646), uuid(647), std::nullopt);
    REQUIRE(expired.code == VoxResultCode::accepted);
    REQUIRE(expired.session->state == VoxState::reconnecting);
    REQUIRE(expired.session->timeout_job_id == uuid(643));
    REQUIRE_FALSE(expired.session->muted_at_ms);
    REQUIRE_FALSE(expired.session->mute_job_id);
    const auto reconnected = fixture.repository->transition(
        {.session_id = expired.session->session_id,
         .expected_revision = expired.session->revision,
         .target = VoxState::ready,
         .reason = "voice_reconnected",
         .actor_user_id = std::nullopt,
         .event_id = uuid(648),
         .idempotency_key = "mute:expiry:reconnected",
         .correlation_id = "mute-expiry-reconnect",
         .now_ms = 600,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    REQUIRE(reconnected.code == VoxResultCode::accepted);
    REQUIRE(reconnected.session->state == VoxState::ready);
  }
}

TEST_CASE("Vox rollback-safe cleanup preserves ready-before-terminal delivery",
          "[vox][persistence][clock][outbox]") {
  VoxFixture fixture;
  const auto started = fixture.summon(1, 31, 100);
  const auto ready = fixture.ready(*started.session, 400, 50);
  REQUIRE(ready.code == VoxResultCode::accepted);
  const auto leaving = fixture.repository->command_leave(
      fixture.command(31, "leave:rollback", 40), uuid(730), uuid(731));
  REQUIRE(leaving.code == VoxResultCode::accepted);
  const auto inactive = fixture.repository->transition(
      {.session_id = leaving.session->session_id,
       .expected_revision = leaving.session->revision,
       .target = VoxState::inactive,
       .reason = "commanded_leave_complete",
       .actor_user_id = 31,
       .event_id = uuid(732),
       .idempotency_key = "leave:rollback:complete",
       .correlation_id = "leave-rollback",
       .now_ms = 30,
       .timeout_job_id = std::nullopt,
       .timeout_due_at_ms = std::nullopt,
       .failure_category = std::nullopt,
       .public_card = true},
      uuid(733));
  REQUIRE(inactive.code == VoxResultCode::accepted);
  REQUIRE(inactive.session->ended_at_ms == 100);

  auto invalid_job_times = fixture.context->connection().prepare(
      "SELECT count(*) FROM scheduled_job WHERE updated_at_ms<created_at_ms "
      "OR (terminal_at_ms IS NOT NULL AND terminal_at_ms<created_at_ms)");
  REQUIRE(invalid_job_times.step());
  REQUIRE(invalid_job_times.column_int64(0) == 0);
  REQUIRE(count(*fixture.context, "voice_public_outbox_dependency") == 1);

  const auto ready_card =
      fixture.work->claim_due_outbox(200, 1'000, "ordering", uuid(734), true);
  REQUIRE(ready_card);
  REQUIRE_FALSE(
      fixture.work->claim_due_outbox(200, 1'000, "ordering", uuid(735), true));
  REQUIRE(fixture.work->mark_public_outbox_submitted(
              *ready_card,
              {.wall_time_ms = 200,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              1'000) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(fixture.work->complete_public_outbox(*ready_card, 90, 200) ==
          sanguinius::WorkMutationStatus::applied);
  const auto terminal_card =
      fixture.work->claim_due_outbox(200, 1'000, "ordering", uuid(736), true);
  REQUIRE(terminal_card);
  REQUIRE(terminal_card->outbox_id != ready_card->outbox_id);
}

TEST_CASE("Vox timeout and restart recovery close sessions without replay",
          "[vox][persistence][recovery]") {
  SECTION("malformed timeout payload is dead-lettered immediately") {
    VoxFixture fixture;
    static_cast<void>(fixture.summon());
    fixture.context->connection().execute(
        "UPDATE scheduled_job SET payload_json='{}'");
    const auto claimed =
        fixture.work->claim_due_job(20'100, 30'000, "worker", uuid(798));
    REQUIRE(claimed);
    REQUIRE(std::holds_alternative<std::monostate>(claimed->payload));

    const auto rejected = fixture.repository->handle_timeout(
        *claimed, 20'100, uuid(799), uuid(800), uuid(866), std::nullopt);
    REQUIRE(rejected.code == VoxResultCode::accepted);
    REQUIRE(rejected.session->state == VoxState::failed);
    REQUIRE(rejected.session->end_reason == "timeout_payload_invalid");
    REQUIRE(rejected.session->last_failure_category == "payload_invalid");
    REQUIRE_FALSE(fixture.repository->active());
    auto job = fixture.context->connection().prepare(
        "SELECT state,last_error_code,lease_owner,lease_token FROM "
        "scheduled_job WHERE job_id=?");
    job.bind(1, claimed->job_id);
    REQUIRE(job.step());
    REQUIRE(job.column_text(0) == "dead");
    REQUIRE(job.column_text(1) == "payload_invalid");
    REQUIRE(job.column_is_null(2));
    REQUIRE(job.column_is_null(3));
  }

  SECTION("connect timeout") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto early =
        fixture.work->claim_due_job(20'099, 30'000, "worker", uuid(799));
    REQUIRE_FALSE(early);
    const auto claimed =
        fixture.work->claim_due_job(20'100, 30'000, "worker", uuid(800));
    REQUIRE(claimed);
    const auto timed_out = fixture.repository->handle_timeout(
        *claimed, 20'100, uuid(801), uuid(802), uuid(803), std::nullopt);
    REQUIRE(timed_out.code == VoxResultCode::accepted);
    REQUIRE(timed_out.session->state == VoxState::failed);
    REQUIRE(timed_out.session->last_failure_category == "connect_timeout");
    REQUIRE_FALSE(timed_out.wake_outbox);
    auto job = fixture.context->connection().prepare(
        "SELECT state,completed_at_ms FROM scheduled_job WHERE job_id=?");
    job.bind(1, claimed->job_id);
    REQUIRE(job.step());
    REQUIRE(job.column_text(0) == "completed");
    REQUIRE(job.column_int64(1) == 20'100);
  }

  SECTION("reconnect timeout and one-rejoin limit") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto reconnecting = fixture.repository->transition(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxState::reconnecting,
         .reason = "unexpected_disconnect",
         .actor_user_id = std::nullopt,
         .event_id = uuid(820),
         .idempotency_key = "reconnect:begin",
         .correlation_id = "reconnect",
         .now_ms = 300,
         .timeout_job_id = uuid(821),
         .timeout_due_at_ms = 20'300,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    REQUIRE(reconnecting.code == VoxResultCode::accepted);
    REQUIRE(reconnecting.session->connection_generation == 2);
    REQUIRE(reconnecting.session->reconnect_count == 1);
    REQUIRE_FALSE(
        fixture.work->claim_due_job(20'299, 30'000, "worker", uuid(822)));
    const auto claimed =
        fixture.work->claim_due_job(20'300, 30'000, "worker", uuid(823));
    REQUIRE(claimed);
    const auto timed_out = fixture.repository->handle_timeout(
        *claimed, 20'300, uuid(824), uuid(825), uuid(826), std::nullopt);
    REQUIRE(timed_out.code == VoxResultCode::accepted);
    REQUIRE(timed_out.session->state == VoxState::failed);
    REQUIRE(timed_out.session->last_failure_category == "reconnect_timeout");
    REQUIRE(timed_out.wake_outbox);
  }

  SECTION("a completed reconnect cannot begin a second application rejoin") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto reconnecting = fixture.repository->transition(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxState::reconnecting,
         .reason = "unexpected_disconnect",
         .actor_user_id = std::nullopt,
         .event_id = uuid(830),
         .idempotency_key = "reconnect:once",
         .correlation_id = "reconnect-once",
         .now_ms = 300,
         .timeout_job_id = uuid(831),
         .timeout_due_at_ms = 20'300,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    const auto reconnected = fixture.repository->transition(
        {.session_id = reconnecting.session->session_id,
         .expected_revision = reconnecting.session->revision,
         .target = VoxState::ready,
         .reason = "voice_reconnected",
         .actor_user_id = std::nullopt,
         .event_id = uuid(832),
         .idempotency_key = "reconnect:ready",
         .correlation_id = "reconnect-once",
         .now_ms = 301,
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    REQUIRE(reconnected.code == VoxResultCode::accepted);
    const auto repeated = fixture.repository->transition(
        {.session_id = reconnected.session->session_id,
         .expected_revision = reconnected.session->revision,
         .target = VoxState::reconnecting,
         .reason = "unexpected_disconnect",
         .actor_user_id = std::nullopt,
         .event_id = uuid(833),
         .idempotency_key = "reconnect:twice",
         .correlation_id = "reconnect-once",
         .now_ms = 302,
         .timeout_job_id = uuid(834),
         .timeout_due_at_ms = 20'302,
         .failure_category = std::nullopt,
         .public_card = false},
        std::nullopt);
    REQUIRE(repeated.code == VoxResultCode::invalid_state);
    REQUIRE(repeated.session->state == VoxState::ready);
  }

  SECTION("empty timeout rechecks cached occupancy at the exact boundary") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto empty = fixture.repository->occupancy(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .human_count = 0,
         .now_ms = 300,
         .empty_job_id = uuid(810),
         .event_id = uuid(811),
         .idempotency_key = "empty:boundary",
         .correlation_id = "empty-boundary"});
    REQUIRE(empty.code == VoxResultCode::accepted);
    REQUIRE_FALSE(
        fixture.work->claim_due_job(60'299, 30'000, "worker", uuid(812)));
    const auto claimed =
        fixture.work->claim_due_job(60'300, 30'000, "worker", uuid(813));
    REQUIRE(claimed);
    const auto rechecked = fixture.repository->handle_timeout(
        *claimed, 60'300, uuid(814), uuid(815), uuid(816), 1);
    REQUIRE(rechecked.code == VoxResultCode::replay);
    REQUIRE(rechecked.session->state == VoxState::ready);
    REQUIRE_FALSE(rechecked.session->empty_since_ms);
    REQUIRE_FALSE(rechecked.session->timeout_job_id);
  }

  SECTION("empty timeout closes an unchanged empty session") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-empty-timeout",
         .event_id = uuid(846),
         .idempotency_key = "fixture:queued:empty-timeout",
         .correlation_id = "empty-close",
         .now_ms = 250,
         .failure_category = std::nullopt});
    REQUIRE(queued.code == VoxResultCode::accepted);
    const auto empty = fixture.repository->occupancy(
        {.session_id = queued.session->session_id,
         .expected_revision = queued.session->revision,
         .human_count = 0,
         .now_ms = 300,
         .empty_job_id = uuid(840),
         .event_id = uuid(841),
         .idempotency_key = "empty:close",
         .correlation_id = "empty-close"});
    const auto claimed =
        fixture.work->claim_due_job(60'300, 30'000, "worker", uuid(842));
    REQUIRE(claimed);
    const auto closed = fixture.repository->handle_timeout(
        *claimed, 60'300, uuid(843), uuid(844), uuid(845), 0);
    REQUIRE(closed.code == VoxResultCode::accepted);
    REQUIRE(closed.session->state == VoxState::inactive);
    REQUIRE(closed.session->end_reason == "empty_timeout");
    REQUIRE(closed.session->fixture_state == VoxFixtureState::failed);
    REQUIRE(closed.session->last_failure_category == "playback_interrupted");
    REQUIRE(closed.wake_outbox);
  }

  SECTION("leave timeout bounds a missing gateway departure") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    REQUIRE(fixture.ready(*started.session).code == VoxResultCode::accepted);
    const auto leaving = fixture.repository->command_leave(
        fixture.command(31, "leave:timeout", 300), uuid(860), uuid(861));
    REQUIRE(leaving.code == VoxResultCode::accepted);
    REQUIRE(leaving.session->state == VoxState::leaving);
    REQUIRE(leaving.session->timeout_job_id == uuid(861));
    REQUIRE(leaving.wake_scheduler);
    REQUIRE_FALSE(
        fixture.work->claim_due_job(20'299, 30'000, "worker", uuid(862)));
    const auto claimed =
        fixture.work->claim_due_job(20'300, 30'000, "worker", uuid(863));
    REQUIRE(claimed);
    REQUIRE(claimed->job_type == sanguinius::vox_leave_timeout_job_type);
    const auto closed = fixture.repository->handle_timeout(
        *claimed, 20'300, uuid(864), uuid(865), uuid(866), std::nullopt);
    REQUIRE(closed.code == VoxResultCode::accepted);
    REQUIRE(closed.session->state == VoxState::inactive);
    REQUIRE(closed.session->end_reason == "leave_timeout");
    REQUIRE(closed.wake_outbox);
  }

  SECTION("prior instance") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    REQUIRE(started.session->fixture_state == VoxFixtureState::pending);
    const auto recovered =
        fixture.repository->recover(uuid(901), 300, uuid(803), uuid(804));
    REQUIRE(recovered == 1);
    REQUIRE_FALSE(fixture.repository->active());
    auto query = fixture.context->connection().prepare(
        "SELECT state,end_reason,fixture_state FROM voice_session");
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "inactive");
    REQUIRE(query.column_text(1) == "restart_abandoned");
    REQUIRE(query.column_text(2) == "pending");
  }

  SECTION("restart marks queued proof playback as interrupted") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-restart-interrupted",
         .event_id = uuid(870),
         .idempotency_key = "fixture:queued:restart",
         .correlation_id = "restart-queued-proof",
         .now_ms = 250,
         .failure_category = std::nullopt});
    REQUIRE(queued.code == VoxResultCode::accepted);

    REQUIRE(fixture.repository->recover(uuid(901), 300, uuid(871), uuid(872)) ==
            1);
    auto query = fixture.context->connection().prepare(
        "SELECT state,end_reason,fixture_state,last_failure_category FROM "
        "voice_session");
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "inactive");
    REQUIRE(query.column_text(1) == "restart_abandoned");
    REQUIRE(query.column_text(2) == "failed");
    REQUIRE(query.column_text(3) == "playback_interrupted");
    auto failure_event = fixture.context->connection().prepare(
        "SELECT count(*) FROM event_journal WHERE "
        "event_type='vox.static_proof_failed.v1'");
    REQUIRE(failure_event.step());
    REQUIRE(failure_event.column_int64(0) == 1);
  }

  SECTION("a join preserves cancellation of an already claimed empty timeout") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto empty = fixture.repository->occupancy(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .human_count = 0,
         .now_ms = 300,
         .empty_job_id = uuid(880),
         .event_id = uuid(881),
         .idempotency_key = "empty:claimed-race",
         .correlation_id = "empty-claimed-race"});
    REQUIRE(empty.code == VoxResultCode::accepted);
    const auto claimed =
        fixture.work->claim_due_job(60'300, 30'000, "worker", uuid(882));
    REQUIRE(claimed);

    const auto occupied = fixture.repository->occupancy(
        {.session_id = empty.session->session_id,
         .expected_revision = empty.session->revision,
         .human_count = 1,
         .now_ms = 60'300,
         .empty_job_id = std::nullopt,
         .event_id = uuid(883),
         .idempotency_key = "occupied:claimed-race",
         .correlation_id = "empty-claimed-race"});
    REQUIRE(occupied.code == VoxResultCode::accepted);
    REQUIRE_FALSE(occupied.session->timeout_job_id);

    const auto stale = fixture.repository->handle_timeout(
        *claimed, 60'300, uuid(884), uuid(885), uuid(886), 1);
    REQUIRE(stale.code == VoxResultCode::replay);
    auto job = fixture.context->connection().prepare(
        "SELECT state,completed_at_ms FROM scheduled_job WHERE job_id=?");
    job.bind(1, claimed->job_id);
    REQUIRE(job.step());
    REQUIRE(job.column_text(0) == "cancelled");
    REQUIRE(job.column_is_null(1));
  }

  SECTION("prior leaving session is cleaned up without a public card") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto leaving = fixture.repository->command_leave(
        fixture.command(31, "leave:restart", 200), uuid(850), uuid(851));
    REQUIRE(leaving.session->state == VoxState::leaving);
    REQUIRE(fixture.repository->recover(uuid(901), 300, uuid(852), uuid(853)) ==
            1);
    auto query = fixture.context->connection().prepare(
        "SELECT state,end_reason FROM voice_session");
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "inactive");
    REQUIRE(query.column_text(1) == "restart_cleanup");
    REQUIRE(count(*fixture.context, "outbox_message") == 0);
  }

  SECTION("restart recovery cancels an undelivered ready card") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    REQUIRE(ready.wake_outbox);
    const auto claimed = fixture.work->claim_due_outbox(
        200, 1'000, "prior-instance", uuid(853), true);
    REQUIRE(claimed.has_value());

    REQUIRE(fixture.repository->recover(uuid(901), 300, uuid(854), uuid(855)) ==
            1);
    auto query = fixture.context->connection().prepare(
        "SELECT state,submission_started_at_ms,terminal_at_ms,last_error_code "
        "FROM outbox_message WHERE aggregate_type='voice_session'");
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "cancelled");
    REQUIRE(query.column_is_null(1));
    REQUIRE(query.column_int64(2) == 300);
    REQUIRE(query.column_text(3) == "vox_session_closed");
    REQUIRE_FALSE(query.step());
  }

  SECTION("shutdown quarantines a submitted ready card as unknown outcome") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    REQUIRE(ready.wake_outbox);
    const auto claimed = fixture.work->claim_due_outbox(
        200, 1'000, "current-instance", uuid(856), true);
    REQUIRE(claimed);
    REQUIRE(
        fixture.work->mark_public_outbox_submitted(
            *claimed,
            {.wall_time_ms = 200,
             .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
             .boot_session_id = std::string{fixture.clock.boot_session_id()}},
            1'000) == sanguinius::WorkMutationStatus::applied);

    const auto shutdown =
        fixture.repository->shutdown(300, uuid(857), uuid(858));
    REQUIRE(shutdown.code == VoxResultCode::accepted);
    auto query = fixture.context->connection().prepare(
        "SELECT state,submission_started_at_ms,first_attempt_at_ms,"
        "provider_nonce,last_error_code FROM outbox_message WHERE outbox_id=?");
    query.bind(1, claimed->outbox_id);
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "failed");
    REQUIRE(query.column_is_null(1));
    REQUIRE(query.column_int64(2) == 200);
    REQUIRE(query.column_text(3) == claimed->provider_nonce);
    REQUIRE(query.column_text(4) == "discord_unknown_outcome_vox_closed");
  }

  SECTION("clean shutdown cancels an undelivered ready card") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    REQUIRE(ready.wake_outbox);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-shutdown",
         .event_id = uuid(860),
         .idempotency_key = "fixture:queued:shutdown",
         .correlation_id = "shutdown-queued-proof",
         .now_ms = 250,
         .failure_category = std::nullopt});
    REQUIRE(queued.code == VoxResultCode::accepted);

    const auto shutdown =
        fixture.repository->shutdown(300, uuid(855), uuid(856));
    REQUIRE(shutdown.code == VoxResultCode::accepted);
    REQUIRE(shutdown.session->state == VoxState::inactive);
    REQUIRE(shutdown.session->fixture_state == VoxFixtureState::failed);
    REQUIRE(shutdown.session->last_failure_category == "playback_interrupted");
    auto query = fixture.context->connection().prepare(
        "SELECT state,last_error_code FROM outbox_message WHERE "
        "aggregate_type='voice_session'");
    REQUIRE(query.step());
    REQUIRE(query.column_text(0) == "cancelled");
    REQUIRE(query.column_text(1) == "vox_session_closed");
  }

  SECTION("shutdown preserves a retained queued proof failure cause") {
    VoxFixture fixture;
    const auto started = fixture.summon();
    const auto ready = fixture.ready(*started.session);
    const auto queued = fixture.repository->fixture(
        {.session_id = ready.session->session_id,
         .expected_revision = ready.session->revision,
         .target = VoxFixtureState::queued,
         .marker = "proof-rejected-before-shutdown",
         .event_id = uuid(861),
         .idempotency_key = "fixture:queued:rejected-shutdown",
         .correlation_id = "shutdown-rejected-proof",
         .now_ms = 250,
         .failure_category = std::nullopt});
    REQUIRE(queued.code == VoxResultCode::accepted);

    const auto shutdown = fixture.repository->shutdown(
        300, uuid(862), uuid(863), "audio_rejected");
    REQUIRE(shutdown.code == VoxResultCode::accepted);
    REQUIRE(shutdown.session->state == VoxState::inactive);
    REQUIRE(shutdown.session->fixture_state == VoxFixtureState::failed);
    REQUIRE(shutdown.session->last_failure_category == "audio_rejected");

    auto event = fixture.context->connection().prepare(
        "SELECT payload_json FROM event_journal WHERE event_type="
        "'vox.static_proof_failed.v1'");
    REQUIRE(event.step());
    const auto payload = nlohmann::json::parse(event.column_text(0));
    REQUIRE(payload.at("failure_category") == "audio_rejected");
  }
}
