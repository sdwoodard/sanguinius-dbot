#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_voice_input_repository.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace {

using sanguinius::VoiceListeningState;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "50000000-0000-4000-8000-" + suffix;
}

struct Fixture {
  Fixture()
      : temporary{}, clock{},
        database{sanguinius::persistence::Database::open_migration(
            temporary.path(), std::chrono::milliseconds{25})} {
    sanguinius::persistence::Migrator migrator{
        sanguinius::persistence::production_migrations(),
        {"test-version", "test-revision"},
        clock};
    REQUIRE(migrator.apply(database.connection()).current_version == 15);
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 10);
    identities.ensure_user({31, "Listener", "listener", false, 10});
    identities.ensure_user({32, "Stopper", "stopper", false, 10});
    auto instance = context->connection().prepare(
        "INSERT INTO application_instance(instance_id,application_version,"
        "git_revision,hostname,process_id,started_at_ms) VALUES(?,?,?,?,?,?)");
    instance.bind(1, uuid(900));
    instance.bind(2, "test");
    instance.bind(3, "revision");
    instance.bind(4, "host");
    instance.bind(5, 1);
    instance.bind(6, 10);
    instance.execute();
    auto session = context->connection().prepare(
        "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
        "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
        "state_version,connection_generation,reconnect_count,fixture_state,"
        "fixture_marker,fixture_queued_at_ms,fixture_played_at_ms,started_at_"
        "ms,"
        "last_active_at_ms) "
        "VALUES(?,?,?,?,?,?,'ready',3,1,0,'played',?,?,?,10,20)");
    session.bind(1, session_id);
    session.bind(2, "10");
    session.bind(3, "20");
    session.bind(4, "40");
    session.bind(5, "31");
    session.bind(6, uuid(900));
    session.bind(7, "fixture");
    session.bind(8, 11);
    session.bind(9, 12);
    session.execute();
    repository = std::make_unique<
        sanguinius::persistence::SqliteVoiceListeningRepository>(context);
  }

  [[nodiscard]] sanguinius::VoiceWindowBeginRequest
  request(const std::size_t base, const std::int64_t now,
          const std::size_t seconds = 15) const {
    return {.window = {.window_id = uuid(base),
                       .vox_session_id = session_id,
                       .guild_id = 10,
                       .text_channel_id = 20,
                       .voice_channel_id = 40,
                       .requester_user_id = 31,
                       .state = VoiceListeningState::proposed,
                       .revision = 1,
                       .connection_generation = 1,
                       .requested_seconds = seconds,
                       .initial_human_count = 1,
                       .reserved_micro_usd =
                           static_cast<std::int64_t>(seconds) * 75,
                       .provider_attempt_started = false,
                       .provider_attempt_started_at_ms = std::nullopt,
                       .created_at_ms = now,
                       .active_at_ms = std::nullopt,
                       .ended_at_ms = std::nullopt,
                       .public_message_id = std::nullopt,
                       .terminal_reason = std::nullopt},
            .interaction_idempotency_key = "listen:" + std::to_string(base),
            .request_fingerprint = std::to_string(seconds),
            .transition_id = uuid(base + 100)};
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  sanguinius::persistence::Database database;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteVoiceListeningRepository>
      repository;
  std::string session_id{uuid(800)};
};

} // namespace

