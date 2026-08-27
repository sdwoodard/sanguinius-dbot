#include "support/application_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

[[nodiscard]] sanguinius::ApplicationOptions social_ai_options() {
  return sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {},
      .features = {.chronicle_enabled = true},
      .tarot_policy = {},
      .wager_policy = {},
      .tarot_house_policy = {.house_enabled = false,
                             .integration_enabled = false},
      .tarot_deck_catalog = std::nullopt,
      .tarot_house_catalog = std::nullopt,
      .build = {"test-version", "test-revision"},
      .persistence = {true, 5, 5, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
      .interaction_queue_capacity = 64,
      .durable_delivery_receipt_wait = std::chrono::milliseconds{100},
      .speech = {},
      .voice_input = {},
      .static_speech_assets = {},
  };
}

} // namespace

TEST_CASE("AI failure preserves public fallback and diagnostic category",
          "[application][failure]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.ai->fail("provider unavailable");
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> speak"));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  const auto replies = fixture.discord->replies();
  REQUIRE(replies[0].content ==
          "I could not form a response just now. Please try again shortly.");
  REQUIRE(fixture.diagnostics->contains_category("ai.response"));
  const auto events = fixture.diagnostics->events();
  REQUIRE(events[0].correlation_id == "correlation-1");
  fixture.application->stop();
}

TEST_CASE("prompt completion failure releases the prepared reservation",
          "[application][failure][relationship]") {
  sanguinius::test::ApplicationFixture fixture{social_ai_options()};
  fixture.relationships->prepared.status =
      sanguinius::PromptPreparationStatus::prepared;
  fixture.relationships->throw_on_completion = true;
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> speak", 80));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.relationships->completion_count() == 1);
  REQUIRE(fixture.relationships->failure_count() == 1);
  REQUIRE(fixture.diagnostics->contains_category("ai.response"));
  fixture.application->stop();
}

TEST_CASE("prompt transition failures are diagnosed without masking fallback",
          "[application][failure][relationship]") {
  sanguinius::test::ApplicationFixture fixture{social_ai_options()};
  fixture.relationships->prepared.status =
      sanguinius::PromptPreparationStatus::prepared;
  fixture.relationships->throw_on_failure = true;
  fixture.ai->fail();
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> speak", 81));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.relationships->failure_count() == 1);
  REQUIRE(fixture.diagnostics->contains_category("ai.prompt_attempt"));
  REQUIRE(fixture.diagnostics->contains_category("ai.response"));
  fixture.application->stop();
}

TEST_CASE("history failure still generates from the triggering request",
          "[application][failure]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.discord->fail_history();
  fixture.ai->set_response("fallback response");
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> latest request"));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  const auto requests = fixture.ai->requests();
  REQUIRE(requests.size() == 1);
  REQUIRE(contains(requests[0].conversation[1].content,
                   "CURRENT REQUEST\nlatest request"));
  REQUIRE_FALSE(contains(requests[0].conversation[1].content, "Test User"));
  REQUIRE(contains(requests[0].conversation[0].content,
                   "CURRENT REQUEST AUTHOR METADATA\nDisplay name: Test User"));
  REQUIRE_FALSE(contains(requests[0].conversation[0].content,
                         "RECENT MESSAGES — OLDEST FIRST"));
  REQUIRE(fixture.diagnostics->contains_category("discord.history"));
  fixture.application->stop();
}

TEST_CASE("message log failure does not prevent command routing",
          "[application][failure]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.log->fail();
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> help"));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.diagnostics->contains_category("message.logging"));
  fixture.application->stop();
}

TEST_CASE("full application queue replies only to actionable input",
          "[application][backpressure]") {
  sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {},
      .features = {},
      .tarot_policy = {},
      .wager_policy = {},
      .tarot_house_policy = {.house_enabled = false,
                             .integration_enabled = false},
      .tarot_deck_catalog = std::nullopt,
      .tarot_house_catalog = std::nullopt,
      .build = {"test-version", "test-revision"},
      .persistence = {true, 1, 1, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 1,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
      .speech = {},
      .voice_input = {},
      .static_speech_assets = {},
  }};
  fixture.log->block();
  fixture.application->start();

  fixture.discord->emit(sanguinius::test::incoming("ordinary one", 1));
  REQUIRE(fixture.log->wait_until_entered(2s));
  fixture.discord->emit(sanguinius::test::incoming("ordinary two", 2));
  fixture.discord->emit(sanguinius::test::incoming("<@42> help", 3));
  fixture.discord->emit(sanguinius::test::incoming("ordinary four", 4));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  const auto replies = fixture.discord->replies();
  REQUIRE(replies.size() == 1);
  REQUIRE(replies[0].target.message_id == 3);
  REQUIRE(replies[0].content ==
          "I am handling too many requests right now. Please try again "
          "shortly.");
  REQUIRE(fixture.diagnostics->contains_category("message.queue_full"));

  fixture.log->release();
  fixture.application->stop();
}

