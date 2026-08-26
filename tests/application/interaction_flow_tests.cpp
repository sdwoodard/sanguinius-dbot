#include "support/application_fixture.hpp"

#include "sanguinius/pending_notice.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
      .tarot_policy = {},
      .wager_policy = {},
      .tarot_house_policy = {.house_enabled = false,
                             .integration_enabled = false},
      .tarot_deck_catalog = std::nullopt,
      .tarot_house_catalog = std::nullopt,
      .build = {"test-version", "test-revision"},
      .persistence = {true, 3, 3, "3.53.4",
                      "00000000-0000-4000-8000-000000000001"},
      .instance_id = "00000000-0000-4000-8000-000000000001",
      .hostname = "test-host",
      .process_id = 123,
      .message_queue_capacity = 64,
      .ai_queue_capacity = 64,
      .ai_worker_count = 1,
      .interaction_queue_capacity = interaction_capacity,
      .durable_delivery_receipt_wait = std::chrono::milliseconds{100},
      .speech = {},
      .static_speech_assets = {.entrance = sanguinius::make_vox_proof_chime(),
                               .error = sanguinius::make_vox_proof_chime(),
                               .farewell = sanguinius::make_vox_proof_chime()},
  };
}

[[nodiscard]] sanguinius::ApplicationOptions chronicle_options() {
  auto options = admin_options();
  options.features.chronicle_enabled = true;
  options.persistence.schema_version = 5;
  options.persistence.target_schema_version = 5;
  return options;
}

[[nodiscard]] sanguinius::ApplicationOptions tarot_options() {
  auto options = admin_options();
  options.controls.test_mode = true;
  options.features.tarot_enabled = true;
  options.persistence.schema_version = 11;
  options.persistence.target_schema_version = 11;
  return options;
}

[[nodiscard]] sanguinius::ApplicationOptions tarot_house_options() {
  auto options = tarot_options();
  options.tarot_house_policy.house_enabled = true;
  options.tarot_house_policy.integration_enabled = true;
  const auto config = std::filesystem::path{__FILE__}
                          .parent_path()
                          .parent_path()
                          .parent_path() /
                      "config";
  options.tarot_deck_catalog =
      sanguinius::load_tarot_deck_catalog(config / "emperor-tarot-v1.json");
  options.tarot_house_catalog = sanguinius::load_tarot_house_catalog(
      config / "tarot-house-v1.json", options.tarot_house_policy.profit_cap);
  return options;
}

[[nodiscard]] sanguinius::ApplicationOptions vox_options() {
  auto options = admin_options();
  options.features.vox_enabled = true;
  options.persistence.schema_version = 13;
  options.persistence.target_schema_version = 14;
  return options;
}

[[nodiscard]] sanguinius::ApplicationOptions vox_narration_options() {
  auto options = vox_options();
  options.features.vox_narration_enabled = true;
  return options;
}

[[nodiscard]] sanguinius::WagerRecord application_wager() {
  return {
      .wager_id = "00000000-0000-4000-8000-000000000780",
      .state = sanguinius::WagerState::draft,
      .revision = 1,
      .guild_id = 10,
      .channel_id = 20,
      .creator_user_id = 30,
      .target_user_id = 31,
      .judge_user_id = std::nullopt,
      .creator_display_name = "Owner",
      .target_display_name = "Target",
      .judge_display_name = std::nullopt,
      .visibility = sanguinius::WagerVisibility::sealed,
      .resolution_policy = sanguinius::WagerResolutionPolicy::mutual,
      .proposition = std::nullopt,
      .stake = std::nullopt,
      .evidence_instructions = std::nullopt,
      .outcome_window_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .offer_duration_ms = std::nullopt,
      .offer_expires_at_ms = std::nullopt,
      .outcome_due_at_ms = std::nullopt,
      .resolution_grace_until_ms = std::nullopt,
      .winner = std::nullopt,
      .terminal_reason = std::nullopt,
      .is_test = false,
      .created_at_ms = 0,
      .updated_at_ms = 0,
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

[[nodiscard]] sanguinius::EventJournalEntry
durable_event(std::string event_id, std::string event_type, std::string key) {
  return sanguinius::EventJournalEntry{
      .event_id = std::move(event_id),
      .event_type = std::move(event_type),
      .aggregate_type = "owner_test",
      .aggregate_id = "handler-validation",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 0,
      .recorded_at_ms = 0,
      .correlation_id = "handler-validation",
      .causation_id = std::nullopt,
      .idempotency_key = std::move(key),
      .payload_json = "{}",
  };
}

[[nodiscard]] sanguinius::OutboxEnqueue
durable_outbox(std::string outbox_id, std::string kind, std::string key) {
  const auto nonce = sanguinius::discord_nonce_from_uuid(outbox_id);
  return sanguinius::OutboxEnqueue{
      .outbox_id = std::move(outbox_id),
      .kind = std::move(kind),
      .aggregate_type = "owner_test",
      .aggregate_id = "handler-validation",
      .target_guild_id = 10,
      .target_channel_id = 20,
      .target_user_id = std::nullopt,
      .available_at_ms = 0,
      .max_attempts = 5,
      .idempotency_key = std::move(key),
      .provider_nonce = nonce,
      .created_at_ms = 0,
  };
}

} // namespace

TEST_CASE("Vox interactions connect play once reconnect and leave",
          "[application][interaction][vox]") {
  sanguinius::test::ApplicationFixture fixture{vox_options()};
  fixture.application->start();
  REQUIRE(fixture.vox->recover_calls() == 1);
  REQUIRE(fixture.discord->command_catalog().version == 13);

  auto summon = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(summon, "vox", "summon", 900));
  REQUIRE(summon->wait_for_edit_count(1, 2s));
  REQUIRE(summon->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(fixture.voice_gateway->wait_for_connects(1, 2s));

  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(fixture.voice_gateway->wait_for_sends(1, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(fixture.voice_gateway->send_count() == 1);

  auto early_say =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto early_say_request = slash(early_say, "vox", "say", 905);
  early_say_request.command_options = {
      {"text", std::string{"Wait for the entrance proof."}}};
  fixture.discord->emit(std::move(early_say_request));
  REQUIRE(early_say->wait_for_edit_count(1, 2s));
  REQUIRE(contains(early_say->edits()[0].content, "queued"));
  REQUIRE(fixture.voice_gateway->send_count() == 1);

  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::track_marker);
  REQUIRE(
      fixture.vox->wait_for_fixture(sanguinius::VoxFixtureState::played, 2s));
  REQUIRE(fixture.voice_gateway->wait_for_sends(2, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::track_marker);

  auto status = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(status, "vox", "status", 901));
  REQUIRE(status->wait_for_edit_count(1, 2s));
  REQUIRE(status->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::public_message});
  REQUIRE(contains(status->edits()[0].content, "<#40>"));
  REQUIRE(contains(status->edits()[0].content, "played"));

  auto duplicate =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(duplicate, "vox", "summon", 902));
  REQUIRE(duplicate->wait_for_edit_count(1, 2s));
  REQUIRE(contains(duplicate->edits()[0].content, "active"));
  REQUIRE(fixture.voice_gateway->send_count() == 2);

  auto reconnect =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto reconnect_request = slash(reconnect, "sang-admin", "disconnect", 903);
  reconnect_request.subcommand_group_name = "vox";
  fixture.discord->emit(std::move(reconnect_request));
  REQUIRE(reconnect->wait_for_edit_count(1, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(fixture.voice_gateway->wait_for_connects(2, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(fixture.vox->wait_for_state(sanguinius::VoxState::ready, 2s));
  REQUIRE(fixture.voice_gateway->send_count() == 2);

  auto leave = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  const auto disconnects_before_leave =
      fixture.voice_gateway->disconnect_count();
  fixture.discord->emit(slash(leave, "vox", "leave", 904));
  REQUIRE(leave->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.vox->wait_for_state(sanguinius::VoxState::leaving, 2s));
  REQUIRE(fixture.voice_gateway->wait_for_sends(3, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::track_marker);
  REQUIRE(fixture.voice_gateway->wait_for_disconnects(
      disconnects_before_leave + 1, 2s));
  fixture.voice_gateway->emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(fixture.vox->wait_for_state(sanguinius::VoxState::inactive, 2s));
  REQUIRE(fixture.voice_gateway->disconnect_count() >=
          disconnects_before_leave + 1);

  bool voice_fenced_before_cluster_shutdown = false;
  fixture.discord->set_shutdown_observer([&] {
    const auto lifecycle = fixture.voice_gateway->lifecycle();
    voice_fenced_before_cluster_shutdown =
        std::find(lifecycle.begin(), lifecycle.end(), "voice.shutdown") !=
        lifecycle.end();
  });
  fixture.application->stop();
  REQUIRE(voice_fenced_before_cluster_shutdown);
}

TEST_CASE("disabled Vox omits commands and callbacks but recovers stale state",
          "[application][interaction][vox][disabled][recovery]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.vox->seed_active();

  fixture.application->start();

  REQUIRE(fixture.vox->recover_calls() == 1);
  REQUIRE(fixture.vox->wait_for_state(sanguinius::VoxState::inactive, 2s));
  const auto &commands = fixture.discord->command_catalog().commands;
  REQUIRE(std::ranges::none_of(
      commands, [](const auto &command) { return command.name == "vox"; }));
  REQUIRE(fixture.voice_gateway->lifecycle().empty());

  fixture.application->stop();
  REQUIRE(fixture.voice_gateway->lifecycle().empty());
}

TEST_CASE("application startup fails closed on wager escrow invariant failure",
          "[application][wager][startup][invariant]") {
  sanguinius::test::ApplicationFixture fixture{tarot_options()};
  fixture.wagers->invariant_result.valid = false;
  fixture.wagers->invariant_result.open_funded_obligation_count = 1;
  fixture.wagers->invariant_result.open_funded_obligation_amount = 20;
  fixture.wagers->invariant_result.escrow_balance = 0;
  REQUIRE_THROWS_AS(fixture.application->start(), std::runtime_error);
  REQUIRE(fixture.instances->stops().size() == 1);
  REQUIRE(fixture.instances->stops().front().reason ==
          sanguinius::ApplicationStopReason::startup_failure);
}

TEST_CASE("Tarot commands preserve private authority and public standings",
          "[application][interaction][tarot][privacy]") {
  sanguinius::test::ApplicationFixture fixture{tarot_options()};
  fixture.application->start();
  REQUIRE(fixture.tarot->initialize_calls == 1);
  REQUIRE(fixture.discord->command_catalog().version == 13);
  REQUIRE(fixture.discord->command_catalog().commands.size() == 3);

  auto balance = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(balance, "tarot", "balance", 160));
  REQUIRE(balance->wait_for_edit_count(1, 2s));
  REQUIRE(balance->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(balance->edits()[0].content, "100"));

  auto standings =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(standings, "tarot", "standings", 161));
  REQUIRE(standings->wait_for_edit_count(1, 2s));
  REQUIRE(standings->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::public_message});
  REQUIRE(contains(standings->edits()[0].content, "Owner"));

  fixture.tarot->current_balance = 40;
  auto trial = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(trial, "tarot", "trial", 162));
  REQUIRE(trial->wait_for_edit_count(1, 2s));
  REQUIRE(trial->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(trial->edits()[0].buttons.size() == 4);
  REQUIRE(fixture.tarot->last_reward == 5);
  REQUIRE(fixture.tarot->last_prompt_variant == 0);

  auto adjust = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto adjustment = slash(adjust, "sang-admin", "adjust", 163);
  adjustment.subcommand_group_name = "tarot";
  adjustment.command_options = {{"amount", std::int64_t{-40}},
                                {"reason", std::string{"live flow setup"}}};
  fixture.discord->emit(std::move(adjustment));
  REQUIRE(adjust->wait_for_edit_count(1, 2s));
  REQUIRE(contains(adjust->edits()[0].content, "[TEST]"));
  REQUIRE(fixture.tarot->current_balance == 0);

  auto nonowner =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto rejected = slash(nonowner, "sang-admin", "adjust", 164);
  rejected.subcommand_group_name = "tarot";
  rejected.user_id = 31;
  rejected.command_options = {{"amount", std::int64_t{1}},
                              {"reason", std::string{"must reject"}}};
  fixture.discord->emit(std::move(rejected));
  REQUIRE(nonowner->replies().size() == 1);
  REQUIRE(contains(nonowner->replies()[0].first.content, "owner-only"));

  auto privacy = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(privacy, "sanguinius", "privacy", 165));
  REQUIRE(privacy->wait_for_edit_count(1, 2s));
  REQUIRE(contains(privacy->edits()[0].content, "immutable financial audit"));

  auto health = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(health, "sang-admin", "health", 166));
  REQUIRE(health->wait_for_edit_count(1, 2s));
  REQUIRE(contains(health->edits()[0].content, "tarot_invariants=ok"));
  fixture.application->stop();

  auto disabled_options = tarot_options();
  disabled_options.controls.test_mode = false;
  sanguinius::test::ApplicationFixture disabled{disabled_options};
  disabled.application->start();
  auto gated = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto gated_request = slash(gated, "sang-admin", "adjust", 167);
  gated_request.subcommand_group_name = "tarot";
  gated_request.command_options = {{"amount", std::int64_t{1}},
                                   {"reason", std::string{"must reject"}}};
  disabled.discord->emit(std::move(gated_request));
  REQUIRE(gated->replies().size() == 1);
  REQUIRE(contains(gated->replies()[0].first.content, "test mode"));
  disabled.application->stop();
}

