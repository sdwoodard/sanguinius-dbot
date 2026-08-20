#include "sanguinius/tarot.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_random.hpp"
#include "support/fake_tarot_repository.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

[[nodiscard]] sanguinius::IncomingInteraction
tarot_interaction(const sanguinius::DiscordId interaction_id = 100) {
  sanguinius::IncomingInteraction interaction;
  interaction.correlation_id = "tarot-unit";
  interaction.interaction_id = interaction_id;
  interaction.guild_id = 10;
  interaction.channel_id = 20;
  interaction.user_id = 30;
  interaction.username = "owner";
  interaction.display_name = "Owner";
  return interaction;
}

} // namespace

TEST_CASE("Tarot policy accepts Stephen's defaults and rejects unsafe bounds",
          "[tarot][policy]") {
  sanguinius::TarotPolicy{}.validate();

  auto policy = sanguinius::TarotPolicy{};
  policy.grace_target = policy.grace_threshold;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.trial_reward_min = policy.trial_reward_max + 1;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.starting_fate = 0;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
  policy = {};
  policy.trial_cooldown_hours = 8'761;
  REQUIRE_THROWS_AS(policy.validate(), std::invalid_argument);
}

TEST_CASE("Tarot component tokens are opaque and versioned", "[tarot][token]") {
  const auto token = std::string{sanguinius::tarot_component_prefix} +
                     "00000000-0000-4000-8000-000000000001";
  REQUIRE(sanguinius::parse_tarot_component(token) ==
          "00000000-0000-4000-8000-000000000001");
  REQUIRE_FALSE(sanguinius::parse_tarot_component("sgt:1:not-a-uuid"));
  REQUIRE_FALSE(sanguinius::parse_tarot_component(
      "sgt:2:00000000-0000-4000-8000-000000000001"));
}

TEST_CASE("deterministic random rewards exercise exact lower and upper bounds",
          "[tarot][random]") {
  sanguinius::test::FakeRandom lower{{0, 0}};
  REQUIRE(lower.uniform(3) == 0);
  REQUIRE(lower.uniform(11) == 0);
  sanguinius::test::FakeRandom upper{{2, 10}};
  REQUIRE(upper.uniform(3) == 2);
  REQUIRE(upper.uniform(11) == 10);
}