TEST_CASE("fixture teardown releases a blocked message log",
          "[application][shutdown]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.log->block();
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("ordinary", 1));

  REQUIRE(fixture.log->wait_until_entered(2s));
}

TEST_CASE("full AI queue preserves mention overload response",
          "[application][backpressure]") {
  sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {},
      .features = {},
      .tarot_policy = {},
      .wager_policy = {},
      .tarot_house_policy = {.house_enabled = false,
                             .integration_enabled = false},
      .tarot_deck_catalog = std::nullopt,
      .tarot_house_catalog = std::nullopt,
      .build = {"test-version", "test-revision"},
      .persistence = {true, 1, 1, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 1,
      .ai_worker_count = 1,
      .speech = {},
      .voice_input = {},
      .static_speech_assets = {},
  }};
  fixture.ai->block();
  fixture.application->start();

  fixture.discord->emit(sanguinius::test::incoming("<@42> first", 1));
  REQUIRE(fixture.ai->wait_until_entered(2s));
  fixture.discord->emit(sanguinius::test::incoming("<@42> second", 2));
  fixture.discord->emit(sanguinius::test::incoming("<@42> third", 3));

  REQUIRE(fixture.log->wait_for_count(3, 2s));
  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  const auto replies = fixture.discord->replies();
  REQUIRE(replies.size() == 1);
  REQUIRE(replies[0].target.message_id == 3);
  REQUIRE(replies[0].content ==
          "I am handling too many requests right now. Please try again "
          "shortly.");

  fixture.ai->release();
  fixture.application->stop();
}

TEST_CASE(
    "shutdown cancels AI without a false failure reply and stops gateway last",
    "[application][shutdown]") {
  sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {},
      .features = {},
      .tarot_policy = {},
      .wager_policy = {},
      .tarot_house_policy = {.house_enabled = false,
                             .integration_enabled = false},
      .tarot_deck_catalog = std::nullopt,
      .tarot_house_catalog = std::nullopt,
      .build = {"test-version", "test-revision"},
      .persistence = {true, 1, 1, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
      .speech = {},
      .voice_input = {},
      .static_speech_assets = {},
  }};
  fixture.ai->block();
  bool cancelled_before_gateway_shutdown = false;
  fixture.discord->set_shutdown_observer(
      [&fixture, &cancelled_before_gateway_shutdown] {
        cancelled_before_gateway_shutdown = fixture.ai->cancelled();
      });

  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("<@42> wait"));
  REQUIRE(fixture.ai->wait_until_entered(2s));

  fixture.application->stop();
  fixture.application->stop();

  REQUIRE(fixture.ai->cancelled());
  REQUIRE(cancelled_before_gateway_shutdown);
  REQUIRE(fixture.discord->replies().empty());
  REQUIRE(fixture.discord->lifecycle() ==
          std::vector<std::string>{"gateway.start", "gateway.stop_accepting",
                                   "gateway.shutdown"});
}

TEST_CASE("partial gateway startup unwinds workers and cannot restart",
          "[application][shutdown]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.discord->fail_start();

  REQUIRE_THROWS_AS(fixture.application->start(), std::runtime_error);
  REQUIRE(fixture.discord->shutdown_called());
  REQUIRE(fixture.instances->starts().size() == 1);
  REQUIRE(fixture.instances->stops().size() == 1);
  REQUIRE(fixture.instances->stops()[0].reason ==
          sanguinius::ApplicationStopReason::startup_failure);
  REQUIRE_THROWS_AS(fixture.application->start(), std::logic_error);
}

TEST_CASE("clean shutdown records one application instance terminal state",
          "[application][database][shutdown]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{2}});
  fixture.application->stop();
  fixture.application->stop();

  REQUIRE(fixture.instances->starts().size() == 1);
  REQUIRE(fixture.instances->starts()[0].started_at_ms == 1'000);
  REQUIRE(fixture.instances->stops().size() == 1);
  REQUIRE(fixture.instances->stops()[0].stopped_at_ms == 2'000);
  REQUIRE(fixture.instances->stops()[0].reason ==
          sanguinius::ApplicationStopReason::clean_shutdown);
}