TEST_CASE("enabled Tarot deck and House routes keep command status ephemeral",
          "[application][interaction][tarot][house][privacy]") {
  sanguinius::test::ApplicationFixture fixture{tarot_house_options()};
  fixture.tarot_integration->retry_result = true;
  fixture.application->start();
  REQUIRE(fixture.tarot_catalogs->install_calls == 1);
  REQUIRE(fixture.tarot_house->reconcile_calls == 1);
  REQUIRE(fixture.tarot_house->schedule_calls == 1);
  REQUIRE(fixture.tarot_integration->schedule_calls == 1);

  auto draw = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(draw, "tarot", "draw", 180));
  REQUIRE(draw->wait_for_edit_count(1, 2s));
  REQUIRE(draw->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(draw->edits()[0].content, "durably queued"));
  REQUIRE(fixture.tarot_draws->last_visibility ==
          sanguinius::TarotVisibility::public_result);
  REQUIRE_FALSE(fixture.tarot_draws->last_is_test);
  REQUIRE(fixture.tarot_house->observe_draw_calls == 1);
  REQUIRE(fixture.tarot_integration->scan_calls == 1);
  REQUIRE_FALSE(fixture.tarot_integration->last_sink_policy.chronicle_enabled);

  auto offers = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto offers_request = slash(offers, "tarot", "offers", 181);
  offers_request.subcommand_group_name = "house";
  fixture.discord->emit(std::move(offers_request));
  REQUIRE(offers->wait_for_edit_count(1, 2s));
  REQUIRE(offers->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(offers->edits()[0].content, "Returning Dawn"));
  REQUIRE(contains(offers->edits()[0].content, "1:1 profit"));
  REQUIRE(fixture.tarot_house->availability_calls == 4);

  auto test_draw =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto test_request = slash(test_draw, "sang-admin", "draw-test", 182);
  test_request.subcommand_group_name = "tarot";
  fixture.discord->emit(std::move(test_request));
  REQUIRE(test_draw->wait_for_edit_count(1, 2s));
  REQUIRE(test_draw->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(test_draw->edits()[0].content, "[TEST]"));
  REQUIRE(fixture.tarot_draws->last_bypass_cooldown);
  REQUIRE(fixture.tarot_draws->last_is_test);
  REQUIRE(fixture.tarot_draws->last_visibility ==
          sanguinius::TarotVisibility::public_result);

  auto resolve = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto resolve_request = slash(resolve, "sang-admin", "house-resolve", 184);
  resolve_request.subcommand_group_name = "tarot";
  resolve_request.command_options = {
      {"reference", std::string{"00000000-0000-4000-8000-000000000902"}},
      {"outcome", std::string{"no"}},
      {"reason", std::string{"Deterministic test observation"}}};
  fixture.discord->emit(std::move(resolve_request));
  REQUIRE(resolve->wait_for_edit_count(1, 2s));
  REQUIRE(resolve->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(fixture.tarot_house->resolve_calls == 1);
  REQUIRE(fixture.tarot_house->last_resolve_test_mode);

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto retry_request = slash(retry, "sang-admin", "integration-retry", 183);
  retry_request.subcommand_group_name = "tarot";
  retry_request.command_options = {
      {"reference", std::string{"00000000-0000-4000-8000-000000000901"}}};
  fixture.discord->emit(std::move(retry_request));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(retry->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(fixture.tarot_integration->retry_calls == 1);
  fixture.application->stop();
}

TEST_CASE("Vox speech mute voice and simulated failure commands stay private",
          "[application][interaction][vox][speech]") {
  sanguinius::test::ApplicationFixture fixture{vox_options()};
  fixture.application->start();
  fixture.vox->seed_active();

  auto say = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto say_request = slash(say, "vox", "say", 910);
  say_request.command_options = {{"text", std::string{"Hold fast."}}};
  fixture.discord->emit(std::move(say_request));
  REQUIRE(say->wait_for_edit_count(1, 2s));
  REQUIRE(say->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(say->edits()[0].content, "queued"));

  auto voice = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(voice, "vox", "voice", 911));
  REQUIRE(voice->wait_for_edit_count(1, 2s));
  REQUIRE(contains(voice->edits()[0].content, "onyx"));

  auto mute = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto mute_request = slash(mute, "vox", "mute", 912);
  mute_request.command_options = {{"duration", std::string{"session"}}};
  fixture.discord->emit(std::move(mute_request));
  REQUIRE(mute->wait_for_edit_count(1, 2s));
  REQUIRE(contains(mute->edits()[0].content, "muted"));
  REQUIRE(fixture.vox->wait_for_state(sanguinius::VoxState::muted, 2s));

  auto speech_test =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto test_request = slash(speech_test, "sang-admin", "speech-test", 913);
  test_request.subcommand_group_name = "vox";
  test_request.command_options = {
      {"scenario", std::string{"provider-failure"}}};
  fixture.discord->emit(std::move(test_request));
  REQUIRE(speech_test->wait_for_edit_count(1, 2s));
  REQUIRE(contains(speech_test->edits()[0].content, "without a provider"));
  REQUIRE(fixture.vox->command_receipt_count() == 3);

  auto stale =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto stale_request = slash(stale, "sang-admin", "speech-test", 914);
  stale_request.subcommand_group_name = "vox";
  stale_request.command_options = {
      {"scenario", std::string{"narration-stale"}}};
  fixture.discord->emit(std::move(stale_request));
  REQUIRE(stale->wait_for_edit_count(1, 2s));
  REQUIRE(contains(stale->edits()[0].content, "narration-stale"));
  REQUIRE(contains(stale->edits()[0].content, "without a provider"));
  REQUIRE(fixture.vox->command_receipt_count() == 4);

  auto speech_test_replay =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto replay_request =
      slash(speech_test_replay, "sang-admin", "speech-test", 913);
  replay_request.subcommand_group_name = "vox";
  replay_request.command_options = {
      {"scenario", std::string{"provider-failure"}}};
  fixture.discord->emit(std::move(replay_request));
  REQUIRE(speech_test_replay->wait_for_edit_count(1, 2s));
  REQUIRE(
      contains(speech_test_replay->edits()[0].content, "without a provider"));
  REQUIRE(fixture.vox->command_receipt_count() == 4);

  auto altered_replay =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto altered_request =
      slash(altered_replay, "sang-admin", "speech-test", 913);
  altered_request.subcommand_group_name = "vox";
  altered_request.command_options = {{"scenario", std::string{"budget-limit"}}};
  fixture.discord->emit(std::move(altered_request));
  REQUIRE(altered_replay->wait_for_edit_count(1, 2s));
  REQUIRE_FALSE(
      contains(altered_replay->edits()[0].content, "without a provider"));
  REQUIRE(fixture.vox->command_receipt_count() == 4);

  fixture.application->stop();
}