TEST_CASE(
    "voice listening repository reserves atomically and audits transitions",
    "[voice-input][database][budget]") {
  Fixture fixture;
  fixture.repository->record_consent_attestation(true, 30, uuid(1), 20);
  const sanguinius::TranscriptionUsagePolicy policy{};
  auto begun = fixture.repository->begin(fixture.request(10, 100), policy);
  REQUIRE(begun.code == sanguinius::VoiceWindowBeginCode::created);
  REQUIRE(begun.window.has_value());
  REQUIRE(begun.window->reserved_micro_usd == 1'125);
  REQUIRE(fixture.repository->begin(fixture.request(20, 101), policy).code ==
          sanguinius::VoiceWindowBeginCode::active_window);
  REQUIRE(fixture.repository->begin(fixture.request(10, 100), policy).code ==
          sanguinius::VoiceWindowBeginCode::replay);

  auto current = *begun.window;
  auto advance = [&](const VoiceListeningState target,
                     const std::size_t sequence) {
    auto updated = fixture.repository->transition(
        {.window_id = current.window_id,
         .expected_revision = current.revision,
         .target = target,
         .reason = "test_transition",
         .actor_user_id = std::nullopt,
         .transition_id = uuid(200 + sequence),
         .idempotency_key = "transition:" + std::to_string(sequence),
         .now_ms = 100 + static_cast<std::int64_t>(sequence)});
    REQUIRE(updated.has_value());
    current = *updated;
  };
  advance(VoiceListeningState::arming_transport, 1);
  fixture.repository->record_public_message(current.window_id, 50, 102);
  advance(VoiceListeningState::arming_indicator, 2);
  advance(VoiceListeningState::active, 3);
  advance(VoiceListeningState::transcribing, 4);
  fixture.repository->record_provider_attempt(current.window_id, 108);
  fixture.repository->record_usage({.window_id = current.window_id,
                                    .provider = "openai",
                                    .model = "gpt-transcribe",
                                    .provider_request_id = "request-id",
                                    .captured_bytes = 960'000,
                                    .captured_duration_ms = 5'000,
                                    .estimated_micro_usd = 1'125,
                                    .latency_ms = 250,
                                    .result_code = "completed",
                                    .provider_sent = true,
                                    .recorded_at_ms = 110});
  advance(VoiceListeningState::completed, 5);
  const auto health = fixture.repository->health(120);
  REQUIRE(health.active_windows == 0);
  REQUIRE(health.day_windows == 1);
  REQUIRE(health.day_micro_usd == 1'125);
  REQUIRE(health.last_result_code == "completed");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_listening_window SET state='failed',state_version="
      "state_version+1 WHERE window_id='" +
      current.window_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE voice_listening_window SET requester_user_id='32' WHERE "
      "window_id='" +
      current.window_id + "'"));
}

TEST_CASE("completed usage and window transition commit atomically",
          "[voice-input][database][transcription][atomicity]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  fixture.repository->record_consent_attestation(true, 30, uuid(910), 100);
  auto current =
      *fixture.repository->begin(fixture.request(110, 200, 5), policy).window;
  std::size_t sequence{};
  for (const auto target :
       {VoiceListeningState::arming_transport,
        VoiceListeningState::arming_indicator, VoiceListeningState::active,
        VoiceListeningState::transcribing}) {
    ++sequence;
    auto updated = fixture.repository->transition(
        {.window_id = current.window_id,
         .expected_revision = current.revision,
         .target = target,
         .reason = "completion_setup",
         .actor_user_id = std::nullopt,
         .transition_id = uuid(920 + sequence),
         .idempotency_key = "completion:setup:" + std::to_string(sequence),
         .now_ms = 200 + static_cast<std::int64_t>(sequence)});
    REQUIRE(updated.has_value());
    current = *updated;
  }
  fixture.repository->record_provider_attempt(current.window_id, 210);

  const sanguinius::VoiceWindowTransitionRequest completion{
      .window_id = current.window_id,
      .expected_revision = current.revision,
      .target = VoiceListeningState::completed,
      .reason = "transcription_completed",
      .actor_user_id = std::nullopt,
      .transition_id = uuid(930),
      .idempotency_key = "completion:final",
      .now_ms = 220};
  const sanguinius::VoiceTranscriptionUsage usage{
      .window_id = current.window_id,
      .provider = "openai",
      .model = "gpt-transcribe",
      .provider_request_id = "request-id",
      .captured_bytes = 960'000,
      .captured_duration_ms = 5'000,
      .estimated_micro_usd = 375,
      .latency_ms = 250,
      .result_code = "completed",
      .provider_sent = true,
      .recorded_at_ms = 220};

  fixture.context->connection().execute(
      "CREATE TRIGGER fail_completed_transition BEFORE INSERT ON "
      "voice_listening_transition WHEN NEW.to_state='completed' BEGIN SELECT "
      "RAISE(ABORT,'forced completion failure'); END");
  REQUIRE_THROWS(fixture.repository->complete_transcription(completion, usage));
  auto rolled_back = fixture.context->connection().prepare(
      "SELECT state,state_version,(SELECT count(*) FROM "
      "voice_transcription_usage WHERE window_id=?) FROM "
      "voice_listening_window WHERE window_id=?");
  rolled_back.bind(1, current.window_id);
  rolled_back.bind(2, current.window_id);
  REQUIRE(rolled_back.step());
  REQUIRE(rolled_back.column_text(0) == "transcribing");
  REQUIRE(rolled_back.column_int64(1) ==
          static_cast<std::int64_t>(current.revision));
  REQUIRE(rolled_back.column_int64(2) == 0);

  fixture.context->connection().execute(
      "DROP TRIGGER fail_completed_transition");
  const auto completed =
      fixture.repository->complete_transcription(completion, usage);
  REQUIRE(completed.has_value());
  REQUIRE(completed->state == VoiceListeningState::completed);
  REQUIRE(fixture.repository->health(230).last_result_code == "completed");
  const auto replay =
      fixture.repository->complete_transcription(completion, usage);
  REQUIRE(replay.has_value());
  REQUIRE(replay->revision == completed->revision);
}

