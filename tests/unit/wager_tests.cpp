#include "sanguinius/wagers.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_wager_repository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace sanguinius;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

[[nodiscard]] IncomingInteraction base_interaction() {
  IncomingInteraction result;
  result.correlation_id = "wager-unit";
  result.interaction_id = 100;
  result.guild_id = 10;
  result.channel_id = 20;
  result.user_id = 30;
  result.username = "owner";
  result.display_name = "Owner";
  result.kind = InteractionKind::slash_command;
  result.command_name = "tarot";
  return result;
}

[[nodiscard]] WagerRecord sealed_record() {
  return WagerRecord{
      .wager_id = uuid(10),
      .state = WagerState::offered,
      .revision = 3,
      .guild_id = 10,
      .channel_id = 20,
      .creator_user_id = 30,
      .target_user_id = 31,
      .judge_user_id = std::nullopt,
      .creator_display_name = "Owner",
      .target_display_name = "Target",
      .judge_display_name = std::nullopt,
      .visibility = WagerVisibility::sealed,
      .resolution_policy = WagerResolutionPolicy::mutual,
      .proposition = "SECRET PROPOSITION",
      .stake = 10,
      .evidence_instructions = "SECRET EVIDENCE RULE",
      .outcome_window_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .offer_duration_ms = 86'400'000,
      .offer_expires_at_ms = 90'000,
      .outcome_due_at_ms = std::nullopt,
      .resolution_grace_until_ms = std::nullopt,
      .winner = std::nullopt,
      .terminal_reason = std::nullopt,
      .is_test = false,
      .created_at_ms = 1'000,
      .updated_at_ms = 2'000,
  };
}

class ServiceFixture {
public:
  explicit ServiceFixture(const bool test_mode = false)
      : clock{std::chrono::sys_seconds{std::chrono::seconds{1}}},
        service{repository,
                clock,
                ids,
                {},
                100,
                {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
                test_mode,
                diagnostics,
                [this] {
                  ++scheduler_wakes;
                  if (throw_scheduler_wake)
                    throw std::runtime_error{"injected scheduler wake failure"};
                },
                [this] {
                  ++outbox_wakes;
                  if (throw_outbox_wake)
                    throw std::runtime_error{"injected outbox wake failure"};
                },
                [this](const std::string_view event) {
                  ++observations;
                  last_event = event;
                  if (throw_observer)
                    throw std::runtime_error{"injected observer failure"};
                }} {}

  test::FakeWagerRepository repository;
  test::FakeClock clock;
  test::FakePersistentIdGenerator ids;
  test::FakeDiagnostics diagnostics;
  std::size_t scheduler_wakes{};
  std::size_t outbox_wakes{};
  std::size_t observations{};
  std::string last_event;
  bool throw_scheduler_wake{};
  bool throw_outbox_wake{};
  bool throw_observer{};
  TarotWagerService service;
};

} // namespace

TEST_CASE("wager policy enforces the schema-backed equal stake range",
          "[wager][policy]") {
  REQUIRE_NOTHROW(WagerPolicy{}.validate());
  auto policy = WagerPolicy{};
  policy.minimum_stake = 0;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.maximum_stake = 101;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.minimum_stake = 50;
  policy.maximum_stake = 49;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.default_outcome_hours = 24 * 7 + 1;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.resolution_grace_hours = 24 * 7 + 1;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
}

