#include "sanguinius/ai_generation.hpp"
#include "sanguinius/cross_feature_orchestrator.hpp"
#include "sanguinius/presentation.hpp"
#include "sanguinius/provider_circuit.hpp"
#include "sanguinius/retention.hpp"
#include "sanguinius/sanguinius_overview.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_speech.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct CircuitTrace {
  std::size_t succeeded{};
  std::size_t failed{};
  sanguinius::ProviderCircuitFailure last_failure{
      sanguinius::ProviderCircuitFailure::ignored};
};

class RecordingCircuitRepository final
    : public sanguinius::ProviderCircuitRepository {
public:
  explicit RecordingCircuitRepository(CircuitTrace &trace) : trace_{trace} {}
  void restart(std::string_view, std::int64_t, std::string) override {}
  bool admit(std::string_view, std::int64_t, std::string) override {
    return true;
  }
  std::string state(std::string_view) const override { return "closed"; }
  void succeeded(std::string_view, std::int64_t, std::string) override {
    ++trace_.succeeded;
  }
  void failed(std::string_view, sanguinius::ProviderCircuitFailure failure,
              std::string_view, std::int64_t, std::string) override {
    ++trace_.failed;
    trace_.last_failure = failure;
  }

private:
  CircuitTrace &trace_;
};

struct RetentionTrace {
  bool schedule_ensured{};
  bool ran{};
  sanguinius::RetentionCounts initial;
};

class RecordingRetentionRepository final
    : public sanguinius::RetentionRepository {
public:
  explicit RecordingRetentionRepository(RetentionTrace &trace)
      : trace_{trace} {}

  void ensure_schedule(std::int64_t, std::string) override {
    trace_.schedule_ensured = true;
  }

  sanguinius::RetentionCounts
  run(std::int64_t, std::string,
      sanguinius::RetentionCounts initial = {}) override {
    trace_.ran = true;
    trace_.initial = initial;
    return initial;
  }

private:
  RetentionTrace &trace_;
};

class HeaderOnlyTts final : public sanguinius::TextToSpeechClient {
public:
  sanguinius::SynthesizedAudio synthesize(const sanguinius::TtsRequest &,
                                          std::stop_token) const override {
    return {.bytes = {std::byte{'R'}, std::byte{'I'}, std::byte{'F'},
                      std::byte{'F'}, std::byte{0}, std::byte{0}, std::byte{0},
                      std::byte{0}, std::byte{'W'}, std::byte{'A'},
                      std::byte{'V'}, std::byte{'E'}},
            .format = sanguinius::AudioFormat::wav,
            .content_type = "audio/wav",
            .provider_request_id = "test-request"};
  }
};

class AmbiguousTtsFailure final : public sanguinius::TextToSpeechClient {
public:
  explicit AmbiguousTtsFailure(const sanguinius::TtsFailureCategory category)
      : category_{category} {}

  sanguinius::SynthesizedAudio synthesize(const sanguinius::TtsRequest &,
                                          std::stop_token) const override {
    throw sanguinius::TtsError{
        category_, "TTS request failed after a response began.", false};
  }

private:
  sanguinius::TtsFailureCategory category_;
};

} // namespace