TEST_CASE("voice restart retains an ambiguous provider-attempt reservation",
          "[voice-input][database][restart][billing]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  fixture.repository->record_consent_attestation(true, 30, uuid(701), 100);
  auto current =
      *fixture.repository->begin(fixture.request(70, 200, 5), policy).window;
  for (const auto target :
       {VoiceListeningState::arming_transport,
        VoiceListeningState::arming_indicator, VoiceListeningState::active,
        VoiceListeningState::transcribing}) {
    auto updated = fixture.repository->transition(
        {.window_id = current.window_id,
         .expected_revision = current.revision,
         .target = target,
         .reason = "attempt_setup",
         .actor_user_id = std::nullopt,
         .transition_id = uuid(710 + current.revision),
         .idempotency_key = "attempt:" + std::to_string(current.revision),
         .now_ms = 200 + static_cast<std::int64_t>(current.revision)});
    REQUIRE(updated.has_value());
    current = *updated;
  }

  fixture.repository->record_provider_attempt(current.window_id, 210);
  auto marked = fixture.repository->active();
  REQUIRE(marked.has_value());
  REQUIRE(marked->provider_attempt_started);
  REQUIRE(marked->provider_attempt_started_at_ms == 210);
  REQUIRE(fixture.repository->abandon_nonterminal(250, "restart", "restart:") ==
          1);
  fixture.repository->release_reservation(current.window_id, 260);

  auto audit = fixture.context->connection().prepare(
      "SELECT provider_attempt_started,reservation_released FROM "
      "voice_listening_window WHERE window_id=?");
  audit.bind(1, current.window_id);
  REQUIRE(audit.step());
  REQUIRE(audit.column_int64(0) == 1);
  REQUIRE(audit.column_int64(1) == 0);
  REQUIRE(fixture.repository->health(300).day_micro_usd == 375);
}

TEST_CASE("known-unsent provider attempts release their reservation",
          "[voice-input][database][billing]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  fixture.repository->record_consent_attestation(true, 30, uuid(750), 100);
  auto current =
      *fixture.repository->begin(fixture.request(80, 200, 5), policy).window;
  for (const auto target :
       {VoiceListeningState::arming_transport,
        VoiceListeningState::arming_indicator, VoiceListeningState::active,
        VoiceListeningState::transcribing}) {
    auto updated = fixture.repository->transition(
        {.window_id = current.window_id,
         .expected_revision = current.revision,
         .target = target,
         .reason = "unsent_setup",
         .actor_user_id = std::nullopt,
         .transition_id = uuid(760 + current.revision),
         .idempotency_key = "unsent:" + std::to_string(current.revision),
         .now_ms = 200 + static_cast<std::int64_t>(current.revision)});
    REQUIRE(updated.has_value());
    current = *updated;
  }
  fixture.repository->record_provider_attempt(current.window_id, 210);
  fixture.repository->record_usage({.window_id = current.window_id,
                                    .provider = "openai",
                                    .model = "gpt-transcribe",
                                    .provider_request_id = std::nullopt,
                                    .captured_bytes = 960,
                                    .captured_duration_ms = 5,
                                    .estimated_micro_usd = 0,
                                    .latency_ms = 10,
                                    .result_code = "transport",
                                    .provider_sent = false,
                                    .recorded_at_ms = 220});
  REQUIRE(fixture.repository->health(230).day_micro_usd == 0);
}