TEST_CASE("wager controls and modals are opaque versioned and bounded",
          "[wager][token][modal]") {
  const auto id = uuid(1);
  REQUIRE(parse_wager_component(std::string{wager_component_prefix} + id) ==
          id);
  REQUIRE(parse_wager_form(std::string{wager_form_prefix} + id) == id);
  REQUIRE(parse_wager_evidence_form(std::string{wager_evidence_prefix} + id) ==
          id);
  REQUIRE(parse_wager_outcome_form(std::string{wager_outcome_prefix} + id) ==
          id);
  REQUIRE(parse_wager_history(std::string{wager_history_prefix} + id) == id);
  REQUIRE_FALSE(parse_wager_component("sgw:2:" + id));
  REQUIRE_FALSE(parse_wager_component("sgw:1:not-a-uuid"));

  const auto wager_form = TarotWagerService::wager_form(id);
  REQUIRE(wager_form.custom_id == std::string{wager_form_prefix} + id);
  REQUIRE(wager_form.fields.size() == 3);
  REQUIRE(wager_form.fields[0].maximum_length == 500);
  REQUIRE(wager_form.fields[1].maximum_length == 3);
  REQUIRE(wager_form.fields[2].maximum_length == 500);
  const auto evidence_form = TarotWagerService::evidence_form(id);
  REQUIRE(evidence_form.fields.size() == 1);
  REQUIRE(evidence_form.fields.front().maximum_length == 1000);
  const auto outcome_form = TarotWagerService::outcome_form(id);
  REQUIRE(outcome_form.custom_id == std::string{wager_outcome_prefix} + id);
  REQUIRE(outcome_form.fields.size() == 1);
  REQUIRE(outcome_form.fields.front().minimum_length == 6);
  REQUIRE(outcome_form.fields.front().maximum_length == 7);
}

TEST_CASE("wager creation validates resolved human roles and explicit policy",
          "[wager][validation][privacy]") {
  ServiceFixture fixture;
  auto interaction = base_interaction();
  interaction.subcommand_name = "wager";
  interaction.command_options = {
      {"target", DiscordId{31}},
      {"visibility", std::string{"sealed"}},
      {"resolution", std::string{"designated"}},
      {"judge", DiscordId{32}},
      {"outcome-in", std::string{"72h"}},
  };
  interaction.resolved_users = {
      {.user_id = 31, .username = "target", .display_name = "Target"},
      {.user_id = 32, .username = "judge", .display_name = "Judge"},
  };
  REQUIRE_NOTHROW(fixture.service.create(interaction));
  REQUIRE(fixture.repository.create_request.has_value());
  REQUIRE(fixture.repository.create_request->target_user_id ==
          DiscordSnowflake{31});
  REQUIRE(fixture.repository.create_request->judge_user_id ==
          DiscordSnowflake{32});
  REQUIRE(fixture.repository.create_request->visibility ==
          WagerVisibility::sealed);
  REQUIRE(fixture.repository.create_request->resolution_policy ==
          WagerResolutionPolicy::designated);
  REQUIRE(fixture.repository.create_request->outcome_window_ms ==
          72 * 3'600'000);
  REQUIRE(fixture.repository.create_request->resolution_grace_ms ==
          48 * 3'600'000);
  REQUIRE_FALSE(fixture.repository.create_request->is_test);

  interaction.resolved_users[0].is_bot = true;
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);
  interaction.resolved_users[0].is_bot = false;
  interaction.command_options.erase(interaction.command_options.begin() + 3);
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);

  interaction.command_options = {{"target", DiscordId{31}},
                                 {"visibility", std::string{"private"}}};
  interaction.resolved_users = {
      {.user_id = 31, .username = "target", .display_name = "Target"}};
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);
  interaction.command_options = {{"target", DiscordId{31}},
                                 {"resolution", std::string{"automatic"}}};
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);
  interaction.command_options = {{"target", DiscordId{30}}};
  interaction.resolved_users = {
      {.user_id = 30, .username = "owner", .display_name = "Owner"}};
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);
}

TEST_CASE("only the owner can self-target when test mode is active",
          "[wager][test-mode][validation]") {
  ServiceFixture fixture{true};
  auto interaction = base_interaction();
  interaction.command_options = {{"target", DiscordId{30}}};
  interaction.resolved_users = {
      {.user_id = 30, .username = "owner", .display_name = "Owner"}};
  REQUIRE_NOTHROW(fixture.service.create(interaction));
  REQUIRE(fixture.repository.create_request->is_test);

  interaction.command_options = {{"target", DiscordId{30}},
                                 {"resolution", std::string{"designated"}},
                                 {"judge", DiscordId{32}}};
  interaction.resolved_users.push_back(
      {.user_id = 32, .username = "judge", .display_name = "Judge"});
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);

  interaction.command_options = {{"target", DiscordId{30}},
                                 {"resolution", std::string{"designated"}},
                                 {"judge", DiscordId{30}}};
  REQUIRE_NOTHROW(fixture.service.create(interaction));
  REQUIRE(fixture.repository.create_request->judge_user_id ==
          DiscordSnowflake{30});

  interaction.user_id = 31;
  interaction.command_options = {{"target", DiscordId{31}}};
  interaction.resolved_users = {
      {.user_id = 31, .username = "target", .display_name = "Target"}};
  REQUIRE_THROWS_AS(fixture.service.create(interaction), std::invalid_argument);
}

