#include "sanguinius/command_registry.hpp"
#include "sanguinius/discord_command_cli.hpp"
#include "sanguinius/interaction_handler.hpp"
#include "sanguinius/interaction_response_state.hpp"
#include "sanguinius/pending_notice.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("voice interaction privacy overwrite is coalesced and sent last",
          "[interaction][voice-input][privacy][ordering]") {
  std::mutex mutex;
  std::vector<sanguinius::InteractionMessage> sent;
  std::vector<sanguinius::DeliveryCallback> completions;
  const auto editor = std::make_shared<
      sanguinius::interaction_handler_detail::SequencedInteractionEditor>(
      [&](sanguinius::InteractionMessage message,
          sanguinius::DeliveryCallback completion) {
        const std::scoped_lock lock{mutex};
        sent.push_back(std::move(message));
        completions.push_back(std::move(completion));
      });

  bool transcript_receipt{};
  editor->submit(
      sanguinius::text_message("Transcript: private words"),
      [&](const sanguinius::DeliveryResult result) {
        transcript_receipt = result == sanguinius::DeliveryResult::success;
      },
      false);
  {
    const std::scoped_lock lock{mutex};
    REQUIRE(sent.size() == 1);
    REQUIRE(contains(sent[0].content, "private words"));
  }

  bool superseded_receipt{};
  editor->submit(
      sanguinius::text_message("No transcript was retained."),
      [&](const sanguinius::DeliveryResult result) {
        superseded_receipt =
            result == sanguinius::DeliveryResult::permanent_failure;
      },
      true);
  editor->submit(
      sanguinius::text_message("Voice listening was disabled. No transcript "
                               "was retained or delivered."),
      {}, true);
  {
    const std::scoped_lock lock{mutex};
    REQUIRE(sent.size() == 1);
  }
  REQUIRE(superseded_receipt);

  sanguinius::DeliveryCallback transcript_completion;
  {
    const std::scoped_lock lock{mutex};
    transcript_completion = completions[0];
  }
  transcript_completion(sanguinius::DeliveryResult::success);
  REQUIRE(transcript_receipt);
  {
    const std::scoped_lock lock{mutex};
    REQUIRE(sent.size() == 2);
    REQUIRE_FALSE(contains(sent[1].content, "private words"));
    REQUIRE(contains(sent[1].content, "No transcript"));
  }

  transcript_completion(sanguinius::DeliveryResult::success);
  {
    const std::scoped_lock lock{mutex};
    REQUIRE(sent.size() == 2);
  }
}