TEST_CASE("Vox narration owner controls are private and idempotent",
          "[application][interaction][vox][narration][idempotency]") {
  sanguinius::test::ApplicationFixture fixture{vox_narration_options()};
  const std::string event_id{"00000000-0000-4000-8000-000000000950"};
  fixture.vox_narration->set_candidate(
      {.intent_id = {},
       .revision = 0,
       .source_event_id = event_id,
       .event_type = "tarot.draw_created.v1",
       .feature = sanguinius::VoxNarrationFeature::tarot,
       .guild_id = "10",
       .channel_id = "20",
       .safe_input = "Public card: The Angel. Public theme: hope.",
       .fallback_line = "The Angel turns toward hope.",
       .rank = 70,
       .created_at_ms = 0,
       .expires_at_ms = 120'000,
       .session_id = {},
       .counterpart_outbox_id = std::nullopt,
       .counterpart_required = true,
       .is_test = true});
  fixture.vox_narration->set_recent(
      {{.intent_id = "00000000-0000-4000-8000-000000000951",
        .event_type = "tarot.draw_created.v1",
        .feature = "tarot",
        .state = "suppressed",
        .reason = "session_budget",
        .created_at_ms = 1}});
  fixture.application->start();

  const auto narration_request =
      [&](const auto &responder, const std::string_view command,
          const sanguinius::DiscordId id, const std::string &reference) {
        auto request = slash(responder, "sang-admin", command, id);
        request.subcommand_group_name = "vox";
        request.command_options = {{"reference", reference}};
        fixture.discord->emit(std::move(request));
      };

  auto preview = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  narration_request(preview, "narration-preview", 920, event_id);
  REQUIRE(preview->wait_for_edit_count(1, 2s));
  REQUIRE(preview->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  CHECK(contains(preview->edits()[0].content, "feature tarot, rank 70"));
  CHECK(fixture.vox_narration->receipt_count() == 1);

  auto replay = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  narration_request(replay, "narration-preview", 920, event_id);
  REQUIRE(replay->wait_for_edit_count(1, 2s));
  CHECK(replay->edits()[0].content == preview->edits()[0].content);
  CHECK(fixture.vox_narration->receipt_count() == 1);

  auto altered = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  narration_request(altered, "narration-preview", 920,
                    "00000000-0000-4000-8000-000000000999");
  REQUIRE(altered->wait_for_edit_count(1, 2s));
  CHECK(contains(altered->edits()[0].content, "could not complete"));
  CHECK(fixture.vox_narration->receipt_count() == 1);

  auto enqueue = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  narration_request(enqueue, "narration-enqueue", 921, event_id);
  REQUIRE(enqueue->wait_for_edit_count(1, 2s));
  CHECK(contains(enqueue->edits()[0].content, "all freshness"));
  CHECK(fixture.vox_narration->receipt_count() == 2);
  CHECK(fixture.vox_narration->enqueue_count() == 1);
  REQUIRE(fixture.vox_narration->last_enqueue_reference());
  CHECK(*fixture.vox_narration->last_enqueue_reference() == event_id);

  auto recent = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto recent_request = slash(recent, "sang-admin", "narration-recent", 922);
  recent_request.subcommand_group_name = "vox";
  fixture.discord->emit(std::move(recent_request));
  REQUIRE(recent->wait_for_edit_count(1, 2s));
  CHECK(contains(recent->edits()[0].content, "tarot/suppressed"));
  CHECK(contains(recent->edits()[0].content, "session_budget"));
  CHECK_FALSE(
      contains(recent->edits()[0].content, "The Angel turns toward hope"));
  CHECK(fixture.discord->public_messages().empty());
  fixture.application->stop();
}

TEST_CASE(
    "peer wager commands route through ephemeral modal and fallback flows",
    "[application][interaction][wager][modal][privacy]") {
  sanguinius::test::ApplicationFixture fixture{tarot_options()};
  auto record = application_wager();
  fixture.wagers->mutation_result = {
      .status = sanguinius::WagerMutationStatus::applied,
      .wager = record,
      .controls = {{std::string{sanguinius::wager_form_prefix} +
                        "00000000-0000-4000-8000-000000000781",
                    "Open wager form"}},
      .committed_event_types = {"tarot.wager_drafted.v1"},
      .public_delivery_created = false,
  };
  fixture.application->start();

  auto create = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto request = slash(create, "tarot", "wager", 300);
  request.command_options = {{"target", sanguinius::DiscordId{31}},
                             {"visibility", std::string{"sealed"}}};
  request.resolved_users = {{.user_id = 31,
                             .username = "target",
                             .display_name = "Target",
                             .is_bot = false}};
  fixture.discord->emit(std::move(request));
  REQUIRE(create->wait_for_edit_count(1, 2s));
  REQUIRE(create->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(create->edits()[0].buttons.size() == 1);
  REQUIRE(fixture.wagers->create_request.has_value());
  REQUIRE(fixture.wagers->create_request->target_user_id ==
          sanguinius::DiscordSnowflake{31});

  auto launcher =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto button = sanguinius::test::interaction(
      launcher, sanguinius::InteractionKind::button, 301);
  button.custom_id = create->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(std::move(button));
  REQUIRE(launcher->modals().size() == 1);
  REQUIRE(launcher->deferrals().empty());
  REQUIRE(launcher->modals()[0].fields.size() == 3);

  record.proposition = "The Blood Angels win";
  record.stake = 10;
  record.offer_duration_ms = 86'400'000;
  record.revision = 2;
  fixture.wagers->mutation_result = {
      .status = sanguinius::WagerMutationStatus::applied,
      .wager = record,
      .controls = {{std::string{sanguinius::wager_component_prefix} +
                        "00000000-0000-4000-8000-000000000782",
                    "Confirm offer"},
                   {std::string{sanguinius::wager_component_prefix} +
                        "00000000-0000-4000-8000-000000000783",
                    "Discard"}},
      .committed_event_types = {"tarot.wager_previewed.v1"},
      .public_delivery_created = false,
  };
  auto preview = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto modal = sanguinius::test::interaction(
      preview, sanguinius::InteractionKind::modal_submit, 302);
  modal.custom_id = launcher->modals()[0].custom_id;
  modal.modal_fields = {{"proposition", "The Blood Angels win"},
                        {"stake", "10"},
                        {"evidence_instructions", "Final score"}};
  fixture.discord->emit(std::move(modal));
  REQUIRE(preview->wait_for_edit_count(1, 2s));
  REQUIRE(preview->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(preview->edits()[0].buttons.size() == 2);
  REQUIRE(fixture.wagers->preview_request.has_value());
  REQUIRE(fixture.wagers->preview_request->stake == 10);

  fixture.wagers->mutation_result.status =
      sanguinius::WagerMutationStatus::forbidden;
  record.proposition = "NEVER DISCLOSE THIS SEALED TERM";
  fixture.wagers->mutation_result.wager = record;
  fixture.wagers->mutation_result.controls.clear();
  fixture.wagers->mutation_result.committed_event_types.clear();
  auto wrong_user =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto fallback = slash(wrong_user, "tarot", "wager-action", 303);
  fallback.user_id = 32;
  fallback.command_options = {{"reference", record.wager_id},
                              {"action", std::string{"accept"}}};
  fixture.discord->emit(std::move(fallback));
  REQUIRE(wrong_user->wait_for_edit_count(1, 2s));
  REQUIRE_FALSE(contains(wrong_user->edits()[0].content, "NEVER DISCLOSE"));
  REQUIRE(contains(wrong_user->edits()[0].content, "not available"));

  auto nonowner =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto role = slash(nonowner, "sang-admin", "wager-role", 304);
  role.subcommand_group_name = "tarot";
  role.user_id = 31;
  role.command_options = {{"reference", record.wager_id},
                          {"role", std::string{"target"}}};
  fixture.discord->emit(std::move(role));
  REQUIRE(nonowner->replies().size() == 1);
  REQUIRE(contains(nonowner->replies()[0].first.content, "owner-only"));
  REQUIRE_FALSE(fixture.wagers->test_role_request.has_value());

  fixture.application->stop();
}

TEST_CASE("wager outcome buttons launch and submit an ephemeral modal",
          "[application][interaction][wager][modal][outcome]") {
  sanguinius::test::ApplicationFixture fixture{tarot_options()};
  auto record = application_wager();
  record.state = sanguinius::WagerState::accepted_funded;
  record.revision = 4;
  record.outcome_due_at_ms = 86'401'000;
  record.resolution_grace_until_ms = 259'201'000;
  fixture.wagers->mutation_result = {
      .status = sanguinius::WagerMutationStatus::applied,
      .wager = record,
      .controls = {{std::string{sanguinius::wager_outcome_prefix} +
                        "00000000-0000-4000-8000-000000000784",
                    "Submit outcome"}},
      .committed_event_types = {},
      .public_delivery_created = false,
  };
  fixture.application->start();

  auto launcher =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto button = sanguinius::test::interaction(
      launcher, sanguinius::InteractionKind::button, 305);
  button.custom_id = fixture.wagers->mutation_result.controls.front().custom_id;
  fixture.discord->emit(std::move(button));
  const auto modals = launcher->modals();
  REQUIRE(modals.size() == 1);
  REQUIRE(launcher->deferrals().empty());
  REQUIRE(modals[0].fields.size() == 1);

  auto outcome = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto modal = sanguinius::test::interaction(
      outcome, sanguinius::InteractionKind::modal_submit, 306);
  modal.custom_id = modals[0].custom_id;
  modal.modal_fields = {{"winner", "target"}};
  fixture.discord->emit(std::move(modal));
  REQUIRE(outcome->wait_for_edit_count(1, 2s));
  REQUIRE(outcome->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(fixture.wagers->outcome_request.has_value());
  REQUIRE(fixture.wagers->outcome_request->token_id ==
          "00000000-0000-4000-8000-000000000784");
  REQUIRE(fixture.wagers->outcome_request->winner ==
          sanguinius::WagerRole::target);

  fixture.application->stop();
}

TEST_CASE("privacy discloses retained Tarot data while Tarot is disabled",
          "[application][interaction][tarot][privacy]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.identities->set_tarot_standings_visibility(30, false);
  fixture.application->start();

  auto privacy = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(privacy, "sanguinius", "privacy", 183));
  REQUIRE(privacy->wait_for_edit_count(1, 2s));
  REQUIRE(privacy->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(privacy->edits()[0].content, "Fate feature: disabled"));
  REQUIRE(contains(privacy->edits()[0].content, "Fate standings: private"));
  REQUIRE(contains(privacy->edits()[0].content,
                   "retained as an immutable financial audit"));

  fixture.application->stop();
}

TEST_CASE(
    "Tarot starting grant survives a failed response and duplicate delivery",
    "[application][interaction][tarot][delivery][idempotency]") {
  sanguinius::test::ApplicationFixture fixture{tarot_options()};
  fixture.application->start();

  auto failed = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  failed->set_edit_result(sanguinius::DeliveryResult::transient_failure);
  fixture.discord->emit(slash(failed, "tarot", "balance", 168));
  REQUIRE(failed->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.tarot->ensure_calls == 1);
  REQUIRE(fixture.tarot->starting_grant_count == 1);
  REQUIRE(fixture.diagnostics->contains_category("interaction.tarot_balance"));

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "tarot", "balance", 168));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(retry->edits()[0].content == failed->edits()[0].content);
  REQUIRE(fixture.tarot->ensure_calls == 2);
  REQUIRE(fixture.tarot->starting_grant_count == 1);

  fixture.application->stop();
}

TEST_CASE(
    "Chronicle interactions preserve immediate and deferred response rules",
    "[application][interaction][chronicle]") {
  sanguinius::test::ApplicationFixture fixture{chronicle_options()};
  fixture.application->start();
  REQUIRE(fixture.discord->command_catalog().version == 13);
  REQUIRE(fixture.discord->command_catalog().commands.size() == 4);

  auto remember =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(remember, "chronicle", "remember", 180));
  REQUIRE(remember->modals().size() == 1);
  REQUIRE(remember->deferrals().empty());
  REQUIRE(remember->modals()[0].fields[0].style ==
          sanguinius::ModalFieldPayload::Style::paragraph);

  auto preview = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto submitted = sanguinius::test::interaction(
      preview, sanguinius::InteractionKind::modal_submit, 181);
  submitted.custom_id = "chronicle.remember:1";
  submitted.modal_fields = {{"text", "An explicit personal memory."},
                            {"visibility", "shared"},
                            {"sensitivity", "personal"},
                            {"expiry", "never"}};
  fixture.discord->emit(std::move(submitted));
  REQUIRE(preview->wait_for_edit_count(1, 2s));
  REQUIRE(preview->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(preview->edits()[0].content, "self_only"));
  REQUIRE(preview->edits()[0].buttons.size() == 2);

  auto context = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto canonize = sanguinius::test::interaction(
      context, sanguinius::InteractionKind::message_context_command, 182);
  canonize.command_name = "Canonize in the Chronicle";
  canonize.context_message = sanguinius::ContextMessageSnapshot{
      .reference = {.message_id = 80, .guild_id = 10, .channel_id = 20},
      .author = {.user_id = 31, .username = "source", .display_name = "Source"},
      .content = "A selected source message.",
      .occurred_at_ms = 1'000};
  fixture.discord->emit(std::move(canonize));
  REQUIRE(context->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->proposal_count() == 1);
  REQUIRE(context->edits()[0].buttons.size() == 3);
  REQUIRE(contains(context->edits()[0].content, "TEST DATA"));
  REQUIRE(context->edits()[0].embed.has_value());
  REQUIRE(contains(context->edits()[0].embed->description,
                   "selected source message"));

  auto edit_launcher =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto edit_button = sanguinius::test::interaction(
      edit_launcher, sanguinius::InteractionKind::button, 186);
  edit_button.custom_id = context->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(std::move(edit_button));
  REQUIRE(edit_launcher->modals().size() == 1);
  REQUIRE(edit_launcher->deferrals().empty());

  auto edit_response =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto edit_submit = sanguinius::test::interaction(
      edit_response, sanguinius::InteractionKind::modal_submit, 187);
  edit_submit.custom_id = edit_launcher->modals()[0].custom_id;
  edit_submit.modal_fields = {{"title", "Edited heading"},
                              {"body", "Edited Chronicle body."},
                              {"type", "deed"},
                              {"visibility", "shared"},
                              {"tags", "deed"}};
  fixture.discord->emit(std::move(edit_submit));
  REQUIRE(edit_response->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->edit_count() == 1);

  fixture.relationships->fail_synchronization();
  fixture.chronicle->set_submit_result(
      {.code = sanguinius::ChronicleResultCode::updated,
       .became_canon = true,
       .wake_outbox = true});
  auto submit_response =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto submit_button = sanguinius::test::interaction(
      submit_response, sanguinius::InteractionKind::button, 188);
  submit_button.custom_id = context->edits()[0].buttons[1].custom_id;
  fixture.discord->emit(std::move(submit_button));
  REQUIRE(submit_response->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->submission_count() == 1);
  REQUIRE(contains(submit_response->edits()[0].content, "now canon"));
  REQUIRE(fixture.diagnostics->contains_category("relationship.canon_sync"));
  fixture.relationships->fail_synchronization(false);
  fixture.chronicle->set_submit_result(
      {.code = sanguinius::ChronicleResultCode::invalid_token});

  const std::string approval_token{"00000000-0000-4000-8000-000000000950"};
  static_cast<void>(fixture.notices->create_with_token(
      {.notice_id = "00000000-0000-4000-8000-000000000951",
       .token_id = "00000000-0000-4000-8000-000000000952",
       .target_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .notice_type = "chronicle_approval",
       .content = {.title = "Chronicle approval requested",
                   .body = "Private bounded proposal provenance.",
                   .actions = {{.custom_id =
                                    sanguinius::make_chronicle_component(
                                        sanguinius::chronicle_component_prefix,
                                        approval_token),
                                .label = "Approve"}}},
       .source_aggregate_type = "chronicle_entry",
       .source_aggregate_id = "00000000-0000-4000-8000-000000000953",
       .expires_at_ms = 60'000,
       .notice_idempotency_key = "notice:chronicle:application",
       .token_idempotency_key = "token:chronicle:application",
       .created_at_ms = 1}));
  auto inbox = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(inbox, "sanguinius", "inbox", 199));
  REQUIRE(inbox->wait_for_edit_count(1, 2s));
  REQUIRE(contains(inbox->edits()[0].content,
                   "Private bounded proposal provenance"));
  REQUIRE(inbox->edits()[0].buttons.size() == 1);
  auto approval_response =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto approval_button = sanguinius::test::interaction(
      approval_response, sanguinius::InteractionKind::button, 200);
  approval_button.custom_id = inbox->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(std::move(approval_button));
  REQUIRE(approval_response->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->approval_count() == 1);

  auto cancel_response =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto cancel_button = sanguinius::test::interaction(
      cancel_response, sanguinius::InteractionKind::button, 189);
  cancel_button.custom_id = preview->edits()[0].buttons[1].custom_id;
  fixture.discord->emit(std::move(cancel_button));
  REQUIRE(cancel_response->wait_for_edit_count(1, 2s));
  REQUIRE(contains(cancel_response->edits()[0].content, "discarded"));
  REQUIRE(fixture.chronicle->confirmation_count() == 0);

  auto discarded_confirm =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto discarded_button = sanguinius::test::interaction(
      discarded_confirm, sanguinius::InteractionKind::button, 190);
  discarded_button.custom_id = preview->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(std::move(discarded_button));
  REQUIRE(discarded_confirm->wait_for_edit_count(1, 2s));
  REQUIRE(contains(discarded_confirm->edits()[0].content,
                   "invalid or no longer available"));

  auto second_preview =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto second_modal = sanguinius::test::interaction(
      second_preview, sanguinius::InteractionKind::modal_submit, 191);
  second_modal.custom_id = "chronicle.remember:1";
  second_modal.modal_fields = {{"text", "An ordinary shared memory."},
                               {"visibility", "shared"},
                               {"sensitivity", "ordinary"},
                               {"expiry", "30d"}};
  fixture.discord->emit(std::move(second_modal));
  REQUIRE(second_preview->wait_for_edit_count(1, 2s));

  auto confirm_response =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto confirm_button = sanguinius::test::interaction(
      confirm_response, sanguinius::InteractionKind::button, 192);
  confirm_button.custom_id = second_preview->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(confirm_button);
  REQUIRE(confirm_response->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->confirmation_count() == 1);
  auto duplicate_confirm =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  confirm_button.responder = duplicate_confirm;
  fixture.discord->emit(std::move(confirm_button));
  REQUIRE(duplicate_confirm->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->confirmation_count() == 1);

  sanguinius::ChronicleEntry test_entry;
  test_entry.entry_id = "00000000-0000-4000-8000-000000000901";
  test_entry.title = "Owner test canon";
  test_entry.body = "A visibly marked test entry.";
  test_entry.status = sanguinius::ChronicleEntryStatus::canon;
  test_entry.tags = {"owner-test"};
  fixture.chronicle->recall_results.entries = {test_entry};
  fixture.chronicle->timeline_results = {test_entry};
  fixture.chronicle_sessions->search_result = {
      .cursor_id = "00000000-0000-4000-8000-000000000902",
      .page = 0,
      .total = 1,
      .items = {{.item_id = test_entry.entry_id,
                 .title = "[TEST DATA] " + test_entry.title,
                 .excerpt = test_entry.body,
                 .occurred_at_ms = test_entry.occurred_at_ms}},
      .navigation_token_id = std::nullopt,
      .presentation = "recall",
  };

  auto recall = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(recall, "chronicle", "recall", 193));
  REQUIRE(recall->wait_for_edit_count(1, 2s));
  REQUIRE(recall->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(recall->edits()[0].content, "TEST DATA"));

  auto timeline =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto timeline_request = slash(timeline, "chronicle", "timeline", 194);
  timeline_request.command_options.push_back({"period", std::string{"7d"}});
  fixture.discord->emit(std::move(timeline_request));
  REQUIRE(timeline->wait_for_edit_count(1, 2s));
  REQUIRE(timeline->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::public_message});
  REQUIRE(contains(timeline->edits()[0].content, "TEST DATA"));

  fixture.chronicle->manageable_results = {
      {.kind = sanguinius::ManageableKind::memory,
       .entity_id = "00000000-0000-4000-8000-000000000902",
       .revision = 1,
       .summary = "An ordinary shared memory."}};
  auto forget = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(forget, "chronicle", "forget", 195));
  REQUIRE(forget->wait_for_edit_count(1, 2s));
  REQUIRE(forget->edits()[0].buttons.size() == 1);
  auto retract = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto retract_button = sanguinius::test::interaction(
      retract, sanguinius::InteractionKind::button, 196);
  retract_button.custom_id = forget->edits()[0].buttons[0].custom_id;
  fixture.discord->emit(retract_button);
  REQUIRE(retract->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->retraction_count() == 1);
  auto duplicate_retract =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  retract_button.responder = duplicate_retract;
  fixture.discord->emit(std::move(retract_button));
  REQUIRE(duplicate_retract->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.chronicle->retraction_count() == 1);

  fixture.chronicle->manageable_results = {
      {.kind = sanguinius::ManageableKind::memory,
       .entity_id = "00000000-0000-4000-8000-000000000903",
       .revision = 2,
       .summary = "A command-only memory."}};
  auto direct_forget =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto direct_forget_request = slash(direct_forget, "chronicle", "forget", 210);
  direct_forget_request.command_options.push_back(
      {"reference", std::string{"00000000-0000-4000-8000-000000000903"}});
  fixture.discord->emit(std::move(direct_forget_request));
  REQUIRE(direct_forget->wait_for_edit_count(1, 2s));
  REQUIRE(direct_forget->edits()[0].buttons.empty());
  REQUIRE(fixture.chronicle->retraction_count() == 2);

  fixture.chronicle->manageable_results = {
      {.kind = sanguinius::ManageableKind::memory,
       .entity_id = "00000000-0000-4000-8000-000000000904",
       .revision = 1,
       .summary = "First matching record."},
      {.kind = sanguinius::ManageableKind::entry,
       .entity_id = "00000000-0000-4000-8000-000000000905",
       .revision = 1,
       .summary = "Second matching record."}};
  auto ambiguous_forget =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto ambiguous_request = slash(ambiguous_forget, "chronicle", "forget", 211);
  ambiguous_request.command_options.push_back(
      {"reference", std::string{"0000"}});
  fixture.discord->emit(std::move(ambiguous_request));
  REQUIRE(ambiguous_forget->wait_for_edit_count(1, 2s));
  REQUIRE(contains(ambiguous_forget->edits()[0].content, "ambiguous"));
  REQUIRE(ambiguous_forget->edits()[0].buttons.empty());
  REQUIRE(fixture.chronicle->retraction_count() == 2);

  auto malformed =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto malformed_timeline = slash(malformed, "chronicle", "timeline", 197);
  malformed_timeline.command_options.push_back({"period", std::string{"90d"}});
  fixture.discord->emit(std::move(malformed_timeline));
  REQUIRE(malformed->replies().size() == 1);
  REQUIRE(malformed->deferrals().empty());
  REQUIRE(contains(malformed->replies()[0].first.content, "malformed"));

  auto wrong = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto wrong_context = sanguinius::test::interaction(
      wrong, sanguinius::InteractionKind::message_context_command, 198);
  wrong_context.channel_id = 21;
  wrong_context.command_name = "Canonize in the Chronicle";
  wrong_context.context_message = sanguinius::ContextMessageSnapshot{};
  fixture.discord->emit(std::move(wrong_context));
  REQUIRE(wrong->replies().size() == 1);
  REQUIRE(fixture.chronicle->proposal_count() == 1);
  fixture.application->stop();
}

