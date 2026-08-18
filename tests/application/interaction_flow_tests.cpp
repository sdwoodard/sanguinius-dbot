#include "support/application_fixture.hpp"

#include "sanguinius/pending_notice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string_view>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

[[nodiscard]] sanguinius::ApplicationOptions
admin_options(const std::size_t interaction_capacity = 64) {
  return sanguinius::ApplicationOptions{
      .persona = "test persona",
      .command_prefix = "!",
      .server_scope = {10, 20, 30},
      .controls = {.admin_commands_enabled = true, .test_mode = true},
      .features = {},
      .build = {"test-version", "test-revision"},
      .persistence = {true, 2, 2, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
      .interaction_queue_capacity = interaction_capacity,
  };
}

[[nodiscard]] sanguinius::IncomingInteraction
slash(const std::shared_ptr<sanguinius::test::FakeInteractionResponder>
          &responder,
      const std::string_view command, const std::string_view subcommand,
      const sanguinius::DiscordId interaction_id = 200) {
  auto request = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::slash_command, interaction_id);
  request.command_name = command;
  request.subcommand_name = subcommand;
  return request;
}

} // namespace

TEST_CASE("slash status privacy and scope enforcement remain ephemeral",
          "[application][interaction][privacy]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.application->start();
  REQUIRE(fixture.discord->command_catalog().commands.size() == 1);

  auto wrong_scope =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto rejected = slash(wrong_scope, "sanguinius", "status", 201);
  rejected.channel_id = 21;
  fixture.discord->emit(std::move(rejected));
  REQUIRE(wrong_scope->replies().size() == 1);
  REQUIRE(wrong_scope->replies()[0].second ==
          sanguinius::ResponseVisibility::ephemeral);
  REQUIRE(fixture.identities->user_count() == 0);
  REQUIRE(fixture.notices->notice_count() == 0);

  auto unknown = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(unknown, "sanguinius", "unknown", 202));
  REQUIRE(unknown->replies().size() == 1);
  REQUIRE(unknown->deferrals().empty());

  auto malformed =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto malformed_status = slash(malformed, "sanguinius", "status", 205);
  malformed_status.command_options.push_back(
      {"unexpected", std::string{"value"}});
  fixture.discord->emit(std::move(malformed_status));
  REQUIRE(malformed->replies().size() == 1);
  REQUIRE(contains(malformed->replies()[0].first.content, "malformed"));
  REQUIRE(fixture.identities->user_count() == 0);

  auto defer_failed =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  defer_failed->set_defer_result(sanguinius::DeliveryResult::transient_failure);
  fixture.discord->emit(slash(defer_failed, "sanguinius", "status", 206));
  REQUIRE(defer_failed->deferrals().size() == 1);
  REQUIRE(defer_failed->edits().empty());
  REQUIRE(fixture.identities->user_count() == 0);
  REQUIRE(fixture.diagnostics->contains_category("interaction.defer"));

  auto status = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(status, "sanguinius", "status", 203));
  REQUIRE(status->wait_for_edit_count(1, 2s));
  REQUIRE(status->deferrals() ==
          std::vector<sanguinius::ResponseVisibility>{
              sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(status->edits()[0].content, "Unopened sealed notices: 0"));

  auto privacy = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(privacy, "sanguinius", "privacy", 204));
  REQUIRE(privacy->wait_for_edit_count(1, 2s));
  REQUIRE(contains(privacy->edits()[0].content, "Discord DMs are never used"));
  REQUIRE(contains(privacy->edits()[0].content,
                   "Raw received voice audio is never persisted"));
  REQUIRE(fixture.identities->user_count() == 1);
  fixture.application->stop();
}

