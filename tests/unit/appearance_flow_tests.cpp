#include "sanguinius/appearances.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] sanguinius::AppearancePolicy policy() {
  const auto path = std::filesystem::path{__FILE__}
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "config/appearance-policy-v1.json";
  std::ifstream stream{path};
  REQUIRE(stream.good());
  return sanguinius::parse_appearance_policy(
      std::string{std::istreambuf_iterator<char>{stream},
                  std::istreambuf_iterator<char>{}});
}

class RecordingRepository final : public sanguinius::AppearanceRepository {
public:
  void register_policy(const sanguinius::AppearancePolicy &,
                       std::int64_t) override {
    registered = true;
  }
  void activate_mode(const sanguinius::AppearanceMode mode,
                     std::int64_t) override {
    active_mode = mode;
    ++mode_activation_count;
  }
  sanguinius::AppearancePolicy load_policy(std::string_view version) override {
    loaded_policy_version = std::string{version};
    return recovered_policy.value_or(policy());
  }
  std::size_t abandon_prior_instance_attempts(
      std::string_view, std::int64_t,
      sanguinius::PersistentIdGenerator &) override {
    return 0;
  }
  bool set_callback_consent(sanguinius::DiscordSnowflake, bool, std::int64_t,
                            std::string, std::string, std::string) override {
    return true;
  }
  std::optional<sanguinius::AppearanceCandidate>
  observe_message(const sanguinius::AppearancePolicy &,
                  const sanguinius::AppearanceMessageObservation &, std::string,
                  std::string) override {
    return std::nullopt;
  }
  sanguinius::AppearanceCandidate
  simulate(const sanguinius::AppearancePolicy &policy_value,
           const sanguinius::AppearanceSimulationRequest &request) override {
    sanguinius::AppearanceCandidate candidate{};
    candidate.candidate_id = request.candidate_id;
    candidate.policy_version =
        simulated_policy_version.value_or(policy_value.policy_version);
    candidate.type = sanguinius::AppearanceCandidateType::simulation;
    candidate.created_at_ms = request.now_ms;
    candidate.expires_at_ms =
        request.now_ms + policy_value.candidate_expiry_ms.at("simulation");
    candidate.actors = {request.owner_user_id};
    candidate.excerpts = {"Synthetic lively banter."};
    candidate.safe_summary =
        "Owner simulation fixture: " + request.fixture + ".";
    candidate.owner_simulation = true;
    candidate.alternating_turns = true;
    candidate.recurrence_matches = 2;
    candidate.human_messages_since_bot = 8;
    return candidate;
  }
  std::vector<sanguinius::AppearanceCandidate>
  scan_events(const sanguinius::AppearancePolicy &, std::int64_t,
              std::string_view) override {
    ++scan_count;
    return {};
  }
  bool record_final(const sanguinius::AppearancePolicy &,
                    sanguinius::AppearanceMode,
                    const sanguinius::AppearanceCandidate &candidate,
                    const sanguinius::AppearanceEvaluation &evaluation,
                    std::string, std::string, std::string_view,
                    std::string status_value,
                    std::optional<sanguinius::AppearanceModelResult>,
                    std::int64_t) override {
    finish(std::move(status_value), evaluation.reason, candidate.candidate_id);
    return true;
  }
  bool prepare_model(const sanguinius::AppearancePolicy &,
                     sanguinius::AppearanceMode,
                     const sanguinius::AppearanceCandidate &,
                     const sanguinius::AppearanceEvaluation &, std::string,
                     std::string, std::string_view, std::int64_t) override {
    prepared = true;
    return true;
  }
  bool complete_model(const sanguinius::AppearancePolicy &,
                      sanguinius::AppearanceMode,
                      const sanguinius::AppearanceCandidate &candidate,
                      const sanguinius::AppearanceEvaluation &evaluation,
                      std::string_view, std::string, std::string status_value,
                      std::optional<sanguinius::AppearanceModelResult> result,
                      std::int64_t) override {
    if (fail_next_completion.exchange(false))
      throw std::runtime_error{"injected completion failure"};
    finish(std::move(status_value),
           result && result->should_speak && !result->serious_context &&
                   evaluation.eligible_for_model
               ? "hypothetical"
               : "reject",
           candidate.candidate_id);
    return true;
  }
  std::optional<sanguinius::AppearanceDecisionRecord>
  decision(std::string_view) override {
    return shown_decision;
  }
  std::vector<sanguinius::AppearanceDecisionRecord>
  recent(std::size_t) override {
    return recent_decisions;
  }
  std::size_t public_outbox_violation_count() override { return 0; }
  void purge(const sanguinius::AppearancePolicy &, std::int64_t) override {}