TEST_CASE("Chronicle profiles and callback preferences preserve visibility",
          "[application][interaction][relationship][privacy]") {
  sanguinius::test::ApplicationFixture fixture{chronicle_options()};
  fixture.relationships->profile_result = {
      .found = true,
      .chronicle_opt_in = true,
      .memory_callbacks = true,
      .user_id = 31,
      .display_name = "Member",
      .dimensions = {.familiarity = 15,
                     .esteem = 5,
                     .mirth = 1,
                     .reliability = 60,
                     .wariness = 30},
      .recent_reasons = {"ai.direct"},
      .shared_canon_count = 2,
      .visible_canon_titles = {"A safe shared heading"},
      .featured_title = std::nullopt,
      .latest_session_summary = std::nullopt,
      .session_open = false,
  };
  fixture.application->start();

  auto self = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(self, "chronicle", "profile", 600));
  REQUIRE(self->wait_for_edit_count(1, 2s));
  REQUIRE(self->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(self->edits()[0].content, "Your Chronicle profile"));
  REQUIRE(contains(self->edits()[0].content, "Memory callbacks: enabled"));
  REQUIRE_FALSE(contains(self->edits()[0].content, "15"));
  REQUIRE_FALSE(contains(self->edits()[0].content, "60"));

  auto other = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto public_profile = slash(other, "chronicle", "profile", 601);
  public_profile.command_options = {{"user", sanguinius::DiscordId{32}}};
  fixture.discord->emit(std::move(public_profile));
  REQUIRE(other->wait_for_edit_count(1, 2s));
  REQUIRE(other->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::public_message});
  REQUIRE(contains(other->edits()[0].content, "Public Chronicle profile"));
  REQUIRE(contains(other->edits()[0].content, "A safe shared heading"));
  REQUIRE_FALSE(contains(other->edits()[0].content, "Memory callbacks"));
  REQUIRE_FALSE(contains(other->edits()[0].content, "Bond:"));

  auto callbacks =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto toggle = slash(callbacks, "chronicle", "callbacks", 602);
  toggle.command_options = {{"mode", std::string{"off"}}};
  fixture.discord->emit(std::move(toggle));
  REQUIRE(callbacks->wait_for_edit_count(1, 2s));
  REQUIRE(callbacks->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(fixture.relationships->preference_change_count() == 1);
  fixture.application->stop();
}