TEST_CASE(
    "wager preview parses one equal stake and rejects malformed modal input",
    "[wager][modal][validation]") {
  ServiceFixture fixture;
  fixture.repository.mutation_result = {
      .status = WagerMutationStatus::applied,
      .wager = sealed_record(),
      .controls = {},
      .committed_event_types = {},
      .public_delivery_created = false,
  };
  auto interaction = base_interaction();
  interaction.kind = InteractionKind::modal_submit;
  interaction.command_name.clear();
  interaction.custom_id = std::string{wager_form_prefix} + uuid(3);
  interaction.modal_fields = {{"proposition", "A bounded proposition"},
                              {"stake", "100"},
                              {"evidence_instructions", "  "}};
  const auto preview = fixture.service.preview(interaction);
  REQUIRE(fixture.repository.preview_request.has_value());
  REQUIRE(fixture.repository.preview_request->stake == 100);
  REQUIRE_FALSE(
      fixture.repository.preview_request->evidence_instructions.has_value());
  REQUIRE(preview.content.find("Offer expiry: 24 hours after confirmation") !=
          std::string::npos);
  REQUIRE(preview.content.find("Outcome window: 24 hours after acceptance") !=
          std::string::npos);
  REQUIRE(preview.content.find("timeout never awards Fate") !=
          std::string::npos);

  interaction.modal_fields[1].second = "101";
  REQUIRE_THROWS_AS(fixture.service.preview(interaction),
                    std::invalid_argument);
  interaction.modal_fields[1].second = "10 Fate";
  REQUIRE_THROWS_AS(fixture.service.preview(interaction),
                    std::invalid_argument);
  interaction.modal_fields[1].second = "10";
  interaction.modal_fields[0].second = std::string(501, 'x');
  REQUIRE_THROWS_AS(fixture.service.preview(interaction),
                    std::invalid_argument);
}

TEST_CASE("wager outcome modal resolves its opaque control and bounded winner",
          "[wager][modal][outcome][validation]") {
  ServiceFixture fixture;
  fixture.repository.mutation_result = {
      .status = WagerMutationStatus::applied,
      .wager = sealed_record(),
      .controls = {},
      .committed_event_types = {"tarot.wager_outcome_submitted.v1"},
      .public_delivery_created = false,
  };
  auto interaction = base_interaction();
  interaction.kind = InteractionKind::modal_submit;
  interaction.command_name.clear();
  interaction.custom_id = std::string{wager_outcome_prefix} + uuid(4);
  interaction.modal_fields = {{"winner", "target"}};

  REQUIRE_NOTHROW(fixture.service.outcome(interaction));
  REQUIRE(fixture.repository.outcome_request.has_value());
  REQUIRE(fixture.repository.outcome_request->token_id == uuid(4));
  REQUIRE(fixture.repository.outcome_request->wager_id.empty());
  REQUIRE(fixture.repository.outcome_request->winner == WagerRole::target);

  interaction.modal_fields.push_back({"extra", "creator"});
  REQUIRE_THROWS_AS(fixture.service.outcome(interaction),
                    std::invalid_argument);
  interaction.modal_fields = {{"winner", "neither"}};
  REQUIRE_THROWS_AS(fixture.service.outcome(interaction),
                    std::invalid_argument);
}

TEST_CASE("forbidden wager results never render sealed terms",
          "[wager][privacy][rendering]") {
  ServiceFixture fixture;
  fixture.repository.mutation_result = {
      .status = WagerMutationStatus::forbidden,
      .wager = sealed_record(),
      .controls = {},
      .committed_event_types = {},
      .public_delivery_created = false,
  };
  auto interaction = base_interaction();
  interaction.command_options = {{"reference", uuid(10)},
                                 {"action", std::string{"accept"}}};
  const auto message = fixture.service.action(interaction);
  REQUIRE(message.content == "That wager action is not available to you.");
  REQUIRE(message.content.find("SECRET") == std::string::npos);
  REQUIRE_FALSE(message.embed.has_value());
}

