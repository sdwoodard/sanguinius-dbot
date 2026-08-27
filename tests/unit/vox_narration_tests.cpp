#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/vox_narration.hpp"

#include "support/fake_ai_client.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

[[nodiscard]] sanguinius::VoxNarrationCandidate
candidate(const sanguinius::VoxNarrationFeature feature,
          std::string safe_input = "A public event occurred.") {
  return {.intent_id = "00000000-0000-4000-8000-000000000001",
          .revision = 1,
          .source_event_id = "00000000-0000-4000-8000-000000000002",
          .event_type = "test.event.v1",
          .feature = feature,
          .guild_id = "10",
          .channel_id = "20",
          .safe_input = std::move(safe_input),
          .fallback_line = std::nullopt,
          .rank = 50,
          .created_at_ms = 1'000,
          .expires_at_ms = 121'000,
          .session_id = "00000000-0000-4000-8000-000000000003",
          .counterpart_outbox_id = "00000000-0000-4000-8000-000000000004",
          .counterpart_required = true,
          .is_test = false};
}

class SessionFlavorRepository final
    : public sanguinius::VoxNarrationRepository {
public:
  std::size_t
  observe_batch(const sanguinius::VoxNarrationObserveRequest &) override {
    ++observe_calls_;
    return 0;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  claim_next(const sanguinius::VoxNarrationClaimRequest &) override {
    return std::nullopt;
  }

  void
  complete_generation(const sanguinius::VoxNarrationCompletion &) override {}

  std::size_t reconcile(std::int64_t, const std::function<std::string()> &,
                        const std::function<bool(std::string_view)> & = {},
                        std::size_t = 50) override {
    return 0;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  preview(std::string_view, std::int64_t) override {
    return std::nullopt;
  }

  std::vector<sanguinius::VoxNarrationRecent> recent(std::size_t) override {
    return {};
  }

  sanguinius::VoxNarrationHealth health() override { return {}; }

  std::optional<std::string> session_flavor_context(std::string_view,
                                                    std::string_view,
                                                    std::string_view) override {
    ++context_calls_;
    return context_current_.load()
               ? std::optional<std::string>{"Summoner display name: Aster. "
                                            "Public featured title: "
                                            "Keeper."}
               : std::optional<std::string>{"Summoner display name: Aster."};
  }

  void revoke_context() noexcept { context_current_.store(false); }
  [[nodiscard]] std::size_t context_calls() const noexcept {
    return context_calls_.load();
  }
  [[nodiscard]] std::size_t observe_calls() const noexcept {
    return observe_calls_.load();
  }

private:
  std::atomic<bool> context_current_{true};
  std::atomic<std::size_t> context_calls_{};
  std::atomic<std::size_t> observe_calls_{};
};

class GenerationRepository final : public sanguinius::VoxNarrationRepository {
public:
  explicit GenerationRepository(sanguinius::VoxNarrationCandidate candidate)
      : candidate_{std::move(candidate)} {
    candidate_.revision = 2;
  }

  std::size_t
  observe_batch(const sanguinius::VoxNarrationObserveRequest &) override {
    return 0;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  claim_next(const sanguinius::VoxNarrationClaimRequest &) override {
    const std::scoped_lock lock{mutex_};
    if (claimed_)
      return std::nullopt;
    claimed_ = true;
    return candidate_;
  }

  std::optional<sanguinius::VoxNarrationCandidate> begin_generation(
      const sanguinius::VoxNarrationGenerationStartRequest &request) override {
    const std::scoped_lock lock{mutex_};
    if (request.intent_id != candidate_.intent_id ||
        request.expected_revision != candidate_.revision)
      return std::nullopt;
    ++candidate_.revision;
    return candidate_;
  }

  void complete_generation(
      const sanguinius::VoxNarrationCompletion &completion) override {
    {
      const std::scoped_lock lock{mutex_};
      completion_ = completion;
    }
    changed_.notify_all();
  }

  std::size_t reconcile(std::int64_t, const std::function<std::string()> &,
                        const std::function<bool(std::string_view)> & = {},
                        std::size_t = 50) override {
    return 0;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  preview(std::string_view, std::int64_t) override {
    return std::nullopt;
  }

  std::vector<sanguinius::VoxNarrationRecent> recent(std::size_t) override {
    return {};
  }

  sanguinius::VoxNarrationHealth health() override { return {}; }

  [[nodiscard]] std::optional<sanguinius::VoxNarrationCompletion>
  wait_for_completion(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    if (!changed_.wait_for(lock, timeout,
                           [this] { return completion_.has_value(); }))
      return std::nullopt;
    return completion_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  sanguinius::VoxNarrationCandidate candidate_;
  std::optional<sanguinius::VoxNarrationCompletion> completion_;
  bool claimed_{};
};

class PassBudgetRepository final : public sanguinius::VoxNarrationRepository {
public:
  std::size_t observe_batch(
      const sanguinius::VoxNarrationObserveRequest &request) override {
    observed = request.limit;
    return request.limit;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  claim_next(const sanguinius::VoxNarrationClaimRequest &) override {
    ++claims;
    return std::nullopt;
  }

  void
  complete_generation(const sanguinius::VoxNarrationCompletion &) override {}

  std::size_t reconcile(std::int64_t, const std::function<std::string()> &,
                        const std::function<bool(std::string_view)> & = {},
                        const std::size_t limit = 50) override {
    reconciled = limit;
    return limit;
  }

  std::optional<sanguinius::VoxNarrationCandidate>
  preview(std::string_view, std::int64_t) override {
    return std::nullopt;
  }

  std::vector<sanguinius::VoxNarrationRecent> recent(std::size_t) override {
    return {};
  }

  sanguinius::VoxNarrationHealth health() override { return {}; }

  std::size_t observed{};
  std::size_t reconciled{};
  std::size_t claims{};
};

} // namespace

TEST_CASE("Vox narration lifecycle has no feature-owned polling loop",
          "[vox][narration][orchestration][polling]") {
  SessionFlavorRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeAiClient ai;
  sanguinius::AiWorkService work{2, 1};
  sanguinius::VoxNarrationService service{
      repository,
      clock,
      ids,
      diagnostics,
      ai,
      work,
      "00000000-0000-4000-8000-000000000099",
      true,
      false};
  service.start();
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  REQUIRE(repository.observe_calls() == 0);
  REQUIRE_FALSE(service.run_one_cycle());
  REQUIRE(repository.observe_calls() == 1);
  service.stop();
}

TEST_CASE("Vox narration shares one fifty-record pass budget",
          "[vox][narration][orchestration][budget]") {
  PassBudgetRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeAiClient ai;
  sanguinius::AiWorkService work{2, 1};
  sanguinius::VoxNarrationService service{
      repository,
      clock,
      ids,
      diagnostics,
      ai,
      work,
      "00000000-0000-4000-8000-000000000099",
      true,
      false};

  REQUIRE(service.run_one_cycle());
  REQUIRE(repository.observed == 32);
  REQUIRE(repository.reconciled == 18);
  REQUIRE(repository.observed + repository.reconciled == 50);
  REQUIRE(repository.claims == 0);
}

TEST_CASE("Vox narration maps only approved public event families",
          "[vox][narration][policy]") {
  struct Expected {
    std::string_view event_type;
    sanguinius::VoxNarrationFeature feature;
    std::uint8_t rank;
    std::int64_t ttl_ms;
  };
  constexpr std::array expected{
      Expected{"chronicle.title_awarded.v1",
               sanguinius::VoxNarrationFeature::chronicle, 100, 120'000},
      Expected{"tarot.wager_resolved.v1",
               sanguinius::VoxNarrationFeature::tarot, 90, 120'000},
      Expected{"tarot.wager_voided.v1", sanguinius::VoxNarrationFeature::tarot,
               90, 120'000},
      Expected{"tarot.house_resolved.v1",
               sanguinius::VoxNarrationFeature::tarot, 90, 120'000},
      Expected{"tarot.house_voided.v1", sanguinius::VoxNarrationFeature::tarot,
               90, 120'000},
      Expected{"chronicle.session_completed.v1",
               sanguinius::VoxNarrationFeature::chronicle, 80, 120'000},
      Expected{"tarot.draw_created.v1", sanguinius::VoxNarrationFeature::tarot,
               70, 120'000},
      Expected{"chronicle.session_started.v1",
               sanguinius::VoxNarrationFeature::chronicle, 60, 120'000},
      Expected{"tarot.wager_funded.v1", sanguinius::VoxNarrationFeature::tarot,
               50, 120'000},
      Expected{"tarot.house_funded.v1", sanguinius::VoxNarrationFeature::tarot,
               50, 120'000},
      Expected{"appearance.live_queued.v1",
               sanguinius::VoxNarrationFeature::appearance, 40, 60'000},
  };

  for (const auto &item : expected) {
    CAPTURE(item.event_type);
    const auto policy = sanguinius::vox_narration_policy(item.event_type);
    REQUIRE(policy);
    CHECK(policy->feature == item.feature);
    CHECK(policy->rank == item.rank);
    CHECK(policy->ttl_ms == item.ttl_ms);
    CHECK(policy->counterpart_required);
  }
  CHECK_FALSE(sanguinius::vox_narration_policy("tarot.wager_offered.v1"));
  CHECK_FALSE(sanguinius::vox_narration_policy("relationship.changed.v1"));
  CHECK_FALSE(sanguinius::vox_narration_policy("voice.transcript.v1"));
}

TEST_CASE("Vox narration request is strict and contains only safe projection",
          "[vox][narration][privacy]") {
  const auto request = sanguinius::vox_narration_request(
      candidate(sanguinius::VoxNarrationFeature::chronicle,
                "Public active title: Keeper of the Watch. Recipient: Aster."));

  REQUIRE(request.json_schema);
  CHECK(request.json_schema->strict);
  CHECK(request.json_schema->name == "vox_narration_line");
  REQUIRE(request.conversation.size() == 1);
  CHECK(request.conversation.front().content.find("Keeper of the Watch") !=
        std::string::npos);
  CHECK(request.conversation.front().content.find("PRIVATE_CANARY") ==
        std::string::npos);
  CHECK(request.max_output_tokens == 96);
}

TEST_CASE("Vox narration rejects hostile or privacy-bearing model output",
          "[vox][narration][privacy]") {
  const auto chronicle = candidate(sanguinius::VoxNarrationFeature::chronicle);
  const auto tarot = candidate(sanguinius::VoxNarrationFeature::tarot);

  CHECK(sanguinius::parse_vox_narration_line(
      R"({"line":"The Chronicle remembers this worthy hour."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line("not json", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"A worthy hour.","extra":true})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The private balance remains sealed."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The pr\u200bivate ba\u200blance remains se\u200baled."})",
      chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The pr\u0301ivate balance remains sealed."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"A s\u00e9aled truth waits beyond the hall."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The \uff53\uff45\uff41\uff4c\uff45\uff44 truth waits."})",
      chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The Chronicle\u2060 remembers this hour."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The Chronicle\udb40\udc80 remembers this hour."})",
      chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"See https://example.invalid now."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"See HTTPS://EXAMPLE.INVALID now."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"See example.invalid now."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Follow discord.gg/example now."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Visit lore.example.invalid now."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Read data:text/plain,hello."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Write to mailto:postmaster."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"**The Chronicle remembers this hour.**"})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Hail <@123456789012345678>."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"One sentence. A second sentence."})", chronicle));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager moved 25 Fate."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \uff11\uff10."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \u0661\u0660."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \u2469."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \u00b2."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \u2169."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager settles at \u4e00."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wager moved twenty Fate."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"Ten coins change hands."})", tarot));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"A hundred credits are won."})", tarot));
  for (const auto dimension : {"esteem", "mirth", "reliability", "wariness"}) {
    CAPTURE(dimension);
    CHECK_FALSE(sanguinius::parse_vox_narration_line(
        std::string{"{\"line\":\"His "} + dimension +
            " rose after this event.\"}",
        chronicle));
  }
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      std::string{"{\"line\":\""} + std::string(161, 'a') + "\"}", chronicle));
}

TEST_CASE("Appearance companions reject copied spans and excessive overlap",
          "[vox][narration][appearance]") {
  CHECK(sanguinius::appearance_narration_too_similar(
      "one two three four five six seven eight nine ten",
      "before one two three four five six seven eight after"));
  CHECK(sanguinius::appearance_narration_too_similar(
      "one two three four five six seven eight nine ten",
      "one two three four stranger"));
  CHECK_FALSE(sanguinius::appearance_narration_too_similar(
      "one two three four five six seven eight nine ten",
      "one two three stranger elsewhere"));
  CHECK(sanguinius::appearance_narration_too_similar(
      "one two three four five six seven eight nine ten",
      "one\xE2\x80\x8B two three four five six seven eight nine ten"));
  CHECK(sanguinius::appearance_narration_too_similar(
      "one two three four five six seven eight nine ten",
      "on\xC3\xA9 tw\xC3\xB3 thr\xC3\xA9\x65 f\xC3\xB3ur "
      "f\xC3\xADve s\xC3\xADx sev\xC3\xA9n \xC3\xA9ight"));

  const auto appearance =
      candidate(sanguinius::VoxNarrationFeature::appearance,
                "The watch is long, but none of you keeps it alone tonight.");
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The watch is long but none of you keeps it alone."})",
      appearance));
  CHECK_FALSE(sanguinius::parse_vox_narration_line(
      R"({"line":"The wa\u200btch is long but none of you keeps it alone."})",
      appearance));
  CHECK(sanguinius::parse_vox_narration_line(
      R"({"line":"Even silence may carry the comfort of company."})",
      appearance));
}

TEST_CASE(
    "Appearance audit distinguishes duplication from invalid model output",
    "[vox][narration][appearance][audit]") {
  using namespace std::chrono_literals;
  const auto run_generation = [](std::string response, std::string safe_input) {
    GenerationRepository repository{candidate(
        sanguinius::VoxNarrationFeature::appearance, std::move(safe_input))};
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    sanguinius::test::FakeAiClient ai;
    ai.set_response(std::move(response));
    sanguinius::AiWorkService work{2, 1};
    work.start();
    sanguinius::VoxNarrationService service{
        repository,
        clock,
        ids,
        diagnostics,
        ai,
        work,
        "00000000-0000-4000-8000-000000000099",
        true,
        false};
    static_cast<void>(service.run_one_cycle());
    const auto completion = repository.wait_for_completion(2s);
    service.stop();
    work.stop();
    REQUIRE(completion);
    CHECK_FALSE(completion->line);
    return completion->model_status;
  };

  const std::string public_text{
      "The watch is long, but none of you keeps it alone tonight."};
  CHECK(run_generation("not json", public_text) ==
        sanguinius::VoxNarrationModelStatus::failed);
  CHECK(run_generation(R"({"line":"The private balance remains sealed."})",
                       public_text) ==
        sanguinius::VoxNarrationModelStatus::failed);
  CHECK(run_generation(
            R"({"line":"The watch is long but none of you keeps it alone."})",
            public_text) == sanguinius::VoxNarrationModelStatus::duplicate);
  CHECK(run_generation(
            R"({"line":"Even silence may carry the comfort of company."})",
            "The watch is long, but no one keeps the vigil al\xC3\xB3ne.") ==
        sanguinius::VoxNarrationModelStatus::failed);
}

TEST_CASE("Vox narration shutdown fences active session flavor callbacks",
          "[vox][narration][shutdown][concurrency]") {
  using namespace std::chrono_literals;
  SessionFlavorRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeAiClient ai;
  ai.set_response(
      R"({"entrance":"The company gathers once more.","farewell":"Until our next vigil."})");
  ai.block();
  sanguinius::AiWorkService work{2, 1};
  work.start();
  std::atomic<std::size_t> ready_count{};
  sanguinius::VoxNarrationService service{
      repository,
      clock,
      ids,
      diagnostics,
      ai,
      work,
      "00000000-0000-4000-8000-000000000099",
      true,
      false,
      {},
      [&ready_count](std::string, std::string, std::string, std::string) {
        ++ready_count;
      }};
  service.start();
  service.prepare_session_flavor("00000000-0000-4000-8000-000000000010", "10",
                                 "30");
  const auto entered = ai.wait_until_entered(1s);
  if (!entered) {
    ai.release();
    service.stop();
    work.stop();
  }
  REQUIRE(entered);

  auto stopped = std::async(std::launch::async, [&service] { service.stop(); });
  REQUIRE(stopped.wait_for(1s) == std::future_status::ready);
  CHECK(ai.cancelled());
  CHECK(ready_count.load() == 0);
  ai.release();
  work.stop();
}

TEST_CASE("Session flavor drops AI output when its public context changes",
          "[vox][narration][session][privacy][concurrency]") {
  using namespace std::chrono_literals;
  SessionFlavorRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeAiClient ai;
  ai.set_response(
      R"({"entrance":"Keeper Aster joins the company.","farewell":"Keeper Aster departs the vigil."})");
  ai.block();
  sanguinius::AiWorkService work{2, 1};
  work.start();
  std::atomic<std::size_t> ready_count{};
  sanguinius::VoxNarrationService service{
      repository,
      clock,
      ids,
      diagnostics,
      ai,
      work,
      "00000000-0000-4000-8000-000000000099",
      true,
      false,
      {},
      [&ready_count](std::string, std::string, std::string, std::string) {
        ++ready_count;
      }};
  service.start();
  service.prepare_session_flavor("00000000-0000-4000-8000-000000000010", "10",
                                 "30");
  REQUIRE(ai.wait_until_entered(1s));
  repository.revoke_context();
  ai.release();
  for (std::size_t attempt = 0; attempt < 100 && repository.context_calls() < 2;
       ++attempt)
    std::this_thread::sleep_for(5ms);
  REQUIRE(repository.context_calls() >= 2);
  CHECK(ready_count.load() == 0);
  service.stop();
  work.stop();
}
