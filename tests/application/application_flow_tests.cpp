#include "support/application_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("help and repository commands pass through application seams",
          "[application][commands]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.application->start();
  REQUIRE_THROWS_AS(fixture.application->start(), std::logic_error);

  fixture.discord->emit(sanguinius::test::incoming("!help", 101));
  fixture.discord->emit(sanguinius::test::incoming("!repo", 102));

  REQUIRE(fixture.discord->wait_for_reply_count(2, 2s));
  const auto replies = fixture.discord->replies();
  REQUIRE(replies[0].target.message_id == 101);
  REQUIRE(replies[0].content ==
          "Mention me at the beginning of a message to ask me something.\n"
          "Sanguinius also supports two commands:\n"
          "`!help` — show this message\n"
          "`!repo` — show the source repository");
  REQUIRE_FALSE(replies[0].embed.has_value());
  REQUIRE(replies[0].suppress_mentions);

  REQUIRE(replies[1].target.message_id == 102);
  REQUIRE(replies[1].content.empty());
  REQUIRE(replies[1].embed.has_value());
  REQUIRE(replies[1].embed->color == 0x0E4BEFU);
  REQUIRE(replies[1].embed->title == "Sanguinius source code");
  REQUIRE(replies[1].embed->url ==
          "https://github.com/sdwoodard/sanguinius-dbot");
  REQUIRE(replies[1].embed->description ==
          "Build instructions and source code for the Sanguinius Discord "
          "bot.");

  fixture.discord->emit(sanguinius::test::incoming("bot output", 103, true));
  REQUIRE(fixture.log->wait_for_count(3, 2s));
  REQUIRE(fixture.discord->replies().size() == 2);
  fixture.application->stop();
}

TEST_CASE("one mention flows through fake history AI and Discord delivery",
          "[application][ai]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.ai->set_response("In the Emperor's name.");
  fixture.discord->set_recent({
      sanguinius::ConversationEntry{1, "first", "First", "Earlier words"},
  });
  fixture.discord->set_fetched(
      sanguinius::ConversationEntry{30, "second", "Second", "Quoted words"});

  auto request = sanguinius::test::incoming("<@42> Answer me", 100);
  request.replied_to = sanguinius::MessageReference{30, 10, 20};

  fixture.application->start();
  fixture.discord->emit(std::move(request));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.ai->wait_for_request_count(1, 2s));
  const auto ai_requests = fixture.ai->requests();
  REQUIRE(ai_requests[0].instructions == "test persona");
  REQUIRE(ai_requests[0].max_output_tokens == 500);
  REQUIRE(ai_requests[0].conversation.size() == 1);
  REQUIRE(ai_requests[0].conversation[0].role == "user");
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "Recent messages (oldest first):\nFirst: Earlier words\n"));
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "Message explicitly being replied to:\n"
                   "Second: Quoted words\n"));
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "Latest request from Test User:\nAnswer me"));

  const auto replies = fixture.discord->replies();
  REQUIRE(replies[0].target.message_id == 100);
  REQUIRE(replies[0].target.guild_id == 10);
  REQUIRE(replies[0].target.channel_id == 20);
  REQUIRE(replies[0].content == "In the Emperor's name.");
  REQUIRE(replies[0].suppress_mentions);
  REQUIRE(fixture.discord->typing_channels() ==
          std::vector<sanguinius::DiscordId>{20});

  const auto logged = fixture.log->messages();
  REQUIRE(logged.size() == 1);
  REQUIRE(logged[0].author_username == "test-user");
  REQUIRE(logged[0].content == "<@42> Answer me");
  fixture.application->stop();
}
