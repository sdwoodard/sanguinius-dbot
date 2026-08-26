#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_speech_repository.hpp"
#include "sanguinius/persistence/sqlite_vox_repository.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/speech_service.hpp"
#include "sanguinius/tts.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_speech.hpp"
#include "support/fake_vox.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using sanguinius::SpeechEnqueueStatus;
using sanguinius::SpeechMutationStatus;
using sanguinius::SpeechPriority;
using sanguinius::SpeechState;
using sanguinius::VoxResultCode;
using sanguinius::VoxState;
using namespace std::chrono_literals;

std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "50000000-0000-4000-8000-" + suffix;
}

struct SpeechFixture {
  SpeechFixture()
      : database{sanguinius::persistence::Database::open_migration(
            temporary.path(), 25ms)} {
    sanguinius::persistence::Migrator migrator{
        sanguinius::persistence::production_migrations(),
        {"test", "revision"},
        clock};
    REQUIRE(migrator.apply(database.connection()).current_version == 13);
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 10);
    identities.ensure_user({30, "Owner", "owner", false, 10});
    identities.ensure_user({31, "Summoner", "summoner", false, 10});
    sanguinius::persistence::SqliteApplicationInstanceRepository instances{
        context};
    instances.record_start({uuid(900), "test", "revision", "host", 1, 10});
    vox =
        std::make_unique<sanguinius::persistence::SqliteVoxRepository>(context);
    speech = std::make_unique<sanguinius::persistence::SqliteSpeechRepository>(
        context);
    const sanguinius::VoxCommandContext command{.guild_id = 10,
                                                .text_channel_id = 20,
                                                .actor_user_id = 31,
                                                .owner_user_id = 30,
                                                .interaction_idempotency_key =
                                                    "speech:summon",
                                                .correlation_id = "speech-test",
                                                .now_ms = 100};
    auto started = vox->start({.context = command,
                               .voice_channel_id = 40,
                               .session_id = uuid(1),
                               .event_id = uuid(2),
                               .timeout_job_id = uuid(3),
                               .deployment_instance_id = uuid(900)});
    REQUIRE(started.code == VoxResultCode::accepted);
    REQUIRE(started.session.has_value());
    auto finalized =
        vox->finalize_summon(command, started.session->session_id,
                             started.session->revision, true, uuid(4));
    REQUIRE(finalized.code == VoxResultCode::accepted);
    REQUIRE(finalized.session.has_value());
    auto ready =
        vox->transition({.session_id = finalized.session->session_id,
                         .expected_revision = finalized.session->revision,
                         .target = VoxState::ready,
                         .reason = "voice_ready",
                         .actor_user_id = std::nullopt,
                         .event_id = uuid(5),
                         .idempotency_key = "speech:ready",
                         .correlation_id = "speech-test",
                         .now_ms = 200,
                         .timeout_job_id = std::nullopt,
                         .timeout_due_at_ms = std::nullopt,
                         .failure_category = std::nullopt,
                         .public_card = false},
                        std::nullopt);
    REQUIRE(ready.code == VoxResultCode::accepted);
    session_id = ready.session->session_id;
  }

  sanguinius::SpeechEnqueueRequest
  request(const std::size_t number, const SpeechPriority priority,
          const std::string &text = "The vox is open.") const {
    const auto normalized = sanguinius::normalize_tts_text(text);
    const auto text_bytes = std::as_bytes(
        std::span{normalized.text.data(), normalized.text.size()});
    return {.speech_id = uuid(100 + number),
            .voice_session_id = session_id,
            .source_event_id = std::nullopt,
            .source_kind = "direct_say",
            .text = normalized,
            .text_hash = sanguinius::sha256_hex(text_bytes),
            .provider = "openai",
            .model = "tts-1",
            .voice = "onyx",
            .priority = priority,
            .earliest_at_ms = 300,
            .expires_at_ms = 120'300,
            .interruptible = priority != SpeechPriority::interactive,
            .deduplication_key = "speech:dedupe:" + std::to_string(number),
            .created_at_ms = 300};
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  sanguinius::persistence::Database database;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteVoxRepository> vox;
  std::unique_ptr<sanguinius::persistence::SqliteSpeechRepository> speech;
  std::string session_id;
};