TEST_CASE("slash status privacy and scope enforcement remain ephemeral",
          "[application][interaction][privacy]") {
  sanguinius::test::ApplicationFixture fixture;
  fixture.application->start();
  REQUIRE(fixture.discord->command_catalog().commands.size() == 2);

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

  sanguinius::test::ApplicationFixture chronicle_disabled{admin_options()};
  chronicle_disabled.application->start();
  auto anniversary_disabled =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  chronicle_disabled.discord->emit(
      slash(anniversary_disabled, "sang-admin", "test-anniversary", 214));
  REQUIRE(anniversary_disabled->replies().size() == 1);
  REQUIRE(contains(anniversary_disabled->replies()[0].first.content,
                   "Chronicle is currently unavailable"));
  REQUIRE(chronicle_disabled.chronicle_sessions->anniversary_queue_calls == 0);
  chronicle_disabled.application->stop();

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
  REQUIRE(contains(health->edits()[0].content, "schema=3/3"));
  REQUIRE(contains(health->edits()[0].content,
                   "command_registration=synchronized"));
  REQUIRE(contains(health->edits()[0].content, "interaction_queue="));

  auto created = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(created, "sang-admin", "test-notice", 300));
  REQUIRE(created->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.notices->notice_count() == 1);
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

TEST_CASE(
    "owner appearance controls stay ephemeral and cannot deliver publicly",
    "[application][interaction][appearance]") {
  auto options = admin_options();
  options.features.appearances_mode = sanguinius::AppearanceMode::dry_run;
  sanguinius::test::ApplicationFixture fixture{options};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.ai->set_response(
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"A fine evening for shared victories.","tone":"warm","memory_ids_used":[],"confidence":0.93})");
  fixture.application->start();
  REQUIRE(fixture.appearances->registered());

  auto simulated =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto simulate = slash(simulated, "sang-admin", "simulate", 500);
  simulate.subcommand_group_name = "appearance";
  simulate.command_options = {
      {"fixture", std::string{"lively_game_night_banter"}}};
  fixture.discord->emit(std::move(simulate));
  REQUIRE(simulated->wait_for_edit_count(1, 2s));
  REQUIRE(simulated->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(
      contains(simulated->edits()[0].content, "Appearance simulation stored:"));
  REQUIRE(fixture.appearances->candidate_count() == 1);
  REQUIRE(fixture.ai->wait_for_request_count(1, 2s));

  const auto reference = simulated->edits()[0].content.substr(
      simulated->edits()[0].content.find_last_of(' ') + 1);
  auto previewed =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto preview = slash(previewed, "sang-admin", "preview", 501);
  preview.subcommand_group_name = "appearance";
  preview.command_options = {{"reference", reference}};
  fixture.discord->emit(std::move(preview));
  REQUIRE(previewed->wait_for_edit_count(1, 2s));
  REQUIRE(contains(previewed->edits()[0].content, "Appearance decision"));

  auto recent = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto recent_request = slash(recent, "sang-admin", "recent", 502);
  recent_request.subcommand_group_name = "appearance";
  fixture.discord->emit(std::move(recent_request));
  REQUIRE(recent->wait_for_edit_count(1, 2s));
  REQUIRE(contains(recent->edits()[0].content,
                   "Appearance public-outbox violations: 0"));
  REQUIRE(fixture.discord->public_messages().empty());
  REQUIRE(fixture.discord->replies().empty());
  fixture.application->stop();

  auto off_options = admin_options();
  off_options.controls.test_mode = false;
  sanguinius::test::ApplicationFixture off{off_options};
  off.application->start();
  auto historical =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto historical_request = slash(historical, "sang-admin", "recent", 503);
  historical_request.subcommand_group_name = "appearance";
  off.discord->emit(std::move(historical_request));
  REQUIRE(historical->wait_for_edit_count(1, 2s));
  REQUIRE(contains(historical->edits()[0].content,
                   "Appearance public-outbox violations: 0"));
  REQUIRE(off.discord->public_messages().empty());
  off.application->stop();
}