TEST_CASE("command catalog version sixteen is deterministic and feature gated",
          "[interaction][commands]") {
  const auto public_catalog = sanguinius::command_catalog(false);
  REQUIRE(public_catalog.version == 16);
  REQUIRE(public_catalog.commands.size() == 4);
  REQUIRE(public_catalog.commands[0].name == "help");
  REQUIRE(public_catalog.commands[1].name == "repo");
  REQUIRE(public_catalog.commands[2].name == "sanguinius");
  REQUIRE(public_catalog.commands[2].subcommands.size() == 5);
  REQUIRE(public_catalog.commands[2].subcommand_groups.size() == 1);
  REQUIRE(public_catalog.commands[2].subcommand_groups[0].name == "quiet");
  REQUIRE(public_catalog.commands[2].subcommand_groups[0].subcommands.size() ==
          4);
  REQUIRE(public_catalog.commands[3].name == "sang-admin");
  REQUIRE(public_catalog.commands[3].subcommands.empty());
  REQUIRE(public_catalog.commands[3].subcommand_groups.size() == 1);
  const auto &safety =
      public_catalog.commands[3].subcommand_groups[0].subcommands;
  REQUIRE(safety.size() == 2);
  REQUIRE(safety[0].name == "status");
  REQUIRE(safety[1].name == "set");

  const auto admin_catalog =
      sanguinius::command_catalog(true, false, false, false, true);
  REQUIRE(admin_catalog.commands.size() == 4);
  REQUIRE(admin_catalog.commands[3].name == "sang-admin");
  REQUIRE(admin_catalog.commands[3].subcommands.size() == 6);
  REQUIRE(admin_catalog.commands[3].subcommand_groups.size() == 3);
  REQUIRE(admin_catalog.commands[3].subcommand_groups[1].name ==
          "reliability-test");
  REQUIRE(admin_catalog.commands[3].subcommand_groups[1].subcommands.size() ==
          3);
  REQUIRE(admin_catalog.commands[3].subcommand_groups[2].name == "appearance");
  REQUIRE(admin_catalog.commands[3].subcommand_groups[2].subcommands.size() ==
          4);
  const auto &appearance =
      admin_catalog.commands[3].subcommand_groups[2].subcommands;
  REQUIRE(appearance[0].name == "simulate");
  REQUIRE(appearance[1].name == "preview");
  REQUIRE(appearance[2].name == "recent");
  REQUIRE(appearance[3].name == "trigger");
  for (const auto &subcommand : appearance)
    REQUIRE(subcommand.name != "force");
  REQUIRE(sanguinius::canonical_command_snapshot(admin_catalog) ==
          sanguinius::canonical_command_snapshot(
              sanguinius::command_catalog(true, false, false, false, true)));
  REQUIRE(sanguinius::canonical_command_snapshot(public_catalog) !=
          sanguinius::canonical_command_snapshot(admin_catalog));

  const auto non_test_admin_catalog = sanguinius::command_catalog(true);
  const auto &non_test_admin = non_test_admin_catalog.commands[3];
  REQUIRE(non_test_admin.subcommand_groups.size() == 2);
  REQUIRE(std::ranges::none_of(
      non_test_admin.subcommand_groups,
      [](const auto &group) { return group.name == "reliability-test"; }));

  const auto chronicle_catalog = sanguinius::command_catalog(false, true);
  REQUIRE(chronicle_catalog.commands.size() == 6);
  REQUIRE(chronicle_catalog.commands[3].name == "chronicle");
  REQUIRE(chronicle_catalog.commands[3].subcommands.size() == 6);
  REQUIRE(chronicle_catalog.commands[4].kind ==
          sanguinius::ApplicationCommandKind::message_context);
  REQUIRE(chronicle_catalog.commands[4].name == "Canonize in the Chronicle");
  REQUIRE(chronicle_catalog.commands[5].name == "sang-admin");

  const auto tarot_catalog =
      sanguinius::command_catalog(true, false, true, false, true);
  REQUIRE(tarot_catalog.version == 16);
  REQUIRE(tarot_catalog.commands.size() == 5);
  REQUIRE(tarot_catalog.commands[3].name == "tarot");
  REQUIRE(tarot_catalog.commands[3].subcommands.size() == 15);
  const auto &standings_visibility =
      tarot_catalog.commands[3].subcommands[3].options[0];
  REQUIRE(standings_visibility.choices[0].name == "Public");
  REQUIRE(standings_visibility.choices[0].value == "public");
  const auto &grace_visibility =
      tarot_catalog.commands[3].subcommands[4].options[0];
  REQUIRE(grace_visibility.choices[0].name == "Public flavor");
  REQUIRE(tarot_catalog.commands[4].name == "sang-admin");
  REQUIRE(tarot_catalog.commands[4].subcommand_groups.size() == 4);
  const auto &tarot_admin = tarot_catalog.commands[4].subcommand_groups[3];
  REQUIRE(tarot_admin.name == "tarot");
  REQUIRE(tarot_admin.subcommands.size() == 14);
  REQUIRE(tarot_admin.subcommands[0].options[0].kind ==
          sanguinius::CommandOptionKind::integer);
  REQUIRE(tarot_admin.subcommands[0].options[0].minimum_integer ==
          sanguinius::minimum_tarot_adjustment);
  REQUIRE(tarot_admin.subcommands[0].options[0].maximum_integer ==
          sanguinius::maximum_tarot_adjustment);
  REQUIRE(tarot_catalog.commands[3].subcommands[8].name == "wager");
  REQUIRE(tarot_catalog.commands[3].subcommands[8].options[0].kind ==
          sanguinius::CommandOptionKind::user);
  REQUIRE(tarot_catalog.commands[3].subcommands[9].name == "wagers");
  REQUIRE(tarot_catalog.commands[3].subcommands[14].name == "disputes");

  const auto vox_catalog =
      sanguinius::command_catalog(true, false, false, true, true);
  REQUIRE(vox_catalog.commands.size() == 5);
  REQUIRE(vox_catalog.commands[3].name == "vox");
  REQUIRE(vox_catalog.commands[3].subcommands.size() == 8);
  REQUIRE(vox_catalog.commands[3].subcommands[0].name == "summon");
  REQUIRE(vox_catalog.commands[3].subcommands[1].name == "status");
  REQUIRE(vox_catalog.commands[3].subcommands[2].name == "leave");
  REQUIRE(vox_catalog.commands[3].subcommands[3].name == "say");
  REQUIRE(vox_catalog.commands[3].subcommands[4].name == "mute");
  REQUIRE(vox_catalog.commands[3].subcommands[5].name == "voice");
  REQUIRE(vox_catalog.commands[3].subcommands[6].name == "listen-start");
  REQUIRE(vox_catalog.commands[3].subcommands[7].name == "listen-stop");
  REQUIRE(vox_catalog.commands[4].subcommand_groups.size() == 4);
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands.size() == 5);
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].name == "vox");
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands[0].name ==
          "disconnect");
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands[1].name ==
          "speech-test");
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands[2].name ==
          "narration-preview");
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands[3].name ==
          "narration-enqueue");
  REQUIRE(vox_catalog.commands[4].subcommand_groups[3].subcommands[4].name ==
          "narration-recent");
  const auto vox_safety_catalog =
      sanguinius::command_catalog(false, false, false, true);
  REQUIRE(vox_safety_catalog.commands.size() == 5);
  REQUIRE(vox_safety_catalog.commands[4].subcommand_groups.size() == 1);
  const auto &vox_safety = vox_safety_catalog.commands[4].subcommand_groups[0];
  REQUIRE(vox_safety.name == "safety");
  REQUIRE(vox_safety.subcommands.size() == 2);
  REQUIRE(vox_safety.subcommands[0].name == "status");
  REQUIRE(vox_safety.subcommands[1].name == "set");
  REQUIRE(tarot_catalog.commands[3].subcommand_groups.size() == 1);
  REQUIRE(tarot_catalog.commands[3].subcommand_groups[0].name == "house");
  REQUIRE(tarot_admin.subcommands[5].name == "house-offer");
  REQUIRE(tarot_admin.subcommands[8].name == "house-cleanup");
  REQUIRE(tarot_admin.subcommands[11].name == "wager-role");
  REQUIRE(tarot_admin.subcommands[12].name == "wager-deadline");
  REQUIRE(tarot_admin.subcommands[13].name == "wager-cleanup");
}