class RetryThenSucceedTts final : public sanguinius::TextToSpeechClient {
public:
  sanguinius::SynthesizedAudio synthesize(const sanguinius::TtsRequest &,
                                          std::stop_token) const override {
    const std::scoped_lock lock{mutex_};
    ++calls_;
    if (calls_ == 1)
      throw sanguinius::TtsError{sanguinius::TtsFailureCategory::transport,
                                 "fixture transport", true,
                                 std::chrono::milliseconds{1}};
    return {.bytes = {std::byte{'R'}, std::byte{'I'}, std::byte{'F'},
                      std::byte{'F'}, std::byte{0}, std::byte{0}, std::byte{0},
                      std::byte{0}, std::byte{'W'}, std::byte{'A'},
                      std::byte{'V'}, std::byte{'E'}},
            .format = sanguinius::AudioFormat::wav,
            .content_type = "audio/wav",
            .provider_request_id = "fixture-request"};
  }

  [[nodiscard]] std::size_t calls() const {
    const std::scoped_lock lock{mutex_};
    return calls_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::size_t calls_{};
};

bool wait_for_count(const std::atomic<std::size_t> &value,
                    const std::size_t expected) {
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    if (value.load() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return false;
}

sanguinius::SpeechServiceConfiguration
speech_configuration(const bool provider_enabled) {
  sanguinius::SpeechServiceConfiguration result;
  result.provider_enabled = provider_enabled;
  result.queue_capacity = 16;
  return result;
}

} // namespace

TEST_CASE("speech queue persists priority ordering and duplicate replay",
          "[speech][persistence][queue]") {
  SpeechFixture fixture;
  const auto flavor =
      fixture.speech->enqueue(fixture.request(1, SpeechPriority::flavor));
  const auto interactive =
      fixture.speech->enqueue(fixture.request(2, SpeechPriority::interactive));
  REQUIRE(flavor.status == SpeechEnqueueStatus::accepted);
  REQUIRE(interactive.status == SpeechEnqueueStatus::accepted);
  const auto replay =
      fixture.speech->enqueue(fixture.request(2, SpeechPriority::interactive));
  REQUIRE(replay.status == SpeechEnqueueStatus::replay);
  REQUIRE(replay.item->speech_id == interactive.item->speech_id);

  const auto claimed = fixture.speech->claim_next(
      fixture.session_id, 301, uuid(300), "speech:claim:interactive");
  REQUIRE(claimed.has_value());
  REQUIRE(claimed->speech_id == interactive.item->speech_id);
  REQUIRE(claimed->state == SpeechState::synthesizing);
  REQUIRE(claimed->text.has_value());
}

TEST_CASE(
    "speech usage is pessimistically reserved and media metadata clears text",
    "[speech][persistence][budget][cache]") {
  SpeechFixture fixture;
  const auto enqueued =
      fixture.speech->enqueue(fixture.request(1, SpeechPriority::interactive));
  const auto claimed = fixture.speech->claim_next(
      fixture.session_id, 301, uuid(300), "speech:claim:one");
  REQUIRE(claimed.has_value());
  const auto reservation = fixture.speech->reserve_usage(
      {.attempt_id = uuid(400),
       .speech_id = claimed->speech_id,
       .attempt_number = 1,
       .provider = "openai",
       .model = "tts-1",
       .voice = "onyx",
       .scalar_count = claimed->scalar_count,
       .estimated_micro_usd =
           sanguinius::estimated_tts_cost_micro_usd(claimed->scalar_count),
       .now_ms = 302,
       .calendar_month_start_ms = 0,
       .policy = {}});
  REQUIRE(reservation.accepted);
  REQUIRE(reservation.usage.rolling_day_attempts == 1);
  REQUIRE(fixture.speech->complete_usage(
              {.attempt_id = uuid(400),
               .state = "succeeded",
               .provider_request_id = "provider-request",
               .latency_ms = 50,
               .duration_ms = 100,
               .error_code = std::nullopt,
               .completed_at_ms = 100}) == SpeechMutationStatus::applied);
  {
    auto completion = fixture.context->connection().prepare(
        "SELECT submitted_at_ms,completed_at_ms FROM tts_usage_attempt WHERE "
        "attempt_id=?");
    completion.bind(1, uuid(400));
    REQUIRE(completion.step());
    REQUIRE(completion.column_int64(1) == completion.column_int64(0));
  }

  const std::string cache_key(64, 'a');
  const std::string checksum(64, 'b');
  fixture.speech->put_cache_metadata({.cache_key = cache_key,
                                      .checksum = checksum,
                                      .byte_count = 19'200,
                                      .frame_count = 4'800,
                                      .provider = "openai",
                                      .model = "tts-1",
                                      .voice = "onyx",
                                      .created_at_ms = 352,
                                      .last_access_at_ms = 352});
  REQUIRE(fixture.speech->transition({.speech_id = claimed->speech_id,
                                      .expected_revision = claimed->revision,
                                      .target = SpeechState::ready,
                                      .transition_id = uuid(401),
                                      .reason = "synthesis_ready",
                                      .idempotency_key = "speech:ready:one",
                                      .occurred_at_ms = 352,
                                      .provider_request_id = "provider-request",
                                      .cache_key = cache_key,
                                      .cache_checksum = checksum,
                                      .marker = std::nullopt,
                                      .duration_ms = 100,
                                      .error_code = std::nullopt}) ==
          SpeechMutationStatus::applied);
  const auto metadata = fixture.speech->cache_metadata(cache_key, 400);
  REQUIRE(metadata.has_value());
  REQUIRE(metadata->last_access_at_ms == 400);
  const auto health = fixture.speech->health(400, 0);
  REQUIRE(health.ready == 1);
  REQUIRE(health.usage.rolling_day_attempts == 1);
}

TEST_CASE("speech restart recovery terminally cancels every active item",
          "[speech][persistence][restart]") {
  SpeechFixture fixture;
  REQUIRE(
      fixture.speech->enqueue(fixture.request(1, SpeechPriority::interactive))
          .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech->enqueue(fixture.request(2, SpeechPriority::flavor))
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech->recover(500, "restart_abandoned") == 2);
  const auto health = fixture.speech->health(500, 0);
  REQUIRE(health.queued == 0);
  REQUIRE(health.synthesizing == 0);
  REQUIRE(health.ready == 0);
  REQUIRE(health.playing == 0);
}

TEST_CASE("speech terminal audit clamps a backward wall clock",
          "[speech][persistence][clock]") {
  SpeechFixture fixture;
  REQUIRE(
      fixture.speech->enqueue(fixture.request(1, SpeechPriority::interactive))
          .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech->cancel_session(fixture.session_id, 100,
                                         "clock_rollback", true) == 1);
  auto terminal = fixture.context->connection().prepare(
      "SELECT terminal_at_ms FROM speech_item WHERE speech_id=?");
  terminal.bind(1, uuid(101));
  REQUIRE(terminal.step());
  REQUIRE(terminal.column_int64(0) == 300);
  auto audit = fixture.context->connection().prepare(
      "SELECT occurred_at_ms FROM speech_item_transition WHERE speech_id=? "
      "ORDER BY to_version DESC LIMIT 1");
  audit.bind(1, uuid(101));
  REQUIRE(audit.step());
  REQUIRE(audit.column_int64(0) == 300);
}

TEST_CASE("speech queue expires stale work and protects reserved control slots",
          "[speech][persistence][queue][expiry][capacity]") {
  SpeechFixture fixture;
  auto expired = fixture.request(1, SpeechPriority::flavor);
  expired.expires_at_ms = 301;
  REQUIRE(fixture.speech->enqueue(expired).status ==
          SpeechEnqueueStatus::accepted);
  REQUIRE_FALSE(fixture.speech
                    ->claim_next(fixture.session_id, 301, uuid(350),
                                 "speech:claim:expired")
                    .has_value());

  for (std::size_t index = 2; index < 18; ++index) {
    auto request = fixture.request(index, SpeechPriority::flavor);
    request.created_at_ms += static_cast<std::int64_t>(index);
    request.earliest_at_ms = request.created_at_ms;
    REQUIRE(fixture.speech->enqueue(request).status ==
            SpeechEnqueueStatus::accepted);
  }
  auto direct = fixture.request(18, SpeechPriority::interactive);
  direct.created_at_ms = 100;
  direct.earliest_at_ms = 100;
  const auto admitted = fixture.speech->enqueue(direct);
  REQUIRE(admitted.status == SpeechEnqueueStatus::accepted);
  REQUIRE(admitted.evicted_speech_id == uuid(102));
  auto eviction = fixture.context->connection().prepare(
      "SELECT terminal_at_ms FROM speech_item WHERE speech_id=?");
  eviction.bind(1, uuid(102));
  REQUIRE(eviction.step());
  REQUIRE(eviction.column_int64(0) == 302);
  auto eviction_audit = fixture.context->connection().prepare(
      "SELECT occurred_at_ms FROM speech_item_transition WHERE speech_id=? "
      "ORDER BY to_version DESC LIMIT 1");
  eviction_audit.bind(1, uuid(102));
  REQUIRE(eviction_audit.step());
  REQUIRE(eviction_audit.column_int64(0) == 302);

  REQUIRE(fixture.speech
              ->enqueue(fixture.request(20, SpeechPriority::critical_control))
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech
              ->enqueue(fixture.request(21, SpeechPriority::critical_control))
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech
              ->enqueue(fixture.request(22, SpeechPriority::critical_control))
              .status == SpeechEnqueueStatus::queue_full);
}

TEST_CASE("speech reconnect keeps direct work and cancels flavor",
          "[speech][persistence][queue][reconnect]") {
  SpeechFixture fixture;
  REQUIRE(fixture.speech->enqueue(fixture.request(1, SpeechPriority::flavor))
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(
      fixture.speech->enqueue(fixture.request(2, SpeechPriority::interactive))
          .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech->cancel_session(fixture.session_id, 400,
                                         "reconnect_stale", false) == 1);
  const auto retained = fixture.speech->claim_next(
      fixture.session_id, 401, uuid(360), "speech:claim:retained");
  REQUIRE(retained.has_value());
  REQUIRE(retained->priority == SpeechPriority::interactive);
}

TEST_CASE("a leaving session accepts only its bounded static farewell",
          "[speech][persistence][queue][leave]") {
  SpeechFixture fixture;
  const sanguinius::VoxCommandContext leave_context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "speech:leave",
      .correlation_id = "speech-leave-test",
      .now_ms = 400};
  const auto leaving =
      fixture.vox->command_leave(leave_context, uuid(380), uuid(381));
  REQUIRE(leaving.code == VoxResultCode::accepted);
  REQUIRE(leaving.session->state == VoxState::leaving);

  auto direct = fixture.request(1, SpeechPriority::interactive);
  direct.created_at_ms = 401;
  direct.earliest_at_ms = 401;
  direct.expires_at_ms = 120'401;
  REQUIRE(fixture.speech->enqueue(direct).status ==
          SpeechEnqueueStatus::invalid_session);

  auto farewell = fixture.request(2, SpeechPriority::flavor,
                                  "The channel closes. Until we speak again.");
  farewell.source_kind = "static_farewell";
  farewell.provider = "static";
  farewell.model = "static-v1";
  farewell.created_at_ms = 401;
  farewell.earliest_at_ms = 401;
  farewell.expires_at_ms = 15'401;
  REQUIRE(fixture.speech->enqueue(farewell).status ==
          SpeechEnqueueStatus::accepted);
}

TEST_CASE(
    "speech purge schedule is idempotent and retention deletes raw metadata",
    "[speech][persistence][purge][retention]") {
  SpeechFixture fixture;
  fixture.speech->ensure_purge_schedule(500, uuid(370));
  fixture.speech->ensure_purge_schedule(500, uuid(371));
  {
    auto scheduled = fixture.context->connection().prepare(
        "SELECT COUNT(*) FROM scheduled_job WHERE job_type='vox.tts_purge.v1'");
    REQUIRE(scheduled.step());
    REQUIRE(scheduled.column_int64(0) == 1);
  }
  REQUIRE(
      fixture.speech->enqueue(fixture.request(1, SpeechPriority::interactive))
          .status == SpeechEnqueueStatus::accepted);
  REQUIRE(fixture.speech->recover(600, "restart_abandoned") == 1);
  const auto after_retention = 600 + 31LL * 24 * 60 * 60 * 1'000;
  REQUIRE(fixture.speech->purge_retained(after_retention) == 1);
  auto remaining =
      fixture.context->connection().prepare("SELECT COUNT(*) FROM speech_item");
  REQUIRE(remaining.step());
  REQUIRE(remaining.column_int64(0) == 0);
}

TEST_CASE("speech startup reconciles cache files and metadata both ways",
          "[speech][service][cache][purge][recovery]") {
  SpeechFixture fixture;
  sanguinius::test::FakeTtsCache cache;
  const auto text = sanguinius::normalize_tts_text("Expired cache metadata");
  const auto key = sanguinius::tts_cache_key(text, {.text = text.text});
  const auto checksum = sanguinius::sha256_hex(
      std::array{std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0}});
  fixture.speech->put_cache_metadata({.cache_key = key,
                                      .checksum = checksum,
                                      .byte_count = 4,
                                      .frame_count = 1,
                                      .provider = "openai",
                                      .model = "tts-1",
                                      .voice = "onyx",
                                      .created_at_ms = 1,
                                      .last_access_at_ms = 1});
  const auto orphan_text =
      sanguinius::normalize_tts_text("Orphaned cache file");
  const auto orphan_key =
      sanguinius::tts_cache_key(orphan_text, {.text = orphan_text.text});