TEST_CASE("interaction router rejects unauthorized and malformed routes",
          "[application][interaction][authorization]") {
  sanguinius::test::ApplicationFixture disabled;
  disabled.application->start();
  auto admin_disabled =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  disabled.discord->emit(slash(admin_disabled, "sang-admin", "health", 210));
  REQUIRE(admin_disabled->replies().size() == 1);
  REQUIRE(contains(admin_disabled->replies()[0].first.content, "disabled"));
  REQUIRE(disabled.identities->user_count() == 0);
  disabled.application->stop();

  auto no_test_mode = admin_options();
  no_test_mode.controls.test_mode = false;
  sanguinius::test::ApplicationFixture guarded{std::move(no_test_mode)};
  guarded.application->start();
  auto wrong_owner =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto rejected_owner = slash(wrong_owner, "sang-admin", "health", 211);
  rejected_owner.user_id = 31;
  guarded.discord->emit(std::move(rejected_owner));
  REQUIRE(wrong_owner->replies().size() == 1);
  REQUIRE(contains(wrong_owner->replies()[0].first.content, "owner-only"));

  auto test_disabled =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  guarded.discord->emit(slash(test_disabled, "sang-admin", "test-notice", 212));
  REQUIRE(test_disabled->replies().size() == 1);
  REQUIRE(contains(test_disabled->replies()[0].first.content, "disabled"));
  REQUIRE(guarded.diagnostics->contains_category("interaction.admin_rejected"));

  for (const auto kind : {sanguinius::InteractionKind::button,
                          sanguinius::InteractionKind::select_menu,
                          sanguinius::InteractionKind::modal_submit}) {
    auto malformed =
        std::make_shared<sanguinius::test::FakeInteractionResponder>();
    auto request = sanguinius::test::interaction(malformed, kind, 213);
    request.custom_id = "sg:1:not-a-token";
    guarded.discord->emit(std::move(request));
    REQUIRE(malformed->replies().size() == 1);
    REQUIRE(contains(malformed->replies()[0].first.content, "invalid"));
  }

  auto context = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  guarded.discord->emit(sanguinius::test::interaction(
      context, sanguinius::InteractionKind::message_context_command, 214));
  REQUIRE(context->replies().size() == 1);
  REQUIRE(contains(context->replies()[0].first.content, "not available yet"));
  REQUIRE(guarded.identities->user_count() == 0);
  REQUIRE(guarded.notices->notice_count() == 0);
  guarded.application->stop();
}

TEST_CASE("owner test notice card and private open are idempotent",
          "[application][interaction][notice]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();
  REQUIRE(fixture.discord->command_catalog().commands.size() == 2);

  auto health = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(health, "sang-admin", "health", 299));
  REQUIRE(health->wait_for_edit_count(1, 2s));
  REQUIRE(contains(health->edits()[0].content, "schema=2/2"));
  REQUIRE(contains(health->edits()[0].content,
                   "command_registration=synchronized"));
  REQUIRE(contains(health->edits()[0].content, "interaction_queue="));

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 300));
  REQUIRE(created->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.notices->notice_count() == 1);
  REQUIRE(fixture.diagnostics->contains_category("interaction.test_notice"));
  const auto cards = fixture.discord->public_messages();
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].guild_id == 10);
  REQUIRE(cards[0].channel_id == 20);
  REQUIRE(cards[0].message.allowed_user_mentions ==
          std::vector<sanguinius::DiscordId>{30});
  REQUIRE(cards[0].message.buttons.size() == 1);
  REQUIRE_FALSE(
      contains(cards[0].message.content, "The sealed-notice test succeeded"));

  auto duplicate =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(duplicate, "sang-admin", "test-notice", 300));
  REQUIRE(duplicate->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.notices->notice_count() == 1);
  REQUIRE(fixture.discord->public_messages().size() == 1);

  auto opened = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto button = sanguinius::test::interaction(
      opened, sanguinius::InteractionKind::button, 301);
  button.custom_id = cards[0].message.buttons[0].custom_id;
  fixture.discord->emit(std::move(button));
  REQUIRE(opened->wait_for_edit_count(1, 2s));
  REQUIRE(contains(opened->edits()[0].content,
                   "The sealed-notice test succeeded."));

  auto replayed =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto replay_button = sanguinius::test::interaction(
      replayed, sanguinius::InteractionKind::button, 302);
  replay_button.custom_id = cards[0].message.buttons[0].custom_id;
  fixture.discord->emit(std::move(replay_button));
  REQUIRE(replayed->wait_for_edit_count(1, 2s));
  REQUIRE(replayed->edits()[0].content == opened->edits()[0].content);
  for (const auto &event : fixture.diagnostics->events()) {
    REQUIRE_FALSE(contains(event.message, "Sealed notice test"));
    REQUIRE_FALSE(contains(event.message, "The sealed-notice test succeeded."));
    REQUIRE_FALSE(
        contains(event.message, cards[0].message.buttons[0].custom_id));
  }
  fixture.application->stop();
}

TEST_CASE("inbox recovers notices when public delivery is unconfirmed",
          "[application][interaction][notice][failure]") {
  for (const auto delivery_result : {
           sanguinius::DeliveryResult::transient_failure,
           sanguinius::DeliveryResult::unknown_outcome,
           sanguinius::DeliveryResult::permanent_failure,
       }) {
    sanguinius::test::ApplicationFixture fixture{admin_options()};
    fixture.discord->set_public_delivery_result(delivery_result);
    fixture.application->start();

    auto created =
        std::make_shared<sanguinius::test::FakeInteractionResponder>();
    fixture.discord->emit(slash(created, "sang-admin", "test-notice", 400));
    REQUIRE(created->wait_for_edit_count(1, 2s));
    REQUIRE(contains(created->edits()[0].content, "not confirmed delivered"));
    REQUIRE(fixture.notices->notice_count() == 1);

    auto inbox = std::make_shared<sanguinius::test::FakeInteractionResponder>();
    fixture.discord->emit(slash(inbox, "sanguinius", "inbox", 401));
    REQUIRE(inbox->wait_for_edit_count(1, 2s));
    REQUIRE(contains(inbox->edits()[0].content,
                     "The sealed-notice test succeeded."));

    auto duplicate =
        std::make_shared<sanguinius::test::FakeInteractionResponder>();
    fixture.discord->emit(slash(duplicate, "sanguinius", "inbox", 401));
    REQUIRE(duplicate->wait_for_edit_count(1, 2s));
    REQUIRE(duplicate->edits()[0].content == inbox->edits()[0].content);
    fixture.application->stop();
  }
}