TEST_CASE(
    "members can manage appearance callback consent while appearances are off",
    "[application][interaction][appearance][consent]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.application->start();
  auto enabled = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto request = slash(enabled, "sanguinius", "appearance-callbacks", 510);
  request.command_options = {{"mode", std::string{"on"}}};
  fixture.discord->emit(std::move(request));
  REQUIRE(enabled->wait_for_edit_count(1, 2s));
  REQUIRE(enabled->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(enabled->edits()[0].content == "Appearance callbacks are enabled.");
  REQUIRE(fixture.appearances->callback_enabled());
  REQUIRE(fixture.discord->public_messages().empty());
  fixture.application->stop();
}

TEST_CASE(
    "live appearance commands remain private and respect owner boundaries",
    "[application][interaction][appearance][live][privacy]") {
  auto options = admin_options();
  options.features.appearances_mode = sanguinius::AppearanceMode::live;
  sanguinius::test::ApplicationFixture fixture{options};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1'000}});
  fixture.application->start();
  REQUIRE(fixture.discord->command_catalog().version == 13);

  auto quiet = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto quiet_request = slash(quiet, "sanguinius", "tonight", 520);
  quiet_request.subcommand_group_name = "quiet";
  quiet_request.user_id = 31;
  fixture.discord->emit(std::move(quiet_request));
  REQUIRE(quiet->wait_for_edit_count(1, 2s));
  REQUIRE(quiet->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(quiet->edits()[0].content, "Server-wide"));

  auto status = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto status_request = slash(status, "sanguinius", "status", 521);
  status_request.user_id = 32;
  fixture.discord->emit(std::move(status_request));
  REQUIRE(status->wait_for_edit_count(1, 2s));
  REQUIRE(contains(status->edits()[0].content, "quiet=active"));
  REQUIRE(contains(status->edits()[0].content, "quiet_until="));
  REQUIRE(contains(status->edits()[0].content,
                   "Your appearance callbacks: disabled"));
  REQUIRE_FALSE(contains(status->edits()[0].content, "31"));
  REQUIRE_FALSE(contains(status->edits()[0].content, "reservations="));
  REQUIRE_FALSE(contains(status->edits()[0].content, "recommendation="));
  REQUIRE_FALSE(contains(status->edits()[0].content, "feedback="));

  auto privacy = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto privacy_request = slash(privacy, "sanguinius", "privacy", 531);
  privacy_request.user_id = 32;
  fixture.discord->emit(std::move(privacy_request));
  REQUIRE(privacy->wait_for_edit_count(1, 2s));
  REQUIRE(contains(privacy->edits()[0].content, "kill_switch=clear"));
  REQUIRE(contains(privacy->edits()[0].content, "quiet=active"));
  REQUIRE_FALSE(contains(privacy->edits()[0].content, "outbox_pending="));
  REQUIRE_FALSE(contains(privacy->edits()[0].content, "model_failures_1h="));

  auto other_clear =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto other_clear_request = slash(other_clear, "sanguinius", "off", 522);
  other_clear_request.subcommand_group_name = "quiet";
  other_clear_request.user_id = 32;
  fixture.discord->emit(std::move(other_clear_request));
  REQUIRE(other_clear->wait_for_edit_count(1, 2s));
  REQUIRE(contains(other_clear->edits()[0].content, "latest quiet setter"));

  auto owner_clear =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto owner_clear_request = slash(owner_clear, "sanguinius", "off", 523);
  owner_clear_request.subcommand_group_name = "quiet";
  fixture.discord->emit(std::move(owner_clear_request));
  REQUIRE(owner_clear->wait_for_edit_count(1, 2s));
  REQUIRE(contains(owner_clear->edits()[0].content, "cleared"));

  auto feedback =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto feedback_request =
      slash(feedback, "sanguinius", "appearance-feedback", 524);
  feedback_request.user_id = 31;
  feedback_request.command_options = {{"response", std::string{"more"}}};
  fixture.discord->emit(std::move(feedback_request));
  REQUIRE(feedback->wait_for_edit_count(1, 2s));
  REQUIRE(feedback->deferrals() ==
          std::vector{sanguinius::ResponseVisibility::ephemeral});
  REQUIRE(contains(feedback->edits()[0].content, "recorded privately"));
  REQUIRE(fixture.appearances->feedback_count() == 1);

  auto disable = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto disable_request = slash(disable, "sang-admin", "disable", 525);
  disable_request.subcommand_group_name = "appearance";
  fixture.discord->emit(std::move(disable_request));
  REQUIRE(disable->wait_for_edit_count(1, 2s));
  REQUIRE(contains(disable->edits()[0].content, "globally disabled"));

  auto suppressed_trigger =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto suppressed_trigger_request =
      slash(suppressed_trigger, "sang-admin", "trigger", 526);
  suppressed_trigger_request.subcommand_group_name = "appearance";
  suppressed_trigger_request.command_options = {
      {"fixture", std::string{"owner_live_safe"}}};
  fixture.discord->emit(std::move(suppressed_trigger_request));
  REQUIRE(suppressed_trigger->wait_for_edit_count(1, 2s));
  REQUIRE(contains(suppressed_trigger->edits()[0].content,
                   "suppressed by final gate: global_kill_switch"));
  REQUIRE_FALSE(contains(suppressed_trigger->edits()[0].content, "queued"));

  auto enable = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto enable_request = slash(enable, "sang-admin", "enable", 527);
  enable_request.subcommand_group_name = "appearance";
  fixture.discord->emit(std::move(enable_request));
  REQUIRE(enable->wait_for_edit_count(1, 2s));
  REQUIRE(contains(enable->edits()[0].content, "kill switch is clear"));

  auto trigger = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto trigger_request = slash(trigger, "sang-admin", "trigger", 528);
  trigger_request.subcommand_group_name = "appearance";
  trigger_request.command_options = {
      {"fixture", std::string{"owner_live_safe"}}};
  fixture.discord->emit(std::move(trigger_request));
  REQUIRE(trigger->wait_for_edit_count(1, 2s));
  REQUIRE(
      contains(trigger->edits()[0].content, "Owner live appearance queued"));

  auto health = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(health, "sang-admin", "health", 529));
  REQUIRE(health->wait_for_edit_count(1, 2s));
  REQUIRE(contains(health->edits()[0].content,
                   "Appearances: configured=live, persisted=live"));
  REQUIRE(fixture.discord->public_messages().empty());
  fixture.application->stop();

  auto no_test_options = admin_options();
  no_test_options.controls.test_mode = false;
  no_test_options.features.appearances_mode = sanguinius::AppearanceMode::live;
  sanguinius::test::ApplicationFixture no_test{no_test_options};
  no_test.application->start();
  auto still_disable =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto still_disable_request =
      slash(still_disable, "sang-admin", "disable", 530);
  still_disable_request.subcommand_group_name = "appearance";
  no_test.discord->emit(std::move(still_disable_request));
  REQUIRE(still_disable->wait_for_edit_count(1, 2s));
  REQUIRE(contains(still_disable->edits()[0].content, "globally disabled"));
  no_test.application->stop();

  auto safety_options = admin_options();
  safety_options.controls.admin_commands_enabled = false;
  safety_options.controls.test_mode = false;
  safety_options.features.appearances_mode = sanguinius::AppearanceMode::live;
  sanguinius::test::ApplicationFixture safety{safety_options};
  safety.application->start();
  REQUIRE(safety.discord->command_catalog().commands.size() == 2);
  REQUIRE(safety.discord->command_catalog().commands[1].name == "sang-admin");
  REQUIRE(safety.discord->command_catalog()
              .commands[1]
              .subcommand_groups[0]
              .subcommands.size() == 2);

  auto safety_disable =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto safety_disable_request =
      slash(safety_disable, "sang-admin", "disable", 532);
  safety_disable_request.subcommand_group_name = "appearance";
  safety.discord->emit(std::move(safety_disable_request));
  REQUIRE(safety_disable->wait_for_edit_count(1, 2s));
  REQUIRE(contains(safety_disable->edits()[0].content, "globally disabled"));

  auto safety_other =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto safety_other_request = slash(safety_other, "sang-admin", "enable", 533);
  safety_other_request.subcommand_group_name = "appearance";
  safety_other_request.user_id = 31;
  safety.discord->emit(std::move(safety_other_request));
  REQUIRE(safety_other->replies().size() == 1);
  REQUIRE(contains(safety_other->replies()[0].first.content, "owner-only"));

  auto unavailable_health =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  safety.discord->emit(slash(unavailable_health, "sang-admin", "health", 534));
  REQUIRE(unavailable_health->replies().size() == 1);
  REQUIRE(contains(unavailable_health->replies()[0].first.content, "disabled"));
  safety.application->stop();
}