  sanguinius::test::FakeVoiceGateway gateway;
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  static_cast<void>(cache.write(orphan_key, clip));
  sanguinius::SpeechService service{
      *fixture.speech,
      nullptr,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(false)};
  service.start();
  const auto health = fixture.speech->health(1, 0);
  REQUIRE(health.cache_entries == 0);
  REQUIRE(health.cache_bytes == 0);
  REQUIRE(cache.keys().empty());
  service.stop();
}

TEST_CASE("speech service charges retries once and reuses normalized cache",
          "[speech][service][retry][budget][cache][playback]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  RetryThenSucceedTts client;
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      *fixture.speech,
      &client,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = std::move(clip)},
      {.provider_enabled = true,
       .usage_policy = {},
       .normalization_limits = {},
       .maximum_attempts = 2,
       .queue_capacity = 16}};
  service.start();
  service.session_ready(fixture.session_id, "10", false, false);
  REQUIRE(service
              .say(fixture.session_id, "10", "Hold fast.",
                   "speech:interaction:one", 300)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(gateway.wait_for_sends(1, 2s));
  REQUIRE(client.calls() == 2);
  std::string marker;
  {
    const std::scoped_lock lock{fixture.context->mutex()};
    auto playing = fixture.context->connection().prepare(
        "SELECT marker FROM speech_item WHERE state='playing'");
    REQUIRE(playing.step());
    marker = playing.column_text(0);
  }
  REQUIRE(service.track_marker(fixture.session_id, std::move(marker)));

  REQUIRE(service
              .say(fixture.session_id, "10", "Hold fast.",
                   "speech:interaction:two", 301)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(gateway.wait_for_sends(2, 2s));
  REQUIRE(client.calls() == 2);
  const auto health = service.health();
  REQUIRE(health.synthesis_worker.capacity == 16);
  REQUIRE(health.playback_worker.capacity == 16);
  REQUIRE(health.repository.usage.rolling_day_attempts == 2);
  REQUIRE(health.repository.usage.rolling_day_succeeded == 1);
  REQUIRE(health.repository.usage.rolling_day_unknown == 1);
  REQUIRE(health.cache.hits == 1);
  service.stop();
}

TEST_CASE("speech marker completion retries transient persistence failures",
          "[speech][service][marker][persistence][retry]") {
  sanguinius::test::FakeSpeechRepository repository;
  sanguinius::test::FakeClock clock;
  clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = "session",
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  RetryThenSucceedTts client;
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      repository,
      &client,
      normalizer,
      cache,
      gateway,
      clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(true)};
  service.start();
  service.session_ready("session", "10", false, false);
  REQUIRE(service
              .say("session", "10", "Persistence is the fence.",
                   "speech:interaction:persistence", 1'000)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(gateway.wait_for_sends(1, 2s));
  const auto attempts_before = repository.transition_attempts();
  repository.fail_next_transitions(2);
  REQUIRE(service.track_marker("session", gateway.marker()));

  bool played{};
  for (std::size_t attempt = 0; attempt < 200 && !played; ++attempt) {
    const auto items = repository.items();
    played = items.size() == 1 && items.front().state == SpeechState::played;
    if (!played)
      std::this_thread::sleep_for(5ms);
  }
  REQUIRE(played);
  REQUIRE(repository.transition_attempts() >= attempts_before + 3);
  service.stop();
}

TEST_CASE("speech failure remains fenced until its terminal state persists",
          "[speech][service][failure][persistence][retry]") {
  sanguinius::test::FakeSpeechRepository repository;
  sanguinius::test::FakeClock clock;
  clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = "session",
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      repository,
      nullptr,
      normalizer,
      cache,
      gateway,
      clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(false)};
  service.start();
  service.session_ready("session", "10", false, false);
  repository.fail_next_transitions(2);
  REQUIRE(service
              .say("session", "10", "This request must fail safely.",
                   "speech:interaction:terminal-retry", 1'000)
              .status == SpeechEnqueueStatus::accepted);

  bool failed{};
  for (std::size_t attempt = 0; attempt < 200 && !failed; ++attempt) {
    for (const auto &item : repository.items()) {
      if (item.source_kind == "direct_say") {
        failed = item.state == SpeechState::failed && !item.text.has_value();
        break;
      }
    }
    if (!failed)
      std::this_thread::sleep_for(5ms);
  }
  REQUIRE(failed);
  REQUIRE(repository.transition_attempts() >= 3);
  service.stop();
}

TEST_CASE("disabled provider emits one static and public-safe text fallback",
          "[speech][service][fallback][privacy]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  std::atomic<std::size_t> notices{};
  std::atomic<bool> nonce_present{};
  std::string notice;
  std::mutex notice_mutex;
  sanguinius::SpeechServiceConfiguration configuration;
  configuration.provider_enabled = false;
  configuration.queue_capacity = 16;
  sanguinius::SpeechService service{
      *fixture.speech,
      nullptr,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = std::move(clip)},
      configuration,
      [&notices, &nonce_present, &notice, &notice_mutex](std::string nonce,
                                                         std::string message) {
        nonce_present.store(!nonce.empty());
        const std::scoped_lock lock{notice_mutex};
        notice = std::move(message);
        ++notices;
      }};
  service.start();
  service.session_ready(fixture.session_id, "10", false, false);
  REQUIRE(service
              .say(fixture.session_id, "10", "Secret requested words.",
                   "speech:interaction:fallback", 300)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(gateway.wait_for_sends(1, 2s));
  REQUIRE(notices.load() == 1);
  REQUIRE(nonce_present.load());
  {
    const std::scoped_lock lock{notice_mutex};
    REQUIRE(notice.find("Secret requested words") == std::string::npos);
    REQUIRE(notice.find("provider") != std::string::npos);
  }
  REQUIRE(service.track_marker(fixture.session_id, gateway.marker()));
  REQUIRE(service
              .say(fixture.session_id, "10", "A second private line.",
                   "speech:interaction:fallback:second", 301)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(wait_for_count(notices, 2));
  REQUIRE(gateway.send_count() == 1);
  service.stop();
}

TEST_CASE(
    "speech playback rejection uses text fallback without static recursion",
    "[speech][service][fallback][gateway]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  gateway.set_send_result(sanguinius::VoiceGatewaySubmit::unavailable);
  RetryThenSucceedTts client;
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  std::atomic<std::size_t> notices{};
  sanguinius::SpeechService service{
      *fixture.speech,
      &client,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(true),
      [&notices](std::string, std::string) { ++notices; }};
  service.start();
  service.session_ready(fixture.session_id, "10", false, false);
  REQUIRE(service
              .say(fixture.session_id, "10", "Gateway rejection.",
                   "speech:interaction:gateway-rejection", 300)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(gateway.wait_for_sends(2, 2s));
  REQUIRE(wait_for_count(notices, 1));
  REQUIRE(gateway.send_count() == 2);
  REQUIRE(service.health().last_failure_category == "unavailable");
  service.stop();
}

TEST_CASE("speech mute immediately stops active automatic playback",
          "[speech][service][mute]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      *fixture.speech,
      nullptr,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(false)};
  service.start();
  service.session_ready(fixture.session_id, "10", false, true);
  REQUIRE(gateway.wait_for_sends(1, 2s));
  service.set_muted(fixture.session_id, true);
  REQUIRE(gateway.stop_audio_count() == 1);
  REQUIRE(service.health().repository.playing == 0);
  service.stop();
}

TEST_CASE("speech mute cancels automatic synthesis without worker delay",
          "[speech][service][mute][concurrency]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  cache.block_writes();
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      *fixture.speech,
      nullptr,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(false)};
  service.start();
  service.session_ready(fixture.session_id, "10", false, true);
  REQUIRE(cache.wait_for_write(2s));
  REQUIRE(service.health().repository.synthesizing == 1);

  service.set_muted(fixture.session_id, true);
  REQUIRE(service.health().repository.synthesizing == 0);
  REQUIRE(gateway.send_count() == 0);

  cache.release_writes();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline &&
         service.health().synthesis_worker.active != 0)
    std::this_thread::sleep_for(10ms);
  REQUIRE(gateway.send_count() == 0);
  REQUIRE(service.health().synthesis_worker.active == 0);
  REQUIRE(service.health().repository.playing == 0);
  service.stop();
}

TEST_CASE("speech expiring during normalization is never submitted",
          "[speech][service][expiry][concurrency]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  RetryThenSucceedTts client;
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  cache.block_writes();
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  sanguinius::SpeechService service{
      *fixture.speech,
      &client,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(true)};
  service.start();
  service.session_ready(fixture.session_id, "10", false, false);
  REQUIRE(service
              .say(fixture.session_id, "10", "This line will expire.",
                   "speech:interaction:expires", 1'000)
              .status == SpeechEnqueueStatus::accepted);
  REQUIRE(cache.wait_for_write(2s));
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{122}});
  cache.release_writes();

  bool expired{};
  for (std::size_t attempt = 0; attempt < 200 && !expired; ++attempt) {
    const auto health = service.health();
    expired = health.repository.synthesizing == 0 &&
              health.repository.ready == 0 && health.repository.playing == 0;
    if (!expired)
      std::this_thread::sleep_for(5ms);
  }
  REQUIRE(expired);
  REQUIRE(gateway.send_count() == 0);
  auto stored = fixture.context->connection().prepare(
      "SELECT state,text FROM speech_item WHERE deduplication_key=?");
  stored.bind(1, "speech:interaction:expires");
  REQUIRE(stored.step());
  REQUIRE(stored.column_text(0) == "expired");
  REQUIRE(stored.column_is_null(1));
  service.stop();
}

