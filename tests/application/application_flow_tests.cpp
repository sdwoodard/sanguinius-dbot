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

TEST_CASE("configured owner receives redacted health in primary scope",
          "[application][admin][health]") {
  sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
      .persona = "PERSONA_SECRET_SENTINEL",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {.admin_commands_enabled = true, .test_mode = false},
      .features = {.chronicle_enabled = false,
                   .tarot_enabled = false,
                   .appearances_mode = sanguinius::AppearanceMode::off,
                   .vox_enabled = false,
                   .voice_input_enabled = false},
      .build = {"test-version", "test-revision"},
      .persistence = {true, 1, 1, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 2,
  }};
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("!SANG-ADMIN health", 104));

  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  const auto replies = fixture.discord->replies();
  REQUIRE(replies.size() == 1);
  REQUIRE(replies[0].target.message_id == 104);
  REQUIRE(replies[0].suppress_mentions);
  REQUIRE(contains(replies[0].content, "Sanguinius health"));
  REQUIRE(contains(replies[0].content, "version=test-version"));
  REQUIRE(contains(replies[0].content, "revision=test-revision"));
  REQUIRE(contains(replies[0].content, "scope=matched"));
  REQUIRE(contains(replies[0].content, "message_queue="));
  REQUIRE(contains(replies[0].content, "ai_queue="));
  REQUIRE(contains(replies[0].content, "test_mode=disabled"));
  REQUIRE_FALSE(contains(replies[0].content, "PERSONA_SECRET_SENTINEL"));
  REQUIRE_FALSE(contains(replies[0].content, "123456789012345678"));
  fixture.application->stop();
}

TEST_CASE("owner admin route is silent when disabled or outside scope",
          "[application][admin][scope]") {
  SECTION("disabled") {
    sanguinius::test::ApplicationFixture fixture;
    fixture.application->start();
    fixture.discord->emit(
        sanguinius::test::incoming("!sang-admin health", 105));

    REQUIRE(fixture.log->wait_for_count(1, 2s));
    fixture.application->stop();
    REQUIRE(fixture.discord->replies().empty());
    REQUIRE(fixture.diagnostics->contains_category("admin.request_rejected"));
    const auto events = fixture.diagnostics->events();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].message ==
            "Owner admin request was not handled: status=disabled scope=none.");
  }

  SECTION("wrong guild channel and owner") {
    constexpr sanguinius::DiscordId guild_id{18446744073709551615ULL};
    constexpr sanguinius::DiscordId channel_id{9223372036854775808ULL};
    constexpr sanguinius::DiscordId owner_user_id{123456789012345678ULL};
    sanguinius::test::ApplicationFixture fixture{sanguinius::ApplicationOptions{
        .persona = "PERSONA_SECRET_SENTINEL",
        .command_prefix = "!",
        .server_scope = {guild_id, channel_id, owner_user_id},
        .controls = {.admin_commands_enabled = true, .test_mode = false},
        .features = {},
        .build = {"test-version", "test-revision"},
        .persistence = {true, 1, 1, "3.53.4",
                        "00000000-0000-4000-8000-000000000001"},
        .instance_id = "00000000-0000-4000-8000-000000000001",
        .hostname = "test-host",
        .process_id = 123,
        .message_queue_capacity = 64,
        .ai_queue_capacity = 64,
        .ai_worker_count = 2,
    }};
    fixture.application->start();

    auto wrong_guild = sanguinius::test::incoming("!sang-admin health", 106);
    wrong_guild.guild_id = 11;
    wrong_guild.channel_id = channel_id;
    wrong_guild.author_user_id = owner_user_id;
    fixture.discord->emit(std::move(wrong_guild));
    auto wrong_channel = sanguinius::test::incoming("!sang-admin health", 107);
    wrong_channel.channel_id = 21;
    wrong_channel.guild_id = guild_id;
    wrong_channel.author_user_id = owner_user_id;
    wrong_channel.author_username = "AUTHOR_SECRET_SENTINEL";
    fixture.discord->emit(std::move(wrong_channel));
    auto non_owner = sanguinius::test::incoming("!sang-admin health", 108);
    non_owner.guild_id = guild_id;
    non_owner.channel_id = channel_id;
    non_owner.author_user_id = 31;
    fixture.discord->emit(std::move(non_owner));
    fixture.discord->emit(
        sanguinius::test::incoming("!sang-admin health", 109, true));
    fixture.discord->emit(
        sanguinius::test::incoming("!sang-admin inspect", 110));
    fixture.discord->emit(sanguinius::test::incoming("!sang-admin reset", 111));

    REQUIRE(fixture.log->wait_for_count(6, 2s));
    fixture.application->stop();
    REQUIRE(fixture.discord->replies().empty());
    REQUIRE(fixture.ai->requests().empty());
    REQUIRE(fixture.diagnostics->contains_category("admin.request_rejected"));

    std::size_t rejection_diagnostics{};
    for (const auto &event : fixture.diagnostics->events()) {
      if (event.category != "admin.request_rejected") {
        continue;
      }
      ++rejection_diagnostics;
      REQUIRE((event.message ==
                   "Owner admin request was not handled: status=rejected "
                   "scope=wrong_guild." ||
               event.message ==
                   "Owner admin request was not handled: status=rejected "
                   "scope=wrong_channel." ||
               event.message ==
                   "Owner admin request was not handled: status=rejected "
                   "scope=owner_required."));
      constexpr std::string_view forbidden[]{
          "PERSONA_SECRET_SENTINEL", "AUTHOR_SECRET_SENTINEL",
          "18446744073709551615",    "9223372036854775808",
          "123456789012345678",
      };
      for (const auto value : forbidden) {
        REQUIRE_FALSE(contains(event.message, value));
      }
    }
    REQUIRE(rejection_diagnostics == 3);
  }
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