TEST_CASE("state-dependent wager rejections render their rejection status",
          "[wager][rendering][status]") {
  ServiceFixture fixture;
  auto interaction = base_interaction();
  interaction.command_options = {{"reference", uuid(10)},
                                 {"action", std::string{"accept"}}};

  fixture.repository.mutation_result = {
      .status = WagerMutationStatus::insufficient_funds,
      .wager = sealed_record(),
      .controls = {},
      .committed_event_types = {},
      .public_delivery_created = false,
  };
  REQUIRE(fixture.service.action(interaction).content.find("enough Fate") !=
          std::string::npos);

  fixture.repository.mutation_result.status = WagerMutationStatus::stale;
  REQUIRE(
      fixture.service.action(interaction).content.find("control is stale") !=
      std::string::npos);

  fixture.repository.mutation_result.status =
      WagerMutationStatus::invalid_state;
  REQUIRE(fixture.service.action(interaction)
              .content.find("no longer in a state") != std::string::npos);
}

TEST_CASE("authorized wager history exposes actionable exact details within "
          "Discord bounds",
          "[wager][history][rendering][bounds]") {
  ServiceFixture fixture;
  auto record = sealed_record();
  record.state = WagerState::disputed;
  record.outcome_due_at_ms = 86'401'000;
  record.resolution_grace_until_ms = 259'201'000;
  fixture.repository.history_result = {
      .status = WagerMutationStatus::applied,
      .wagers = {record},
      .outcomes = {"creator submitted creator as winner.",
                   "target submitted target as winner."},
      .evidence = std::vector<std::string>(10, std::string(1'000, 'e')),
      .evidence_total_count = 10,
      .previous_cursor_id = std::nullopt,
      .next_cursor_id = std::nullopt,
      .page = 0,
      .total = 1,
      .controls = {},
      .exact = true,
  };
  auto interaction = base_interaction();
  interaction.subcommand_name = "wagers";
  interaction.command_options = {{"reference", record.wager_id}};
  const auto message = fixture.service.wagers(interaction);
  REQUIRE(message.content.find(record.wager_id) != std::string::npos);
  REQUIRE(message.content.find("SECRET PROPOSITION") != std::string::npos);
  REQUIRE(message.content.find("creator submitted creator as winner") !=
          std::string::npos);
  REQUIRE(message.content.find("Additional private evidence") !=
          std::string::npos);
  REQUIRE(message.content.size() <= 1'900);

  fixture.repository.history_result.exact = false;
  fixture.repository.history_result.evidence.clear();
  fixture.repository.history_result.outcomes.clear();
  interaction.command_options.clear();
  const auto list = fixture.service.wagers(interaction);
  REQUIRE(list.content.find(record.wager_id) != std::string::npos);
}

TEST_CASE("wager observer failure is contained after durable commit",
          "[wager][observer][failure]") {
  ServiceFixture fixture;
  fixture.repository.mutation_result = {
      .status = WagerMutationStatus::applied,
      .wager = sealed_record(),
      .controls = {},
      .committed_event_types = {"tarot.wager_drafted.v1"},
      .public_delivery_created = true,
  };
  fixture.throw_outbox_wake = true;
  fixture.throw_scheduler_wake = true;
  fixture.throw_observer = true;
  auto interaction = base_interaction();
  interaction.command_options = {{"target", DiscordId{31}}};
  interaction.resolved_users = {
      {.user_id = 31, .username = "target", .display_name = "Target"}};
  const auto message = fixture.service.create(interaction);
  REQUIRE(message.content.find("SECRET PROPOSITION") != std::string::npos);
  REQUIRE(message.content.find(uuid(10)) != std::string::npos);
  REQUIRE(fixture.scheduler_wakes == 1);
  REQUIRE(fixture.outbox_wakes == 1);
  REQUIRE(fixture.observations == 1);
  REQUIRE(fixture.last_event == "tarot.wager_drafted.v1");
  REQUIRE(fixture.diagnostics.contains_category("wager.post_commit"));
}
