#include "sanguinius/command_registry.hpp"
#include "sanguinius/discord_command_cli.hpp"
#include "sanguinius/interaction_response_state.hpp"
#include "sanguinius/pending_notice.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string_view>

namespace {

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("command catalog version three is deterministic and feature gated",
          "[interaction][commands]") {
  const auto public_catalog = sanguinius::command_catalog(false);
  REQUIRE(public_catalog.version == 3);
  REQUIRE(public_catalog.commands.size() == 1);
  REQUIRE(public_catalog.commands[0].name == "sanguinius");
  REQUIRE(public_catalog.commands[0].subcommands.size() == 3);

  const auto admin_catalog = sanguinius::command_catalog(true);
  REQUIRE(admin_catalog.commands.size() == 2);
  REQUIRE(admin_catalog.commands[1].name == "sang-admin");
  REQUIRE(admin_catalog.commands[1].subcommands.size() == 6);
  REQUIRE(sanguinius::canonical_command_snapshot(admin_catalog) ==
          sanguinius::canonical_command_snapshot(
              sanguinius::command_catalog(true)));
  REQUIRE(sanguinius::canonical_command_snapshot(public_catalog) !=
          sanguinius::canonical_command_snapshot(admin_catalog));

  const auto chronicle_catalog = sanguinius::command_catalog(false, true);
  REQUIRE(chronicle_catalog.commands.size() == 3);
  REQUIRE(chronicle_catalog.commands[1].name == "chronicle");
  REQUIRE(chronicle_catalog.commands[1].subcommands.size() == 4);
  REQUIRE(chronicle_catalog.commands[2].kind ==
          sanguinius::ApplicationCommandKind::message_context);
  REQUIRE(chronicle_catalog.commands[2].name ==
          "Canonize in the Chronicle");
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