TEST_CASE("failed private reveal remains retrievable through the inbox",
          "[application][interaction][notice][delivery]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 450));
  REQUIRE(created->wait_for_edit_count(1, 2s));
  const auto cards = fixture.discord->public_messages();
  REQUIRE(cards.size() == 1);

  auto failed = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  failed->set_edit_result(sanguinius::DeliveryResult::transient_failure);
  auto button = sanguinius::test::interaction(
      failed, sanguinius::InteractionKind::button, 451);
  button.custom_id = cards[0].message.buttons[0].custom_id;
  fixture.discord->emit(std::move(button));
  REQUIRE(failed->wait_for_edit_count(1, 2s));
  REQUIRE(contains(failed->edits()[0].content,
                   "The sealed-notice test succeeded."));
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);
  REQUIRE(fixture.diagnostics->contains_category("interaction.component"));

  auto inbox = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(inbox, "sanguinius", "inbox", 452));
  REQUIRE(inbox->wait_for_edit_count(1, 2s));
  REQUIRE(inbox->edits()[0].content == failed->edits()[0].content);
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 0);
  fixture.application->stop();
}

TEST_CASE("late private delivery completion is suppressed after shutdown",
          "[application][interaction][notice][shutdown]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 455));
  REQUIRE(created->wait_for_edit_count(1, 2s));

  auto inbox = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  inbox->hold_edit_completions();
  fixture.discord->emit(slash(inbox, "sanguinius", "inbox", 456));
  REQUIRE(inbox->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);

  fixture.application->stop();
  inbox->complete_edit_completions();
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);
}

TEST_CASE("prefix health uses live interaction and pending notice metrics",
          "[application][interaction][health]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 457));
  REQUIRE(created->wait_for_edit_count(1, 2s));
  fixture.discord->emit(sanguinius::test::incoming("!sang-admin health", 458));
  REQUIRE(fixture.discord->wait_for_reply_count(1, 2s));

  const auto replies = fixture.discord->replies();
  REQUIRE(replies.size() == 1);
  REQUIRE(contains(replies[0].content, "interaction_queue="));
  REQUIRE(contains(replies[0].content, "pending_notices=1"));
  fixture.application->stop();
}

TEST_CASE("duplicate empty inbox interaction never advances to a later notice",
          "[application][interaction][notice][idempotency]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();

  auto empty = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(empty, "sanguinius", "inbox", 460));
  REQUIRE(empty->wait_for_edit_count(1, 2s));
  REQUIRE(contains(empty->edits()[0].content, "No sealed notices"));

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 461));
  REQUIRE(created->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);

  auto replay = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(replay, "sanguinius", "inbox", 460));
  REQUIRE(replay->wait_for_edit_count(1, 2s));
  REQUIRE(replay->edits()[0].content == empty->edits()[0].content);
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 1);

  auto fresh = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(fresh, "sanguinius", "inbox", 462));
  REQUIRE(fresh->wait_for_edit_count(1, 2s));
  REQUIRE(
      contains(fresh->edits()[0].content, "The sealed-notice test succeeded."));
  REQUIRE(fixture.notices->pending_count(30, 1'000) == 0);
  fixture.application->stop();
}

TEST_CASE("interaction queue saturation edits the deferred response",
          "[application][interaction][backpressure]") {
  sanguinius::test::ApplicationFixture fixture{admin_options(1)};
  fixture.identities->block();
  fixture.application->start();

  auto first = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(first, "sanguinius", "status", 501));
  REQUIRE(fixture.identities->wait_until_entered(2s));

  auto queued = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(queued, "sanguinius", "status", 502));
  auto overloaded =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(overloaded, "sanguinius", "status", 503));
  REQUIRE(overloaded->wait_for_edit_count(1, 2s));
  REQUIRE(contains(overloaded->edits()[0].content, "too many interactions"));
  REQUIRE(fixture.diagnostics->contains_category("interaction.queue_full"));

  fixture.identities->release();
  REQUIRE(first->wait_for_edit_count(1, 2s));
  REQUIRE(queued->wait_for_edit_count(1, 2s));
  fixture.application->stop();
}