TEST_CASE("AI worst-case cost arithmetic is conservative and bounded",
          "[m18][ai][budget]") {
  const sanguinius::AiGenerationPolicy policy;
  REQUIRE(policy.rolling_day_micro_usd == 1'250'000);
  REQUIRE(policy.calendar_month_micro_usd == 25'000'000);
  REQUIRE(policy.rolling_day_generations == 300);
  REQUIRE(policy.direct_user_ten_minute_generations == 30);
  REQUIRE(sanguinius::estimated_ai_cost_micro_usd(16'000, 500, 1'000'000,
                                                  2'000'000) == 17'000);
  REQUIRE(sanguinius::estimated_ai_cost_micro_usd(1, 1, 1, 1) == 2);
  REQUIRE_THROWS_AS(
      sanguinius::estimated_ai_cost_micro_usd(16'001, 1, 1'000'000, 1'000'000),
      std::invalid_argument);
  REQUIRE_THROWS_AS(
      sanguinius::estimated_ai_cost_micro_usd(1, 501, 1'000'000, 1'000'000),
      std::invalid_argument);
}

TEST_CASE("AI provider request identifiers are strictly sanitized",
          "[m18][ai][provider][privacy]") {
  REQUIRE(sanguinius::sanitize_ai_provider_request_id("req_123-safe.4") ==
          "req_123-safe.4");
  REQUIRE(sanguinius::sanitize_ai_provider_request_id("unsafe value").empty());
  REQUIRE(sanguinius::sanitize_ai_provider_request_id(std::string(129, 'a'))
              .empty());
  const sanguinius::AiProviderError error{
      sanguinius::AiProviderErrorCategory::timeout, "unsafe value"};
  REQUIRE(error.provider_request_id().empty());
}

TEST_CASE("TTS circuit closes only after normalized media validation",
          "[m18][tts][provider][circuit][media]") {
  CircuitTrace trace;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::CircuitBreakingTextToSpeechClient client{
      std::make_unique<HeaderOnlyTts>(),
      std::make_unique<RecordingCircuitRepository>(trace), clock, ids};
  static_cast<void>(client.synthesize({.text = "A test line."}, {}));
  REQUIRE(trace.succeeded == 0);
  client.provider_response_rejected(
      sanguinius::TtsFailureCategory::decoder_failed);
  REQUIRE(trace.failed == 1);
  REQUIRE(trace.last_failure == sanguinius::ProviderCircuitFailure::retryable);
}

TEST_CASE("ambiguous TTS transport failures count toward the provider circuit",
          "[m18][tts][provider][circuit][timeout]") {
  for (const auto category : {sanguinius::TtsFailureCategory::timeout,
                              sanguinius::TtsFailureCategory::transport}) {
    CircuitTrace trace;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::CircuitBreakingTextToSpeechClient client{
        std::make_unique<AmbiguousTtsFailure>(category),
        std::make_unique<RecordingCircuitRepository>(trace), clock, ids};

    try {
      static_cast<void>(client.synthesize({.text = "A test line."}, {}));
      FAIL("Expected the provider failure to be propagated.");
    } catch (const sanguinius::TtsError &error) {
      REQUIRE_FALSE(error.retryable());
    }
    REQUIRE(trace.failed == 1);
    REQUIRE(trace.last_failure ==
            sanguinius::ProviderCircuitFailure::retryable);
  }
}

TEST_CASE("one orchestration consumer failure does not block later consumers",
          "[m18][orchestration][failure]") {
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::CrossFeatureOrchestrator orchestrator{diagnostics, 10ms};
  std::atomic<std::size_t> completed{};
  orchestrator.add_consumer(
      "failed", []() -> bool { throw std::runtime_error{"injected"}; });
  orchestrator.add_consumer("later", [&] {
    ++completed;
    return false;
  });
  orchestrator.start();
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (completed.load() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  orchestrator.stop();
  REQUIRE(completed.load() >= 1);
  REQUIRE(diagnostics.contains_category("cross_feature.failed"));
  const auto health = orchestrator.health();
  const auto failed =
      std::ranges::find(health.consumers, std::string_view{"failed"},
                        &sanguinius::CrossFeatureConsumerHealth::name);
  REQUIRE(failed != health.consumers.end());
  REQUIRE(failed->degraded);
  REQUIRE(failed->backlog);
  REQUIRE(failed->failures >= 1);
}

TEST_CASE("retention schedule selects the next 0400 UTC boundary",
          "[m18][retention]") {
  using namespace std::chrono;
  const auto day = sys_days{year{2026} / August / 27};
  const auto at_three =
      duration_cast<milliseconds>((day + hours{3}).time_since_epoch()).count();
  const auto at_four =
      duration_cast<milliseconds>((day + hours{4}).time_since_epoch()).count();
  REQUIRE(sanguinius::RetentionService::next_due_utc(at_three) == at_four);
  REQUIRE(sanguinius::RetentionService::next_due_utc(at_four) ==
          at_four + duration_cast<milliseconds>(days{1}).count());
}

TEST_CASE("daily retention reconciles the TTS cache without a Vox service",
          "[m18][retention][tts][cache]") {
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{10s}};
  sanguinius::test::FakePersistentIdGenerator ids{
      {"00000000-0000-4000-8000-000000000401",
       "00000000-0000-4000-8000-000000000402"}};
  sanguinius::test::FakeSpeechRepository speech;
  sanguinius::test::FakeTtsCache cache;
  const std::string key(64, 'a');
  static_cast<void>(cache.write(key, {.samples = {0, 0}}));
  cache.set_purge_removed_keys({key});
  RetentionTrace trace;
  sanguinius::RetentionService service{
      std::make_unique<RecordingRetentionRepository>(trace), clock, ids,
      &speech, &cache};

  REQUIRE(trace.schedule_ensured);
  REQUIRE(cache.health().entries == 1);
  const auto counts = service.run();
  REQUIRE(trace.ran);
  REQUIRE(cache.health().entries == 0);
  REQUIRE(counts.tts_cache_removals == 1);
  REQUIRE(counts.tts_cache_failures == 0);
}

TEST_CASE("TTS cache retention failures do not block database retention",
          "[m18][retention][tts][cache][failure]") {
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{10s}};
  sanguinius::test::FakePersistentIdGenerator ids{
      {"00000000-0000-4000-8000-000000000403",
       "00000000-0000-4000-8000-000000000404"}};
  sanguinius::test::FakeSpeechRepository speech;
  sanguinius::test::FakeTtsCache cache;
  cache.fail_purge();
  RetentionTrace trace;
  sanguinius::RetentionService service{
      std::make_unique<RecordingRetentionRepository>(trace), clock, ids,
      &speech, &cache};

  const auto counts = service.run();
  REQUIRE(trace.ran);
  REQUIRE(counts.tts_cache_removals == 0);
  REQUIRE(counts.tts_cache_failures == 1);
}

TEST_CASE("member status renders runtime controls and degraded providers",
          "[m18][status][runtime]") {
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{10s}};
  sanguinius::SanguiniusOverviewService overview{{.chronicle_enabled = true,
                                                  .tarot_enabled = true,
                                                  .vox_enabled = true,
                                                  .voice_input_enabled = true},
                                                 clock};
  sanguinius::UserPreferences preferences{};
  preferences.memory_callback_opt_in = true;
  const auto message =
      overview.status(2, "quiet", preferences,
                      {.text_ai = "degraded",
                       .tts = "disabled",
                       .vox_output = "disabled",
                       .voice_input = "unavailable",
                       .voice_consent_attested = std::nullopt});
  REQUIRE(message.embed.has_value());
  REQUIRE(message.embed->timestamp_ms == 10'000);
  const auto value = [&message](const std::string_view name) {
    const auto found = std::ranges::find(
        message.embed->fields, name, &sanguinius::EmbedPayload::Field::name);
    REQUIRE(found != message.embed->fields.end());
    return found->value;
  };
  REQUIRE(value("Text AI") == "degraded");
  REQUIRE(value("TTS") == "disabled");
  REQUIRE(value("Vox") == "disabled");
  REQUIRE(value("Listening") == "unavailable");
  REQUIRE(value("Chronicle callbacks") == "enabled");
  REQUIRE(value("Appearance callbacks") == "disabled");
}

TEST_CASE("privacy overview names member voice controls",
          "[m18][privacy][voice]") {
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{10s}};
  sanguinius::SanguiniusOverviewService overview{{.chronicle_enabled = true,
                                                  .tarot_enabled = true,
                                                  .vox_enabled = true,
                                                  .voice_input_enabled = true},
                                                 clock};
  const auto message = overview.privacy({}, 0, "ready", "ready",
                                        {.tts = "disabled",
                                         .vox_output = "disabled",
                                         .voice_input = "disabled",
                                         .voice_consent_attested = false});
  REQUIRE(message.embed.has_value());
  const auto voice =
      std::ranges::find(message.embed->fields, std::string_view{"Voice"},
                        &sanguinius::EmbedPayload::Field::name);
  REQUIRE(voice != message.embed->fields.end());
  REQUIRE(voice->value.find("/vox summon") != std::string::npos);
  REQUIRE(voice->value.find("/vox mute") != std::string::npos);
  REQUIRE(voice->value.find("/vox leave") != std::string::npos);
  REQUIRE(voice->value.find("/vox listen-start") != std::string::npos);
  REQUIRE(voice->value.find("/vox listen-stop") != std::string::npos);
  REQUIRE(voice->value.find("Output: disabled") != std::string::npos);
  REQUIRE(voice->value.find("Input: disabled") != std::string::npos);
  REQUIRE(voice->value.find("Guild consent: not attested") !=
          std::string::npos);
  const auto appearances =
      std::ranges::find(message.embed->fields, std::string_view{"Appearances"},
                        &sanguinius::EmbedPayload::Field::name);
  REQUIRE(appearances != message.embed->fields.end());
  REQUIRE(appearances->value.find("/sanguinius quiet for") !=
          std::string::npos);
  REQUIRE(appearances->value.find("/sanguinius quiet off") !=
          std::string::npos);
  REQUIRE(appearances->value.find("`/sanguinius quiet`.") == std::string::npos);
}