TEST_CASE("command registration reconciliation is guarded and retryable",
          "[interaction][commands][registration]") {
  sanguinius::CommandRegistrationCoordinator registration;
  REQUIRE(registration.state() ==
          sanguinius::CommandRegistrationState::not_started);
  REQUIRE(registration.begin());
  REQUIRE_FALSE(registration.begin());
  REQUIRE(registration.catalog_fetched(true, true) ==
          sanguinius::CommandCatalogFetchAction::none);
  REQUIRE(registration.state() ==
          sanguinius::CommandRegistrationState::synchronized);

  REQUIRE(registration.begin());
  REQUIRE(registration.catalog_fetched(true, false) ==
          sanguinius::CommandCatalogFetchAction::update_required);
  REQUIRE_FALSE(registration.begin());
  registration.catalog_updated(true);
  REQUIRE(registration.state() ==
          sanguinius::CommandRegistrationState::synchronized);

  REQUIRE(registration.begin());
  REQUIRE(registration.catalog_fetched(false, false) ==
          sanguinius::CommandCatalogFetchAction::none);
  REQUIRE(registration.state() == sanguinius::CommandRegistrationState::failed);
  REQUIRE(registration.begin());
  registration.catalog_updated(false);
  REQUIRE(registration.state() == sanguinius::CommandRegistrationState::failed);
}

TEST_CASE("Discord command operator parser requires the destructive guard",
          "[interaction][commands][usage]") {
  constexpr std::array<std::string_view, 3> sync{"discord", "commands", "sync"};
  REQUIRE(sanguinius::parse_discord_command(sync) ==
          sanguinius::DiscordCommandOperation::synchronize);

  constexpr std::array<std::string_view, 4> clear{"discord", "commands",
                                                  "clear", "--confirm"};
  REQUIRE(sanguinius::parse_discord_command(clear) ==
          sanguinius::DiscordCommandOperation::clear);

  constexpr std::array<std::string_view, 3> unguarded{"discord", "commands",
                                                      "clear"};
  REQUIRE_FALSE(sanguinius::parse_discord_command(unguarded).has_value());
  constexpr std::array<std::string_view, 4> wrong_guard{"discord", "commands",
                                                        "clear", "yes"};
  REQUIRE_FALSE(sanguinius::parse_discord_command(wrong_guard).has_value());
}