TEST_CASE("speech test scenarios inject sanitized production failures",
          "[speech][service][test-mode][fallback]") {
  SpeechFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  sanguinius::test::FakeVoiceGateway gateway;
  gateway.start([](sanguinius::VoiceEvent) {});
  REQUIRE(gateway.connect({.session_id = fixture.session_id,
                           .guild_id = 10,
                           .channel_id = 40,
                           .member_user_id = 31,
                           .generation = 1,
                           .validate_member_channel = true}) ==
          sanguinius::VoiceGatewaySubmit::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  sanguinius::test::FakeAudioNormalizer normalizer;
  sanguinius::test::FakeTtsCache cache;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto clip = sanguinius::make_vox_proof_chime();
  std::atomic<std::size_t> notices{};
  sanguinius::SpeechService service{
      *fixture.speech,
      nullptr,
      normalizer,
      cache,
      gateway,
      fixture.clock,
      ids,
      diagnostics,
      {.entrance = clip, .error = clip, .farewell = clip},
      speech_configuration(false),
      [&notices](std::string, std::string) { ++notices; }};
  service.start();
  service.session_ready(fixture.session_id, "10", false, false);
  REQUIRE(service.run_test_scenario(fixture.session_id, "10",
                                    "provider-failure", "interaction:first"));
  REQUIRE(gateway.wait_for_sends(1, 2s));
  REQUIRE(wait_for_count(notices, 1));
  REQUIRE(service.health().last_failure_category == "provider_unavailable");
  REQUIRE(service.health().repository.usage.rolling_day_attempts == 0);
  REQUIRE(service.track_marker(fixture.session_id, gateway.marker()));

  REQUIRE(service.run_test_scenario(fixture.session_id, "10",
                                    "provider-failure", "interaction:first"));
  std::this_thread::sleep_for(50ms);
  REQUIRE(notices.load() == 1);
  REQUIRE(gateway.send_count() == 1);

  REQUIRE(service.run_test_scenario(fixture.session_id, "10",
                                    "provider-failure", "interaction:second"));
  REQUIRE(wait_for_count(notices, 2));
  REQUIRE(gateway.send_count() == 1);

  REQUIRE(service.run_test_scenario(fixture.session_id, "10", "budget-limit",
                                    "interaction:third"));
  REQUIRE(gateway.wait_for_sends(2, 2s));
  REQUIRE(wait_for_count(notices, 3));
  REQUIRE(service.health().last_failure_category == "budget_exhausted");
  REQUIRE(service.health().repository.usage.rolling_day_attempts == 0);
  auto audited = fixture.context->connection().prepare(
      "SELECT COUNT(*),COUNT(DISTINCT source_event_id) FROM speech_item WHERE "
      "source_kind IN ('test_provider_failure','test_budget_limit')");
  REQUIRE(audited.step());
  REQUIRE(audited.column_int64(0) == 3);
  REQUIRE(audited.column_int64(1) == 3);
  service.stop();
}