TEST_CASE("owner durable test controls schedule retry and inspect safely",
          "[application][interaction][durable][scheduler][outbox]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  fixture.application->start();

  auto scheduled =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(
      slash(scheduled, "sang-admin", "test-schedule-notice", 350));
  REQUIRE(scheduled->wait_for_edit_count(1, 2s));
  REQUIRE(contains(scheduled->edits()[0].content, "scheduled"));
  REQUIRE(fixture.notices->notice_count() == 0);

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{61}});
  REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  const auto trace = fixture.durable_work->recent_events(10);
  REQUIRE(trace.size() == 3);
  REQUIRE(trace[0].event_type == "notice.queued.v1");
  REQUIRE(trace[1].event_type == "owner_test.notice_job_fired.v1");
  REQUIRE(trace[2].event_type == "owner.test_notice_scheduled.v1");
  REQUIRE(trace[0].correlation_id == "350");
  REQUIRE(trace[1].correlation_id == trace[0].correlation_id);
  REQUIRE(trace[2].correlation_id == trace[0].correlation_id);
  REQUIRE(trace[0].causation_id ==
          std::optional<std::string>{trace[1].event_id});
  REQUIRE(trace[1].causation_id ==
          std::optional<std::string>{trace[2].event_id});

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 351));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(contains(retry->edits()[0].content, "first attempt will fail"));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(2, 2s));
  const auto public_messages = fixture.discord->public_messages();
  REQUIRE(public_messages.size() == 2);
  REQUIRE(
      contains(public_messages.back().message.content, "No private content"));

  auto recent = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(recent, "sang-admin", "work-recent", 352));
  REQUIRE(recent->wait_for_edit_count(1, 2s));
  REQUIRE(contains(recent->edits()[0].content, "Recent durable work"));
  REQUIRE_FALSE(
      contains(recent->edits()[0].content, "The sealed-notice test succeeded"));

  auto dead = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(dead, "sang-admin", "work-dead", 353));
  REQUIRE(dead->wait_for_edit_count(1, 2s));
  REQUIRE(contains(dead->edits()[0].content, "Failed and dead durable work"));
  REQUIRE_FALSE(
      contains(dead->edits()[0].content, "The sealed-notice test succeeded"));

  auto replay = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(replay, "sang-admin", "test-public-retry", 351));
  REQUIRE(replay->wait_for_edit_count(1, 2s));
  REQUIRE(contains(replay->edits()[0].content, "already queued"));
  REQUIRE(fixture.discord->public_messages().size() == 2);
  fixture.application->stop();
}