TEST_CASE("interaction response state permits one acknowledgement and fixed "
          "visibility",
          "[interaction][response]") {
  sanguinius::InteractionResponseState deferred;
  REQUIRE(
      deferred.acknowledge_defer(sanguinius::ResponseVisibility::ephemeral));
  REQUIRE_FALSE(
      deferred.acknowledge_reply(sanguinius::ResponseVisibility::ephemeral));
  REQUIRE_FALSE(deferred.acknowledge_modal());
  REQUIRE(deferred.may_edit());
  REQUIRE(deferred.may_follow_up(sanguinius::ResponseVisibility::ephemeral));
  REQUIRE_FALSE(
      deferred.may_follow_up(sanguinius::ResponseVisibility::public_message));

  sanguinius::InteractionResponseState modal;
  REQUIRE(modal.acknowledge_modal());
  REQUIRE_FALSE(modal.may_edit());
  REQUIRE_FALSE(modal.may_follow_up(sanguinius::ResponseVisibility::ephemeral));
}

TEST_CASE("component IDs contain only the version and an opaque UUID",
          "[interaction][token][privacy]") {
  constexpr std::string_view token{"00000000-0000-4000-8000-000000000123"};
  const auto component = sanguinius::make_component_id(token);
  REQUIRE(component == "sg:1:00000000-0000-4000-8000-000000000123");
  REQUIRE(sanguinius::parse_component_token(component) == token);

  for (const std::string_view invalid : {
           "sg:1:",
           "sg:2:00000000-0000-4000-8000-000000000123",
           "sg:1:00000000-0000-1000-8000-000000000123",
           "sg:1:00000000-0000-4000-7000-000000000123",
           "sg:1:00000000-0000-4000-8000-000000000123:notice=secret",
       }) {
    REQUIRE_FALSE(sanguinius::parse_component_token(invalid).has_value());
  }
}

TEST_CASE("neutral cards omit private notice content and identifiers",
          "[interaction][notice][privacy]") {
  sanguinius::test::FakePendingNoticeRepository repository;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{1}}};
  sanguinius::test::FakePersistentIdGenerator ids{{
      "00000000-0000-4000-8000-000000000111",
      "00000000-0000-4000-8000-000000000112",
  }};
  sanguinius::PendingNoticeService service{repository, clock, ids};
  sanguinius::IncomingInteraction interaction;
  interaction.interaction_id = 900;
  interaction.guild_id = 10;
  interaction.channel_id = 20;
  interaction.user_id = 30;
  const auto created = service.create_test_notice(interaction);

  const auto &message = created.public_card.message;
  REQUIRE(created.persistence.created);
  REQUIRE(message.allowed_user_mentions ==
          std::vector<sanguinius::DiscordId>{30});
  REQUIRE(message.buttons.size() == 1);
  REQUIRE(sanguinius::parse_component_token(message.buttons[0].custom_id) ==
          created.persistence.token_id);
  REQUIRE(contains(message.content, "<@30>"));
  REQUIRE_FALSE(contains(message.content, "Sealed notice test"));
  REQUIRE_FALSE(contains(message.content, "The sealed-notice test succeeded."));
  REQUIRE_FALSE(
      contains(message.content, created.persistence.notice.notice_id));
  REQUIRE_FALSE(contains(message.buttons[0].custom_id,
                         created.persistence.notice.notice_id));
  const auto public_rendering =
      message.content + message.embed->title + message.embed->description +
      message.buttons[0].label + message.buttons[0].custom_id;
  for (const std::string_view private_value : {
           "Sealed notice test",
           "The sealed-notice test succeeded.",
           "owner_test",
           "notice:create:test:900",
           "token:create:test:900",
       }) {
    REQUIRE_FALSE(contains(public_rendering, private_value));
  }
  REQUIRE_FALSE(
      contains(public_rendering, created.persistence.notice.notice_id));
}