TEST_CASE("Tarot observer failures are contained after committed work",
          "[tarot][observer][failure]") {
  sanguinius::test::FakeTarotRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeRandom random{{}};
  sanguinius::test::FakeDiagnostics diagnostics;
  bool outbox_woken{};
  std::size_t observations{};
  sanguinius::TarotService service{
      repository,
      clock,
      ids,
      random,
      {},
      {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      false,
      diagnostics,
      [&] { outbox_woken = true; },
      [&](const std::string_view) {
        ++observations;
        throw std::runtime_error{"injected observer failure"};
      }};
  service.initialize();

  sanguinius::IncomingInteraction interaction;
  interaction.correlation_id = "tarot-observer";
  interaction.interaction_id = 100;
  interaction.guild_id = 10;
  interaction.channel_id = 20;
  interaction.user_id = 30;
  interaction.username = "owner";
  interaction.display_name = "Owner";
  const auto message = service.balance(interaction);

  REQUIRE(message.content == "Your Fate balance is 100.");
  REQUIRE(repository.ensure_calls == 1);
  REQUIRE(observations == 1);
  REQUIRE(diagnostics.contains_category("tarot.observer"));
  REQUIRE_FALSE(outbox_woken);
}

TEST_CASE("Tarot startup fails closed on an invalid ledger",
          "[tarot][startup][invariant]") {
  sanguinius::test::FakeTarotRepository repository;
  repository.invariants_valid = false;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeRandom random{{}};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::TarotService service{
      repository,
      clock,
      ids,
      random,
      {},
      {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      false,
      diagnostics,
      [] {}};

  REQUIRE_THROWS_AS(service.initialize(), std::runtime_error);
  REQUIRE(repository.initialize_calls == 1);
}

TEST_CASE("Tarot admin audit reasons must contain non-whitespace text",
          "[tarot][admin][audit]") {
  sanguinius::test::FakeTarotRepository repository;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeRandom random{{}};
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::TarotService service{
      repository,
      clock,
      ids,
      random,
      {},
      {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      true,
      diagnostics,
      [] {}};
  service.initialize();

  auto adjustment = tarot_interaction(150);
  adjustment.command_options = {{"amount", std::int64_t{1}},
                                {"reason", std::string{" \t\n"}}};
  REQUIRE_THROWS_AS(service.adjust(adjustment), std::invalid_argument);
  REQUIRE(repository.current_balance == 100);

  auto reversal = tarot_interaction(151);
  reversal.command_options = {
      {"transaction", std::string{"00000000-0000-4000-8000-000000000150"}},
      {"reason", std::string{" \r\n"}}};
  REQUIRE_THROWS_AS(service.reverse(reversal), std::invalid_argument);
  REQUIRE(repository.current_balance == 100);
}

TEST_CASE("Trial replay returns the persisted draw without sampling again",
          "[tarot][trial][random][duplicate]") {
  sanguinius::test::FakeTarotRepository repository;
  repository.current_balance = 40;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeRandom random{{2, 10}};
  sanguinius::test::FakeDiagnostics diagnostics;
  std::vector<std::string> observations;
  sanguinius::TarotService service{
      repository,
      clock,
      ids,
      random,
      {},
      {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      true,
      diagnostics,
      [] {},
      [&](const std::string_view event_type) {
        observations.emplace_back(event_type);
      }};
  service.initialize();
  const auto interaction = tarot_interaction(200);

  const auto first = service.start_trial(interaction);
  REQUIRE(first.buttons.size() == 4);
  REQUIRE(repository.last_reward == 15);
  REQUIRE(repository.last_prompt_variant == 2);
  REQUIRE(random.call_count() == 2);

  const auto replay = service.start_trial(interaction);
  REQUIRE(replay.content == first.content);
  REQUIRE(replay.buttons.size() == first.buttons.size());
  for (std::size_t index = 0; index < first.buttons.size(); ++index)
    REQUIRE(replay.buttons[index].custom_id == first.buttons[index].custom_id);
  REQUIRE(random.call_count() == 2);
  REQUIRE(observations == std::vector<std::string>{"tarot.starting_grant.v1",
                                                   "tarot.trial_started.v1"});
}

TEST_CASE("Tarot observers receive the committed lost-eligibility event",
          "[tarot][observer][recovery]") {
  sanguinius::test::FakeTarotRepository repository;
  repository.completion_result = {
      .status = sanguinius::TarotRecoveryStatus::lost_eligibility,
      .kind = sanguinius::TarotRecoveryKind::trial,
      .visibility = sanguinius::TarotVisibility::private_result,
      .claim_id = "00000000-0000-4000-8000-000000000201",
      .balance = 50,
      .reward = std::nullopt,
      .cooldown_until_ms = std::nullopt,
      .prompt_variant = std::nullopt,
      .custom_ids = {},
      .mutation_created = true,
      .public_delivery_created = false,
      .committed_event_types = {"tarot.recovery_eligibility_lost.v1"}};
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeRandom random{{}};
  sanguinius::test::FakeDiagnostics diagnostics;
  std::vector<std::string> observations;
  sanguinius::TarotService service{
      repository,
      clock,
      ids,
      random,
      {},
      {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      false,
      diagnostics,
      [] {},
      [&](const std::string_view event_type) {
        observations.emplace_back(event_type);
      }};
  service.initialize();
  auto interaction = tarot_interaction(201);
  interaction.custom_id = "sgt:1:00000000-0000-4000-8000-000000000200";

  const auto message = service.apply_component(interaction);
  REQUIRE(message.content.find("eligibility changed") != std::string::npos);
  REQUIRE(observations ==
          std::vector<std::string>{"tarot.recovery_eligibility_lost.v1"});
}