TEST_CASE("unknown Discord delivery reconciles by nonce within its window",
          "[application][durable][outbox][unknown]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->accept_unknown_delivery();
  fixture.discord->set_public_delivery_results(
      {sanguinius::DeliveryResult::unknown_outcome});
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 360));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("discord_unknown_outcome",
                                                      2s));

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{115}});
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);
  REQUIRE(fixture.durable_work->health(115'000).failed_outbox == 0);
  fixture.application->stop();
}

TEST_CASE("a success receipt without a message ID reconciles by nonce",
          "[application][durable][outbox][receipt]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->omit_next_success_message_id();
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 362));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("discord_unknown_outcome",
                                                      2s));

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{115}});
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);
  REQUIRE(fixture.durable_work->health(115'000).failed_outbox == 0);
  fixture.application->stop();
}

TEST_CASE("stale unknown Discord delivery is quarantined without resend",
          "[application][durable][outbox][unknown][quarantine]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->accept_unknown_delivery();
  fixture.discord->set_public_delivery_results(
      {sanguinius::DeliveryResult::unknown_outcome});
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 361));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("discord_unknown_outcome",
                                                      2s));

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{196}});
  REQUIRE(fixture.durable_work->wait_for_outbox_error(
      "discord_unknown_outcome_stale", 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);
  REQUIRE(fixture.durable_work->health(196'000).failed_outbox == 1);
  fixture.application->stop();
}

TEST_CASE("lost Discord callbacks are quarantined after the nonce window",
          "[application][durable][outbox][callback-loss][quarantine]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->hold_public_callbacks();
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 364));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.discord->wait_for_held_public_callback_count(1, 2s));

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{196}});
  REQUIRE(fixture.durable_work->wait_for_outbox_error(
      "discord_unknown_outcome_stale", 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);
  REQUIRE(fixture.durable_work->health(196'000).failed_outbox == 1);

  fixture.discord->release_public_callbacks();
  REQUIRE(fixture.discord->public_messages().size() == 1);
  REQUIRE(fixture.durable_work->health(196'000).failed_outbox == 1);
  fixture.application->stop();
}

TEST_CASE("wall clock rollback cannot reopen the Discord nonce window",
          "[application][durable][outbox][clock][quarantine]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->hold_public_callbacks();
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 365));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));

  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{5}});
  fixture.clock->advance_elapsed(std::chrono::seconds{300});
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{165}});
  REQUIRE(fixture.durable_work->wait_for_outbox_error(
      "discord_unknown_outcome_stale", 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  REQUIRE(fixture.discord->public_messages().size() == 1);

  fixture.application->stop();
  fixture.discord->release_public_callbacks();
}

TEST_CASE("outbox workers bound asynchronous Discord submissions",
          "[application][durable][outbox][bounded]") {
  auto options = admin_options();
  options.durable_delivery_receipt_wait = 2s;
  sanguinius::test::ApplicationFixture fixture{std::move(options)};
  fixture.discord->hold_public_callbacks();

  const std::array rows{
      std::pair{"00000000-0000-4000-8000-000000000811",
                "00000000-0000-4000-8000-000000000911"},
      std::pair{"00000000-0000-4000-8000-000000000812",
                "00000000-0000-4000-8000-000000000912"},
      std::pair{"00000000-0000-4000-8000-000000000813",
                "00000000-0000-4000-8000-000000000913"},
  };
  std::size_t index{};
  for (const auto &[event_id, outbox_id] : rows) {
    const auto suffix = std::to_string(++index);
    REQUIRE(fixture.durable_work->enqueue_public(
        durable_event(event_id, "owner.bounded_queued.v1",
                      "event:bounded:" + suffix),
        durable_outbox(outbox_id,
                       std::string{sanguinius::public_discord_outbox_kind},
                       "outbox:bounded:" + suffix),
        sanguinius::PublicOutboxPayload{
            .request = {.guild_id = 10,
                        .channel_id = 20,
                        .message = sanguinius::text_message("bounded")},
            .fail_before_first_send = false}));
  }

  fixture.application->start();
  REQUIRE(fixture.discord->wait_for_public_message_count(2, 2s));
  REQUIRE_FALSE(fixture.discord->wait_for_public_message_count(3, 250ms));

  fixture.discord->release_public_callbacks();
  REQUIRE(fixture.discord->wait_for_public_message_count(3, 2s));
  fixture.discord->release_public_callbacks();
  REQUIRE(fixture.durable_work->wait_for_outbox_idle(2s));
  fixture.application->stop();
}

TEST_CASE("shutdown fences a late public receipt for lease recovery",
          "[application][durable][outbox][shutdown][restart]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{100}});
  fixture.discord->hold_public_callbacks();
  fixture.application->start();

  auto retry = std::make_shared<sanguinius::test::FakeInteractionResponder>();
  fixture.discord->emit(slash(retry, "sang-admin", "test-public-retry", 363));
  REQUIRE(retry->wait_for_edit_count(1, 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("test_injected_transient",
                                                      2s));
  fixture.clock->set(std::chrono::sys_seconds{std::chrono::seconds{105}});
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
  REQUIRE(fixture.discord->wait_for_held_public_callback_count(1, 2s));

  fixture.application->stop();
  fixture.discord->release_public_callbacks();
  REQUIRE(fixture.durable_work->health(105'000).claimed_outbox == 1);
  const auto recovered = fixture.durable_work->claim_due_outbox(
      165'000, 225'000, "restart-instance",
      "00000000-0000-4000-8000-000000000999", true);
  REQUIRE(recovered.has_value());
  REQUIRE(recovered->attempt_count == 3);
  REQUIRE(fixture.discord->public_messages().size() == 1);
}

TEST_CASE("unknown and malformed durable handlers dead-letter safely",
          "[application][durable][outbox][dead-letter]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  const auto unknown_id = std::string{"00000000-0000-4000-8000-000000000801"};
  REQUIRE(fixture.durable_work->enqueue_public(
      durable_event("00000000-0000-4000-8000-000000000701",
                    "owner.unknown_queued.v1", "event:unknown-handler"),
      durable_outbox(unknown_id, "owner_test.unknown.v1",
                     "outbox:unknown-handler"),
      sanguinius::PublicOutboxPayload{
          .request = {.guild_id = 10,
                      .channel_id = 20,
                      .message = sanguinius::text_message("safe")},
          .fail_before_first_send = false}));

  const auto malformed_id = std::string{"00000000-0000-4000-8000-000000000802"};
  auto malformed_outbox = durable_outbox(
      malformed_id, std::string{sanguinius::public_discord_outbox_kind},
      "outbox:malformed-handler");
  malformed_outbox.target_user_id = 30;
  REQUIRE(fixture.durable_work->enqueue_notice(
      durable_event("00000000-0000-4000-8000-000000000702",
                    "owner.malformed_queued.v1", "event:malformed-handler"),
      malformed_outbox,
      sanguinius::NoticeOutboxPayload{
          .notice = {.notice_id = "00000000-0000-4000-8000-000000000901",
                     .token_id = "00000000-0000-4000-8000-000000000902",
                     .target_user_id = 30,
                     .guild_id = 10,
                     .channel_id = 20,
                     .notice_type = "owner_test.notice.v1",
                     .content = {"private title", "private body"},
                     .source_aggregate_type = "owner_test",
                     .source_aggregate_id = "handler-validation",
                     .expires_at_ms = 86'400'000,
                     .notice_idempotency_key = "notice:malformed-handler",
                     .token_idempotency_key = "token:malformed-handler",
                     .created_at_ms = 0},
          .announce_publicly = true}));

  fixture.application->start();
  REQUIRE(
      fixture.durable_work->wait_for_outbox_error("handler_unknown_type", 2s));
  REQUIRE(fixture.durable_work->wait_for_outbox_error("payload_invalid", 2s));
  REQUIRE(fixture.durable_work->health(0).dead_outbox == 2);
  REQUIRE(fixture.discord->public_messages().empty());
  fixture.application->stop();
}

TEST_CASE("unknown outbox handlers dead-letter while Discord is not ready",
          "[application][durable][outbox][unknown][readiness]") {
  sanguinius::test::ApplicationFixture fixture{admin_options()};
  fixture.discord->set_ready_on_start(false);
  const auto unknown_id = std::string{"00000000-0000-4000-8000-000000000803"};
  REQUIRE(fixture.durable_work->enqueue_public(
      durable_event("00000000-0000-4000-8000-000000000703",
                    "owner.unknown_unready.v1", "event:unknown-unready"),
      durable_outbox(unknown_id, "owner_test.future.v1",
                     "outbox:unknown-unready"),
      sanguinius::PublicOutboxPayload{
          .request = {.guild_id = 10,
                      .channel_id = 20,
                      .message = sanguinius::text_message("safe")},
          .fail_before_first_send = false}));

  fixture.application->start();
  REQUIRE(
      fixture.durable_work->wait_for_outbox_error("handler_unknown_type", 2s));
  REQUIRE(fixture.durable_work->health(0).dead_outbox == 1);
  REQUIRE(fixture.discord->public_messages().empty());
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
    REQUIRE(contains(created->edits()[0].content, "queued"));
    REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));
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
  REQUIRE(fixture.discord->wait_for_public_message_count(1, 2s));
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
  REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));

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
  REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));
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
  REQUIRE(fixture.notices->wait_for_notice_count(1, 2s));
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