TEST_CASE("known-unsent accounting rolls back usage and release together",
          "[voice-input][database][billing][atomicity]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  fixture.repository->record_consent_attestation(true, 30, uuid(780), 100);
  auto current =
      *fixture.repository->begin(fixture.request(90, 200, 5), policy).window;
  for (const auto target :
       {VoiceListeningState::arming_transport,
        VoiceListeningState::arming_indicator, VoiceListeningState::active,
        VoiceListeningState::transcribing}) {
    auto updated = fixture.repository->transition(
        {.window_id = current.window_id,
         .expected_revision = current.revision,
         .target = target,
         .reason = "atomic_setup",
         .actor_user_id = std::nullopt,
         .transition_id = uuid(790 + current.revision),
         .idempotency_key = "atomic:" + std::to_string(current.revision),
         .now_ms = 200 + static_cast<std::int64_t>(current.revision)});
    REQUIRE(updated.has_value());
    current = *updated;
  }
  fixture.repository->record_provider_attempt(current.window_id, 210);
  fixture.context->connection().execute(
      "CREATE TRIGGER fail_voice_reservation_release BEFORE UPDATE OF "
      "reservation_released ON voice_listening_window WHEN "
      "NEW.reservation_released=1 BEGIN SELECT RAISE(ABORT,'forced release "
      "failure'); END");
  const sanguinius::VoiceTranscriptionUsage usage{
      .window_id = current.window_id,
      .provider = "openai",
      .model = "gpt-transcribe",
      .provider_request_id = std::nullopt,
      .captured_bytes = 960,
      .captured_duration_ms = 5,
      .estimated_micro_usd = 0,
      .latency_ms = 10,
      .result_code = "transport",
      .provider_sent = false,
      .recorded_at_ms = 220};
  REQUIRE_THROWS(fixture.repository->record_usage(usage));
  auto rolled_back = fixture.context->connection().prepare(
      "SELECT reservation_released,(SELECT count(*) FROM "
      "voice_transcription_usage WHERE window_id=?) FROM "
      "voice_listening_window WHERE window_id=?");
  rolled_back.bind(1, current.window_id);
  rolled_back.bind(2, current.window_id);
  REQUIRE(rolled_back.step());
  REQUIRE(rolled_back.column_int64(0) == 0);
  REQUIRE(rolled_back.column_int64(1) == 0);

  fixture.context->connection().execute(
      "DROP TRIGGER fail_voice_reservation_release");
  fixture.repository->record_usage(usage);
  auto committed = fixture.context->connection().prepare(
      "SELECT reservation_released,(SELECT count(*) FROM "
      "voice_transcription_usage WHERE window_id=?) FROM "
      "voice_listening_window WHERE window_id=?");
  committed.bind(1, current.window_id);
  committed.bind(2, current.window_id);
  REQUIRE(committed.step());
  REQUIRE(committed.column_int64(0) == 1);
  REQUIRE(committed.column_int64(1) == 1);
  REQUIRE(fixture.repository->health(230).day_micro_usd == 0);
}

TEST_CASE("voice restart abandonment releases unsent reservations",
          "[voice-input][database][restart][privacy]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  REQUIRE(fixture.repository->begin(fixture.request(29, 199, 5), policy).code ==
          sanguinius::VoiceWindowBeginCode::consent_missing);
  fixture.repository->record_consent_attestation(true, 30, uuid(2), 199);
  REQUIRE(fixture.repository->begin(fixture.request(30, 200, 5), policy).code ==
          sanguinius::VoiceWindowBeginCode::created);
  REQUIRE(fixture.repository->abandon_nonterminal(250, "restart", "restart:") ==
          1);
  REQUIRE_FALSE(fixture.repository->active().has_value());
  const auto health = fixture.repository->health(300);
  REQUIRE(health.day_windows == 1);
  REQUIRE(health.day_micro_usd == 0);

  fixture.repository->set_kill_switch(true, 30, uuid(500), 301);
  REQUIRE(fixture.repository->kill_switch_enabled());
  REQUIRE(fixture.repository->begin(fixture.request(40, 302, 5), policy).code ==
          sanguinius::VoiceWindowBeginCode::kill_switch);
}