  [[nodiscard]] bool wait(const std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, timeout, [this] { return completed; });
  }
  [[nodiscard]] std::string status() const {
    const std::scoped_lock lock{mutex};
    return model_status;
  }
  [[nodiscard]] std::string action() const {
    const std::scoped_lock lock{mutex};
    return final_action;
  }
  [[nodiscard]] std::string candidate_id() const {
    const std::scoped_lock lock{mutex};
    return completed_candidate_id;
  }

  bool registered{};
  bool prepared{};
  sanguinius::AppearanceMode active_mode{sanguinius::AppearanceMode::off};
  std::atomic_bool fail_next_completion{};
  std::atomic_size_t mode_activation_count{};
  std::atomic_size_t scan_count{};
  std::optional<sanguinius::AppearanceDecisionRecord> shown_decision;
  std::vector<sanguinius::AppearanceDecisionRecord> recent_decisions;
  std::optional<std::string> simulated_policy_version;
  std::optional<sanguinius::AppearancePolicy> recovered_policy;
  std::string loaded_policy_version;

private:
  void finish(std::string status_value, std::string action_value,
              std::string candidate_id_value) {
    {
      const std::scoped_lock lock{mutex};
      model_status = std::move(status_value);
      final_action = std::move(action_value);
      completed_candidate_id = std::move(candidate_id_value);
      completed = true;
    }
    changed.notify_all();
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  bool completed{};
  std::string model_status;
  std::string final_action;
  std::string completed_candidate_id;
};

enum class AiBehavior { response, refusal, incomplete, timeout, wait_for_stop };

class ScriptedAi final : public sanguinius::AiClient {
public:
  ScriptedAi(AiBehavior behavior_value, std::string response_value = {})
      : behavior{behavior_value}, response{std::move(response_value)} {}

  std::string generate(const sanguinius::AiRequest &,
                       const std::stop_token stop) const override {
    entered.store(true);
    if (behavior == AiBehavior::refusal)
      throw sanguinius::AiRefusal{};
    if (behavior == AiBehavior::incomplete)
      throw sanguinius::AiIncompleteResponse{};
    if (behavior == AiBehavior::timeout)
      throw std::runtime_error{"request timeout"};
    if (behavior == AiBehavior::wait_for_stop) {
      while (!stop.stop_requested())
        std::this_thread::yield();
      throw sanguinius::OperationCancelled{};
    }
    return response;
  }

  AiBehavior behavior;
  std::string response;
  mutable std::atomic<bool> entered{false};
};

[[nodiscard]] sanguinius::AppearanceSimulationRequest request() {
  return {.fixture = "lively_game_night_banter",
          .idempotency_key = "flow",
          .correlation_id = "flow",
          .owner_user_id = 30,
          .now_ms = 1'000,
          .candidate_id = {},
          .event_id = {}};
}

[[nodiscard]] std::vector<std::string> ids() {
  std::vector<std::string> result;
  for (std::size_t value = 1; value <= 24; ++value) {
    auto suffix = std::to_string(value);
    suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
    result.push_back("00000000-0000-4000-8000-" + suffix);
  }
  return result;
}

} // namespace

TEST_CASE("appearance model failures are single-attempt audited rejections",
          "[appearance][flow][model-failure]") {
  const std::vector<std::pair<AiBehavior, std::string>> cases{
      {AiBehavior::refusal, "model_refusal"},
      {AiBehavior::incomplete, "model_incomplete"},
      {AiBehavior::timeout, "model_timeout"},
      {AiBehavior::response, "model_invalid_json"}};
  for (const auto &[behavior, expected] : cases) {
    CAPTURE(expected);
    RecordingRepository repository;
    sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
    sanguinius::test::FakePersistentIdGenerator generator{ids()};
    sanguinius::test::FakeDiagnostics diagnostics;
    ScriptedAi ai{behavior, "not-json"};
    sanguinius::AiWorkService work{4, 1};
    work.start();
    sanguinius::AppearanceService service{repository,
                                          clock,
                                          generator,
                                          policy(),
                                          sanguinius::AppearanceMode::dry_run,
                                          "instance",
                                          &ai,
                                          &work,
                                          diagnostics};
    service.start();
    static_cast<void>(service.simulate(request()));
    REQUIRE(repository.wait(2s));
    work.stop();
    REQUIRE(repository.registered);
    REQUIRE(repository.prepared);
    REQUIRE(repository.status() == expected);
    REQUIRE(repository.action() == "reject");
    REQUIRE(repository.public_outbox_violation_count() == 0);
  }
}