TEST_CASE(
    "shared presentation timestamps embeds and rejects duplicate controls",
    "[m18][presentation][components]") {
  const sanguinius::FeatureConfiguration features{.chronicle_enabled = true};
  const auto help = sanguinius::presentation::help("all", features, 12'000);
  const auto repository = sanguinius::presentation::repository(13'000);
  REQUIRE(help.embed->timestamp_ms == 12'000);
  REQUIRE(repository.embed->timestamp_ms == 13'000);

  sanguinius::InteractionMessage message{
      .content = {},
      .embed = std::nullopt,
      .buttons = {{.custom_id = "duplicate", .label = "Previous"},
                  {.custom_id = "duplicate", .label = "Next"}},
      .allowed_user_mentions = {}};
  REQUIRE_THROWS_AS(sanguinius::presentation::validate(message),
                    std::invalid_argument);
  message.buttons[0].custom_id =
      std::string{sanguinius::presentation::disabled_previous_custom_id};
  message.buttons[1].custom_id =
      std::string{sanguinius::presentation::disabled_next_custom_id};
  REQUIRE_NOTHROW(sanguinius::presentation::validate(message));
  REQUIRE(sanguinius::presentation::action_button_style("Submit") ==
          sanguinius::ButtonStyle::primary);
  REQUIRE(sanguinius::presentation::action_button_style("Edit") ==
          sanguinius::ButtonStyle::secondary);
  REQUIRE(sanguinius::presentation::action_button_style("Retract record") ==
          sanguinius::ButtonStyle::danger);
  REQUIRE(sanguinius::presentation::action_button_style("Consent to void") ==
          sanguinius::ButtonStyle::danger);
}

TEST_CASE("overview normalizes member state while retaining owner diagnostics",
          "[m18][status][privacy]") {
  sanguinius::test::FakeClock clock;
  sanguinius::SanguiniusOverviewService overview{{}, clock};
  const auto member = overview.status(
      0, "configured=live kill_switch=0", {},
      {.vox_output = "connecting", .voice_consent_attested = std::nullopt});
  const auto appearance =
      std::ranges::find(member.embed->fields, std::string_view{"Appearances"},
                        &sanguinius::EmbedPayload::Field::name);
  const auto vox =
      std::ranges::find(member.embed->fields, std::string_view{"Vox"},
                        &sanguinius::EmbedPayload::Field::name);
  REQUIRE(appearance->value == "unavailable");
  REQUIRE(vox->value == "unavailable");

  const auto health = overview.owner_health(
      "operational", "ready",
      {.vox_output = "degraded", .voice_consent_attested = std::nullopt},
      "configured=live kill_switch=0");
  REQUIRE(health.find("Member overview: appearances=ready") !=
          std::string::npos);
  REQUIRE(health.find("Appearances: configured=live kill_switch=0") !=
          std::string::npos);
}
