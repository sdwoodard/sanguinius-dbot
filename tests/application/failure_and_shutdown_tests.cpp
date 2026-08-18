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
  REQUIRE(contains(requests[0].conversation[0].content,
                   "Latest request from Test User:\nlatest request"));
  REQUIRE_FALSE(contains(requests[0].conversation[0].content,
                         "Recent messages (oldest first):"));
  REQUIRE(fixture.diagnostics->contains_category("discord.history"));
  fixture.application->stop();
}

TEST_CASE("message log failure does not prevent command routing",
          "[application][failure]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.log->fail();
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("!help"));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.diagnostics->contains_category("message.logging"));
  fixture.application->stop();
}

TEST_CASE("full application queue replies only to actionable input",
          "[application][backpressure]") {
  sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .message_queue_capacity = 1,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
  }};
  fixture.log->block();
  fixture.application->start();

  fixture.discord->emit(sanguinius::test::incoming("ordinary one", 1));
  REQUIRE(fixture.log->wait_until_entered(2s));
  fixture.discord->emit(sanguinius::test::incoming("ordinary two", 2));
  fixture.discord->emit(sanguinius::test::incoming("!help", 3));
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
      .message_queue_capacity = 64,
      .ai_queue_capacity = 1,
      .ai_worker_count = 1,
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
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
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
  REQUIRE_THROWS_AS(fixture.application->start(), std::logic_error);
}