TEST_CASE(
    "appearance model success produces only a hypothetical preview decision",
    "[appearance][flow]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  ScriptedAi ai{
      AiBehavior::response,
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"A fine evening for victory.","tone":"warm","memory_ids_used":[],"confidence":0.93})"};
  sanguinius::AiWorkService work{4, 1};
  work.start();
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        &ai,
                                        &work,
                                        diagnostics};
  service.start();
  static_cast<void>(service.simulate(request()));
  REQUIRE(repository.wait(2s));
  work.stop();
  REQUIRE(repository.status() == "model_accepted");
  REQUIRE(repository.action() == "hypothetical");
  REQUIRE(repository.public_outbox_violation_count() == 0);
}

TEST_CASE("appearance completion failure becomes an audited rejection",
          "[appearance][flow][model-failure][persistence]") {
  RecordingRepository repository;
  repository.fail_next_completion = true;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  ScriptedAi ai{
      AiBehavior::response,
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"A fine evening for victory.","tone":"warm","memory_ids_used":[],"confidence":0.93})"};
  sanguinius::AiWorkService work{4, 1};
  work.start();
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        &ai,
                                        &work,
                                        diagnostics};
  service.start();
  static_cast<void>(service.simulate(request()));
  REQUIRE(repository.wait(2s));
  work.stop();
  REQUIRE(repository.status() == "model_completion_failure");
  REQUIRE(repository.action() == "reject");
  REQUIRE(diagnostics.contains_category("appearance.model_completion"));
  REQUIRE(repository.public_outbox_violation_count() == 0);
}

TEST_CASE("appearance queue saturation rejects without fallback prose",
          "[appearance][flow][model-failure]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  ScriptedAi ai{AiBehavior::response, "unused"};
  sanguinius::AiWorkService stopped_work{1, 1};
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        &ai,
                                        &stopped_work,
                                        diagnostics};
  service.start();
  static_cast<void>(service.simulate(request()));
  REQUIRE(repository.wait(100ms));
  REQUIRE(repository.status() == "model_queue_saturated");
  REQUIRE(repository.action() == "reject");
  REQUIRE_FALSE(repository.candidate_id().empty());
  REQUIRE_FALSE(ai.entered.load());
}

TEST_CASE("appearance shutdown cancels an active attempt exactly once",
          "[appearance][flow][shutdown]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  ScriptedAi ai{AiBehavior::wait_for_stop};
  sanguinius::AiWorkService work{1, 1};
  work.start();
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        &ai,
                                        &work,
                                        diagnostics};
  service.start();
  static_cast<void>(service.simulate(request()));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!ai.entered.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(ai.entered.load());
  work.stop();
  REQUIRE(repository.wait(100ms));
  REQUIRE(repository.status() == "model_cancelled");
  REQUIRE(repository.action() == "reject");
}

TEST_CASE("appearance owner preview is UTF-8 safe and Discord bounded",
          "[appearance][flow][preview][bounds]") {
  RecordingRepository repository;
  std::string prose;
  for (std::size_t index = 0; index < 500; ++index)
    prose += "\xF0\x9F\xA9\xB8";
  repository.shown_decision = sanguinius::AppearanceDecisionRecord{
      .decision_id = "00000000-0000-4000-8000-000000000901",
      .candidate_id = "00000000-0000-4000-8000-000000000902",
      .policy_version = "m9-initial-1",
      .candidate_type = "conversation",
      .safe_summary = "Conversation activity across eight bounded messages.",
      .state = "final",
      .action = "hypothetical",
      .reason = "hypothetical",
      .score = 75,
      .model_status = "model_accepted",
      .preview = prose,
      .created_at_ms = 1'000,
      .gates = {{"source_enabled", true}, {"callback_consent", true}},
      .score_components = {{"relevance", 20}},
      .memory_ids = {},
      .serious_categories = {}};
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        nullptr,
                                        nullptr,
                                        diagnostics};
  const auto rendered = service.preview(repository.shown_decision->decision_id);
  REQUIRE(rendered.size() <= 1'900);
  REQUIRE(sanguinius::valid_utf8(rendered));
  REQUIRE(rendered.find("callback_consent") != std::string::npos);
  REQUIRE(rendered.find("Policy: m9-initial-1") != std::string::npos);
  REQUIRE(rendered.find("Conversation activity") != std::string::npos);
}