TEST_CASE("voice audit cleanup survives wall-clock rollback",
          "[voice-input][database][privacy][clock]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};

  fixture.repository->record_consent_attestation(true, 30, uuid(3), 500);
  fixture.repository->record_consent_attestation(false, 30, uuid(4), 100);
  REQUIRE(
      fixture.repository->begin(fixture.request(50, 1'000, 5), policy).code ==
      sanguinius::VoiceWindowBeginCode::consent_missing);
  fixture.repository->record_consent_attestation(true, 30, uuid(5), 50);

  auto begun = fixture.repository->begin(fixture.request(50, 1'000, 5), policy);
  REQUIRE(begun.code == sanguinius::VoiceWindowBeginCode::created);
  REQUIRE(begun.window.has_value());
  auto stopped = fixture.repository->transition(
      {.window_id = begun.window->window_id,
       .expected_revision = begun.window->revision,
       .target = VoiceListeningState::stopped,
       .reason = "clock_rollback",
       .actor_user_id = 32,
       .transition_id = uuid(600),
       .idempotency_key = "transition:clock-rollback",
       .now_ms = 800});
  REQUIRE(stopped.has_value());
  REQUIRE(stopped->ended_at_ms == 1'000);
  fixture.repository->release_reservation(stopped->window_id, 700);

  auto stopped_audit = fixture.context->connection().prepare(
      "SELECT reservation_released_at_ms,occurred_at_ms FROM "
      "voice_listening_window w JOIN voice_listening_transition t ON "
      "t.window_id=w.window_id AND t.to_version=w.state_version WHERE "
      "w.window_id=?");
  stopped_audit.bind(1, stopped->window_id);
  REQUIRE(stopped_audit.step());
  REQUIRE(stopped_audit.column_int64(0) == 1'000);
  REQUIRE(stopped_audit.column_int64(1) == 1'000);

  auto pending =
      fixture.repository->begin(fixture.request(60, 1'100, 5), policy);
  REQUIRE(pending.code == sanguinius::VoiceWindowBeginCode::created);
  REQUIRE(pending.window.has_value());
  REQUIRE(fixture.repository->abandon_nonterminal(600, "restart", "restart:") ==
          1);
  auto abandoned = fixture.context->connection().prepare(
      "SELECT ended_at_ms,reservation_released_at_ms,occurred_at_ms FROM "
      "voice_listening_window w JOIN voice_listening_transition t ON "
      "t.window_id=w.window_id AND t.to_version=w.state_version WHERE "
      "w.window_id=?");
  abandoned.bind(1, pending.window->window_id);
  REQUIRE(abandoned.step());
  REQUIRE(abandoned.column_int64(0) == 1'100);
  REQUIRE(abandoned.column_int64(1) == 1'100);
  REQUIRE(abandoned.column_int64(2) == 1'100);
}

TEST_CASE("voice stop transition creates a first-seen audit actor atomically",
          "[voice-input][database][privacy][identity]") {
  Fixture fixture;
  const sanguinius::TranscriptionUsagePolicy policy{};
  fixture.repository->record_consent_attestation(true, 30, uuid(650), 100);
  auto begun = fixture.repository->begin(fixture.request(65, 200, 5), policy);
  REQUIRE(begun.code == sanguinius::VoiceWindowBeginCode::created);
  REQUIRE(begun.window.has_value());

  const sanguinius::DiscordSnowflake first_seen_stopper{99};
  auto stopped = fixture.repository->transition(
      {.window_id = begun.window->window_id,
       .expected_revision = begun.window->revision,
       .target = VoiceListeningState::stopped,
       .reason = "first_seen_stop",
       .actor_user_id = first_seen_stopper,
       .transition_id = uuid(651),
       .idempotency_key = "transition:first-seen-stop",
       .now_ms = 210});
  REQUIRE(stopped.has_value());
  REQUIRE(stopped->state == VoiceListeningState::stopped);

  auto audit = fixture.context->connection().prepare(
      "SELECT u.is_bot,u.first_seen_at_ms,p.updated_at_ms,t.actor_user_id "
      "FROM discord_user u JOIN user_preference p ON p.user_id=u.user_id "
      "JOIN voice_listening_transition t ON t.actor_user_id=u.user_id "
      "WHERE u.user_id=? AND t.window_id=? AND t.to_state='stopped'");
  audit.bind(1, first_seen_stopper.str());
  audit.bind(2, stopped->window_id);
  REQUIRE(audit.step());
  REQUIRE(audit.column_int64(0) == 0);
  REQUIRE(audit.column_int64(1) == 210);
  REQUIRE(audit.column_int64(2) == 210);
  REQUIRE(audit.column_text(3) == first_seen_stopper.str());
  REQUIRE_FALSE(audit.step());
}

TEST_CASE("voice input schema has no audio or transcript content columns",
          "[voice-input][database][privacy]") {
  Fixture fixture;
  for (const std::string table :
       {"voice_listening_window", "voice_listening_transition",
        "voice_transcription_usage"}) {
    auto columns = fixture.context->connection().prepare(
        "SELECT lower(name) FROM pragma_table_info(?)");
    columns.bind(1, table);
    while (columns.step()) {
      const auto name = columns.column_text(0);
      REQUIRE(name.find("audio") == std::string::npos);
      REQUIRE(name.find("transcript_text") == std::string::npos);
      REQUIRE(name.find("body") == std::string::npos);
      REQUIRE(name.find("json") == std::string::npos);
    }
  }
}
