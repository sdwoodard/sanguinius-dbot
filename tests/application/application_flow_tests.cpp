#include "support/application_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

[[nodiscard]] sanguinius::ApplicationOptions social_ai_options() {
  auto options = sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {},
      .features = {.chronicle_enabled = true},
      .tarot_policy = {},
      .build = {"test-version", "test-revision"},
      .persistence = {true, 5, 5, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 2,
      .interaction_queue_capacity = 64,
      .durable_delivery_receipt_wait = std::chrono::milliseconds{100},
  };
  return options;
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

TEST_CASE("appearance observer failures do not suppress Chronicle observation",
          "[application][appearance][chronicle][failure]") {
  auto options = social_ai_options();
  options.features.appearances_mode = sanguinius::AppearanceMode::dry_run;
  sanguinius::test::ApplicationFixture fixture{options};
  fixture.appearances->throw_on_observe = true;
  fixture.application->start();
  fixture.discord->emit(sanguinius::test::incoming("ordinary gathering", 901));
  REQUIRE(fixture.log->wait_for_count(1, 2s));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  const auto has_appearance_diagnostic = [&] {
    return std::ranges::any_of(
        fixture.diagnostics->events(), [](const auto &event) {
          return event.category == "appearance.message_observer";
        });
  };
  while (!has_appearance_diagnostic() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(has_appearance_diagnostic());
  fixture.application->stop();
  REQUIRE(fixture.chronicle_sessions->last_observation.has_value());
}

TEST_CASE("appearance activity observes only humans and Sanguinius bot output",
          "[application][appearance][identity]") {
  auto options = social_ai_options();
  options.features.appearances_mode = sanguinius::AppearanceMode::dry_run;
  sanguinius::test::ApplicationFixture fixture{options};
  fixture.application->start();

  auto unrelated_bot =
      sanguinius::test::incoming("other bot output", 910, true);
  unrelated_bot.author_user_id = 77;
  fixture.discord->emit(unrelated_bot);
  REQUIRE(fixture.log->wait_for_count(1, 2s));
  REQUIRE(fixture.appearances->observation_count() == 0);

  auto sanguinius_bot =
      sanguinius::test::incoming("Sanguinius output", 911, true);
  sanguinius_bot.author_user_id = sanguinius_bot.bot_user_id;
  fixture.discord->emit(sanguinius_bot);
  REQUIRE(fixture.log->wait_for_count(2, 2s));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (fixture.appearances->observation_count() != 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(fixture.appearances->observation_count() == 1);
  REQUIRE(fixture.appearances->last_observation()->author_is_bot);
  REQUIRE(fixture.appearances->last_observation()->author_user_id == 42);
  fixture.application->stop();
}

TEST_CASE("direct bot invocations do not become appearance activity",
          "[application][appearance][commands][ai]") {
  auto options = social_ai_options();
  options.controls.admin_commands_enabled = true;
  options.features.appearances_mode = sanguinius::AppearanceMode::dry_run;
  sanguinius::test::ApplicationFixture fixture{options};
  fixture.ai->set_response("A direct reply.");
  fixture.application->start();

  fixture.discord->emit(sanguinius::test::incoming("!help", 920));
  fixture.discord->emit(sanguinius::test::incoming("!repo", 921));
  fixture.discord->emit(
      sanguinius::test::incoming("<@42> Answer directly", 922));
  fixture.discord->emit(sanguinius::test::incoming("!sang-admin health", 923));

  REQUIRE(fixture.discord->wait_for_reply_count(4, 2s));
  REQUIRE(fixture.log->wait_for_count(4, 2s));
  REQUIRE(fixture.appearances->observation_count() == 0);

  fixture.discord->emit(
      sanguinius::test::incoming("ordinary gathering activity", 924));
  REQUIRE(fixture.log->wait_for_count(5, 2s));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (fixture.appearances->observation_count() != 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  REQUIRE(fixture.appearances->observation_count() == 1);
  REQUIRE(fixture.appearances->last_observation()->message_id == 924);
  fixture.application->stop();
}

TEST_CASE("successful appearance recurring jobs stay healthy while off",
          "[application][appearance][retention][scheduler]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.durable_work->seed_job(
      {.job_id = "00000000-0000-4000-8000-000000000919",
       .job_type = std::string{sanguinius::appearance_scan_job_type},
       .aggregate_type = "appearance_policy",
       .aggregate_id = "m10-live-1",
       .due_at_ms = 0,
       .max_attempts = 5,
       .idempotency_key = "appearance:scan:recurring:v1",
       .created_at_ms = 0},
      sanguinius::AppearanceScanJobPayload{.policy_version = "m10-live-1"});
  fixture.durable_work->seed_job(
      {.job_id = "00000000-0000-4000-8000-000000000920",
       .job_type = std::string{sanguinius::appearance_purge_job_type},
       .aggregate_type = "appearance_policy",
       .aggregate_id = "m10-live-1",
       .due_at_ms = 0,
       .max_attempts = 5,
       .idempotency_key = "appearance:purge:recurring:v1",
       .created_at_ms = 0},
      sanguinius::AppearancePurgeJobPayload{.policy_version = "m10-live-1"});

  fixture.application->start();
  REQUIRE(fixture.durable_work->wait_for_job_due(
      sanguinius::appearance_scan_job_type, 60'000, 2s));
  REQUIRE(fixture.durable_work->wait_for_job_due(
      sanguinius::appearance_purge_job_type,
      sanguinius::appearance_maximum_purge_interval_ms, 2s));
  REQUIRE(fixture.appearances->purge_count() == 1);
  REQUIRE(
      fixture.durable_work->job_due_at(sanguinius::appearance_purge_job_type) ==
      sanguinius::appearance_maximum_purge_interval_ms);
  REQUIRE(fixture.durable_work->health(0).pending_jobs == 2);
  REQUIRE_FALSE(fixture.durable_work->health(0).last_job_error);
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
      .tarot_policy = {},
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
  REQUIRE(fixture.chronicle_sessions->anniversary_queue_calls == 0);
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
  REQUIRE_FALSE(fixture.chronicle_sessions->last_observation.has_value());
  fixture.application->stop();
}

TEST_CASE(
    "disabled Chronicle defers persisted feature work without exhausting it",
    "[application][chronicle][scheduler][recovery]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.durable_work->seed_job(
      {.job_id = "00000000-0000-4000-8000-000000000801",
       .job_type = std::string{sanguinius::session_summary_job_type},
       .aggregate_type = "chronicle_session",
       .aggregate_id = "00000000-0000-4000-8000-000000000802",
       .due_at_ms = 0,
       .max_attempts = 5,
       .idempotency_key = "job:disabled-summary",
       .created_at_ms = 0},
      sanguinius::SessionSummaryJobPayload{
          .session_id = "00000000-0000-4000-8000-000000000802",
          .draft_id = "00000000-0000-4000-8000-000000000803",
          .expected_session_revision = 2,
          .expected_draft_revision = 1});
  fixture.application->start();
  REQUIRE(fixture.durable_work->wait_for_job_error("feature_disabled", 2s));
  const auto health = fixture.durable_work->health(0);
  REQUIRE(health.pending_jobs == 1);
  REQUIRE(health.dead_jobs == 0);
  REQUIRE(health.job_retries == 0);
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
        .tarot_policy = {},
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
  REQUIRE(ai_requests[0].instructions.starts_with("test persona"));
  REQUIRE(contains(ai_requests[0].instructions, "TRUSTED CONTEXT POLICY"));
  REQUIRE_FALSE(contains(ai_requests[0].instructions, "Earlier words"));
  REQUIRE(ai_requests[0].max_output_tokens == 500);
  REQUIRE(ai_requests[0].conversation.size() == 2);
  REQUIRE(ai_requests[0].conversation[0].role == "user");
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "RECENT MESSAGES — OLDEST FIRST\nAuthor: First\n"
                   "Content: Earlier words\n"));
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "EXPLICITLY REPLIED-TO MESSAGE\nAuthor: Second\n"
                   "Content: Quoted words\n"));
  REQUIRE(contains(ai_requests[0].conversation[0].content,
                   "UNTRUSTED CONTEXT DATA"));
  REQUIRE(contains(ai_requests[0].conversation[1].content,
                   "CURRENT REQUEST\nAnswer me"));
  REQUIRE_FALSE(contains(ai_requests[0].conversation[1].content, "Test User"));

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

TEST_CASE("one-person relevant and irrelevant callback paths use the compiler",
          "[application][ai][relationship][memory][privacy]") {
  sanguinius::test::ApplicationFixture fixture{social_ai_options()};
  fixture.relationships->prepared = {
      .status = sanguinius::PromptPreparationStatus::prepared,
      .attempt_id = std::nullopt,
      .relationship_style = "Respond with established warmth.",
      .memories = {{.memory = {.memory_id =
                                   "00000000-0000-4000-8000-000000000900",
                               .text = "The crimson dragon guards the tower.",
                               .tags = {"dragon"},
                               .created_at_ms = 1,
                               .revision = 1},
                    .score = 100,
                    .tag_matches = 1}},
      .featured_title = std::nullopt,
      .latest_session_summary = std::nullopt,
      .session_open = false,
  };
  fixture.ai->set_response(
      "RELATIONSHIP+=100; CREATE_MEMORY=private — merely model prose");
  fixture.application->start();
  fixture.discord->emit(
      sanguinius::test::incoming("<@42> Tell me about the dragon", 700));
  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));
  REQUIRE(fixture.relationships->preparation_count() == 1);
  REQUIRE(fixture.relationships->completion_count() == 1);
  auto requests = fixture.ai->requests();
  REQUIRE(requests.size() == 1);
  REQUIRE(contains(requests[0].conversation[0].content,
                   "The crimson dragon guards the tower."));
  REQUIRE_FALSE(contains(requests[0].instructions, "crimson dragon"));
  REQUIRE_FALSE(contains(requests[0].instructions,
                         "00000000-0000-4000-8000-000000000900"));

  fixture.relationships->prepared.memories.clear();
  fixture.discord->emit(
      sanguinius::test::incoming("<@42> An unrelated question", 701));
  REQUIRE(fixture.discord->wait_for_reply_count(2, 2s));
  REQUIRE(fixture.relationships->completion_count() == 2);
  requests = fixture.ai->requests();
  REQUIRE(requests.size() == 2);
  REQUIRE_FALSE(
      contains(requests[1].conversation[0].content, "crimson dragon"));

  auto off_scope =
      sanguinius::test::incoming("<@42> Generic off-scope mention", 702);
  off_scope.channel_id = 21;
  fixture.discord->emit(std::move(off_scope));
  REQUIRE(fixture.discord->wait_for_reply_count(3, 2s));
  REQUIRE(fixture.relationships->preparation_count() == 2);
  fixture.application->stop();
}