TEST_CASE("appearance recent renders references accepted by preview",
          "[appearance][flow][recent]") {
  RecordingRepository repository;
  repository.recent_decisions.push_back(
      {.decision_id = "00000000-0000-4000-8000-000000000911",
       .candidate_id = "00000000-0000-4000-8000-000000000912",
       .policy_version = "m9-initial-1",
       .candidate_type = "conversation",
       .safe_summary = "Conversation activity.",
       .state = "final",
       .action = "reject",
       .reason = "score_below_threshold",
       .score = 45,
       .model_status = "not_requested",
       .preview = std::nullopt,
       .created_at_ms = 1'000,
       .gates = {},
       .score_components = {},
       .memory_ids = {},
       .serious_categories = {}});
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        nullptr,
                                        nullptr,
                                        diagnostics};
  const auto rendered = service.recent();
  REQUIRE(rendered.find(repository.recent_decisions.front().decision_id) !=
          std::string::npos);
  REQUIRE(rendered.find("...") == std::string::npos);
}

TEST_CASE("recovered appearance candidates use their immutable policy snapshot",
          "[appearance][flow][policy][restart]") {
  RecordingRepository repository;
  auto old_policy = policy();
  old_policy.policy_version = "m9-old-policy";
  old_policy.score_threshold = 100;
  repository.simulated_policy_version = old_policy.policy_version;
  repository.recovered_policy = old_policy;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::AppearanceService service{repository,
                                        clock,
                                        generator,
                                        policy(),
                                        sanguinius::AppearanceMode::dry_run,
                                        "instance",
                                        nullptr,
                                        nullptr,
                                        diagnostics};
  service.start();
  static_cast<void>(service.simulate(request()));
  REQUIRE(repository.wait(100ms));
  REQUIRE(repository.loaded_policy_version == "m9-old-policy");
  REQUIRE(repository.action() == "score_below_threshold");
  REQUIRE_FALSE(repository.prepared);
}

TEST_CASE("degraded runtime rejects before an appearance model attempt",
          "[appearance][flow][runtime]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::AppearanceService service{
      repository,
      clock,
      generator,
      policy(),
      sanguinius::AppearanceMode::dry_run,
      "instance",
      nullptr,
      nullptr,
      diagnostics,
      "persona",
      "America/New_York",
      [] {
        return sanguinius::AppearanceRuntimeState{.operational = false,
                                                  .degraded = true};
      }};
  service.start();
  static_cast<void>(service.simulate(request()));
  REQUIRE(repository.wait(100ms));
  REQUIRE(repository.action() == "operational");
  REQUIRE_FALSE(repository.prepared);
}

TEST_CASE("appearance event scans wait for runtime readiness",
          "[appearance][flow][runtime][restart]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  bool ready{};
  sanguinius::AppearanceService service{
      repository,
      clock,
      generator,
      policy(),
      sanguinius::AppearanceMode::dry_run,
      "instance",
      nullptr,
      nullptr,
      diagnostics,
      "persona",
      "America/New_York",
      [&ready] {
        return sanguinius::AppearanceRuntimeState{.operational = ready,
                                                  .degraded = !ready};
      }};
  service.start();

  REQUIRE(repository.active_mode == sanguinius::AppearanceMode::dry_run);
  REQUIRE(repository.mode_activation_count.load() == 1);
  REQUIRE_FALSE(service.scan_events());
  REQUIRE(repository.scan_count.load() == 0);
  ready = true;
  REQUIRE(service.scan_events());
  REQUIRE(repository.scan_count.load() == 1);
}

TEST_CASE("appearance event scans are healthy no-ops while off",
          "[appearance][flow][runtime]") {
  RecordingRepository repository;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1s}};
  sanguinius::test::FakePersistentIdGenerator generator{ids()};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::AppearanceService service{
      repository,
      clock,
      generator,
      policy(),
      sanguinius::AppearanceMode::off,
      "instance",
      nullptr,
      nullptr,
      diagnostics,
      "persona",
      "America/New_York",
      [] {
        return sanguinius::AppearanceRuntimeState{.operational = false,
                                                  .degraded = true};
      }};
  service.start();

  REQUIRE(repository.active_mode == sanguinius::AppearanceMode::off);
  REQUIRE(repository.mode_activation_count.load() == 1);
  REQUIRE(service.scan_events());
  REQUIRE(repository.mode_activation_count.load() == 2);
  REQUIRE(repository.scan_count.load() == 0);
}
