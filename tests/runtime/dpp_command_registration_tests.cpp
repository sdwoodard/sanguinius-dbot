#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/dpp_cluster_host.hpp"
#include "sanguinius/dpp_command_registry.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"
#include "sanguinius/dpp_voice_gateway.hpp"
#include "sanguinius/dpp_voice_input_adapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <dpp/dpp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

[[nodiscard]] dpp::slashcommand command() {
  dpp::slashcommand result{"sanguinius", "Consult Sanguinius.", 42};
  result.set_dm_permission(false);
  result.add_option(dpp::command_option{dpp::co_sub_command, "status",
                                        "Show a private status summary."});
  return result;
}

[[nodiscard]] dpp::slashcommand_map existing(dpp::slashcommand command) {
  command.id = 100;
  command.version = 200;
  return {{command.id, std::move(command)}};
}

} // namespace

TEST_CASE("DPP command comparison ignores assigned fields only",
          "[discord][commands][registration]") {
  const std::vector<dpp::slashcommand> desired{command()};
  auto registered = existing(command());
  REQUIRE(sanguinius::dpp_adapter_detail::commands_match(registered, desired));

  registered.begin()->second.set_default_permissions(0);
  REQUIRE_FALSE(
      sanguinius::dpp_adapter_detail::commands_match(registered, desired));

  registered = existing(command());
  registered.begin()->second.add_localization("fr", "sanguinius-fr",
                                              "Description francaise");
  REQUIRE_FALSE(
      sanguinius::dpp_adapter_detail::commands_match(registered, desired));
}

TEST_CASE("DPP translates Chronicle context commands and typed options",
          "[discord][commands][chronicle]") {
  const auto translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(false, true), 42);
  REQUIRE(translated.size() == 6);
  const auto context =
      std::find_if(translated.begin(), translated.end(),
                   [](const dpp::slashcommand &command) {
                     return command.name == "Canonize in the Chronicle";
                   });
  REQUIRE(context != translated.end());
  REQUIRE(context->type == dpp::ctxm_message);

  const auto chronicle = std::find_if(translated.begin(), translated.end(),
                                      [](const dpp::slashcommand &command) {
                                        return command.name == "chronicle";
                                      });
  REQUIRE(chronicle != translated.end());
  REQUIRE(chronicle->options.size() == 9);
  const auto timeline =
      std::find_if(chronicle->options.begin(), chronicle->options.end(),
                   [](const dpp::command_option &option) {
                     return option.name == "timeline";
                   });
  REQUIRE(timeline != chronicle->options.end());
  REQUIRE(timeline->options.size() == 1);
  REQUIRE(timeline->options[0].type == dpp::co_string);
  REQUIRE(timeline->options[0].choices.size() == 3);
  const auto profile =
      std::find_if(chronicle->options.begin(), chronicle->options.end(),
                   [](const dpp::command_option &option) {
                     return option.name == "profile";
                   });
  REQUIRE(profile != chronicle->options.end());
  REQUIRE(profile->options.size() == 1);
  REQUIRE(profile->options[0].type == dpp::co_user);
  const auto session =
      std::find_if(chronicle->options.begin(), chronicle->options.end(),
                   [](const dpp::command_option &option) {
                     return option.name == "session";
                   });
  REQUIRE(session != chronicle->options.end());
  REQUIRE(session->type == dpp::co_sub_command_group);
  REQUIRE(session->options.size() == 6);
  REQUIRE(session->options[0].type == dpp::co_sub_command);
  const auto title = std::find_if(
      chronicle->options.begin(), chronicle->options.end(),
      [](const dpp::command_option &option) { return option.name == "title"; });
  REQUIRE(title != chronicle->options.end());
  const auto title_list = std::find_if(
      title->options.begin(), title->options.end(),
      [](const dpp::command_option &option) { return option.name == "list"; });
  REQUIRE(title_list != title->options.end());
  REQUIRE(title_list->options.size() == 1);
  REQUIRE(title_list->options[0].name == "recipient");
  REQUIRE(title_list->options[0].type == dpp::co_user);
}

TEST_CASE("DPP serializes modal text inputs as direct Label children",
          "[discord][modal][serialization]") {
  const auto payload = sanguinius::ChronicleService::edit_entry_modal(
      "00000000-0000-4000-8000-000000000001");
  const auto response = nlohmann::json::parse(
      sanguinius::dpp_adapter_detail::modal_response_json(payload));

  REQUIRE(response.at("type") == dpp::ir_modal_dialog);
  const auto &components = response.at("data").at("components");
  REQUIRE(components.size() == payload.fields.size());
  for (std::size_t index{}; index < payload.fields.size(); ++index) {
    const auto &label = components.at(index);
    REQUIRE(label.at("type") == dpp::cot_label);
    REQUIRE(label.at("label") == payload.fields.at(index).label);
    REQUIRE(label.at("component").at("type") == dpp::cot_text);
    REQUIRE(label.at("component").at("custom_id") ==
            payload.fields.at(index).custom_id);
    REQUIRE_FALSE(label.at("component").contains("components"));
  }
}

TEST_CASE("DPP translates owner appearance controls as one nested group",
          "[discord][commands][appearance]") {
  const auto translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(true, false), 42);
  const auto admin = std::find_if(translated.begin(), translated.end(),
                                  [](const dpp::slashcommand &command) {
                                    return command.name == "sang-admin";
                                  });
  REQUIRE(admin != translated.end());
  const auto appearance =
      std::find_if(admin->options.begin(), admin->options.end(),
                   [](const dpp::command_option &option) {
                     return option.name == "appearance";
                   });
  REQUIRE(appearance != admin->options.end());
  REQUIRE(appearance->type == dpp::co_sub_command_group);
  REQUIRE(appearance->options.size() == 4);
  REQUIRE(appearance->options[0].name == "simulate");
  REQUIRE(appearance->options[0].options.size() == 1);
  REQUIRE(appearance->options[0].options[0].choices.size() == 13);
}

TEST_CASE("DPP voice catalog and intent are independently feature gated",
          "[discord][commands][vox][intent]") {
  const auto translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(true, false, false, true), 42);
  const auto vox = std::ranges::find(translated, std::string{"vox"},
                                     &dpp::slashcommand::name);
  REQUIRE(vox != translated.end());
  REQUIRE(vox->options.size() == 8);
  REQUIRE(vox->options[6].name == "listen-start");
  REQUIRE(vox->options[7].name == "listen-stop");
  const auto admin = std::ranges::find(translated, std::string{"sang-admin"},
                                       &dpp::slashcommand::name);
  REQUIRE(admin != translated.end());
  const auto vox_admin = std::ranges::find(admin->options, std::string{"vox"},
                                           &dpp::command_option::name);
  REQUIRE(vox_admin != admin->options.end());
  REQUIRE(vox_admin->type == dpp::co_sub_command_group);
  REQUIRE(vox_admin->options.size() == 5);

  const auto safety_translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(false, false, false, true), 42);
  const auto safety_admin = std::ranges::find(
      safety_translated, std::string{"sang-admin"}, &dpp::slashcommand::name);
  REQUIRE(safety_admin != safety_translated.end());
  const auto safety_vox = std::ranges::find(
      safety_admin->options, std::string{"safety"}, &dpp::command_option::name);
  REQUIRE(safety_vox != safety_admin->options.end());
  REQUIRE(safety_vox->options.size() == 2);
  REQUIRE(safety_vox->options[0].name == "status");
  REQUIRE(safety_vox->options[1].name == "set");

  sanguinius::DppClusterHost text_only{"test-token", false};
  REQUIRE((text_only.intents() & dpp::i_guild_voice_states) == 0U);
  REQUIRE(text_only.native().ws_mode == dpp::ws_json);
  sanguinius::DppClusterHost voice{"test-token", true};
  REQUIRE((voice.intents() & dpp::i_guild_voice_states) != 0U);
  REQUIRE((voice.intents() & dpp::i_direct_messages) == 0U);
  REQUIRE((voice.intents() & dpp::i_guild_members) == 0U);
}

TEST_CASE("DPP voice-ready translation and binding replacement fail closed",
          "[discord][vox][translation]") {
  const sanguinius::VoiceGatewaySnapshot binding{.bound = true,
                                                 .connected = true,
                                                 .ready = false,
                                                 .dave_active = false,
                                                 .marker_completed = false,
                                                 .completed_marker = {},
                                                 .bot_moved = false,
                                                 .session_id = "session-one",
                                                 .guild_id = 10,
                                                 .channel_id = 40,
                                                 .observed_channel_id = 40,
                                                 .generation = 2,
                                                 .human_count = 1};
  const auto ready = sanguinius::dpp_voice_gateway_detail::translate_ready(
      binding, 40, true, true);
  REQUIRE(ready.kind == sanguinius::VoiceEventKind::ready);
  REQUIRE(ready.dave_active);
  REQUIRE(ready.generation == 2);

  const auto not_ready = sanguinius::dpp_voice_gateway_detail::translate_ready(
      binding, 40, false, false);
  REQUIRE(not_ready.kind == sanguinius::VoiceEventKind::error);
  REQUIRE(not_ready.failure_category == "voice_not_ready");
  const auto moved = sanguinius::dpp_voice_gateway_detail::translate_ready(
      binding, 41, true, true);
  REQUIRE(moved.kind == sanguinius::VoiceEventKind::bot_moved);
  REQUIRE(moved.channel_id == 41);
  REQUIRE_FALSE(moved.dave_active);

  REQUIRE(sanguinius::dpp_voice_gateway_detail::may_replace_binding(
      binding, "session-one"));
  REQUIRE_FALSE(sanguinius::dpp_voice_gateway_detail::may_replace_binding(
      binding, "session-two"));
  auto disconnected = binding;
  disconnected.connected = false;
  REQUIRE(sanguinius::dpp_voice_gateway_detail::may_replace_binding(
      disconnected, "session-two"));

  REQUIRE(sanguinius::dpp_voice_gateway_detail::matches_voice_session(
      "discord-session-two", "discord-session-two"));
  REQUIRE_FALSE(sanguinius::dpp_voice_gateway_detail::matches_voice_session(
      "discord-session-two", "discord-session-one"));
  REQUIRE_FALSE(sanguinius::dpp_voice_gateway_detail::matches_voice_session(
      {}, "discord-session-one"));
  const int current_voice_client{};
  const int stale_voice_client{};
  REQUIRE(sanguinius::dpp_voice_gateway_detail::matches_voice_client(
      &current_voice_client, &current_voice_client));
  REQUIRE_FALSE(sanguinius::dpp_voice_gateway_detail::matches_voice_client(
      &current_voice_client, &stale_voice_client));
  REQUIRE_FALSE(sanguinius::dpp_voice_gateway_detail::matches_voice_client(
      nullptr, &current_voice_client));

  using sanguinius::dpp_voice_input_adapter_detail::classify_receive_client;
  using sanguinius::dpp_voice_input_adapter_detail::ReceiveClientDisposition;
  REQUIRE(classify_receive_client(&current_voice_client, &current_voice_client,
                                  10, 10) == ReceiveClientDisposition::accept);
  REQUIRE(classify_receive_client(&current_voice_client, &stale_voice_client,
                                  10, 10) ==
          ReceiveClientDisposition::connection_changed);
  REQUIRE(classify_receive_client(&current_voice_client, nullptr, 10, {}) ==
          ReceiveClientDisposition::connection_changed);
  REQUIRE(classify_receive_client(&current_voice_client, &stale_voice_client,
                                  10, 11) == ReceiveClientDisposition::ignore);
  REQUIRE(classify_receive_client(nullptr, &current_voice_client, 10, 10) ==
          ReceiveClientDisposition::ignore);

  const auto chunks =
      sanguinius::dpp_voice_gateway_detail::pcm_chunk_sizes(23'044);
  REQUIRE(chunks == std::vector<std::size_t>{11'520, 11'520, 4});
  REQUIRE(std::ranges::all_of(chunks,
                              [](const auto bytes) { return bytes % 4 == 0; }));
  REQUIRE(
      sanguinius::dpp_voice_gateway_detail::pcm_chunk_sizes(23'043).empty());

  const auto undeaf = nlohmann::json::parse(
      sanguinius::dpp_voice_gateway_detail::voice_state_update_payload(
          sanguinius::DiscordSnowflake{10}, sanguinius::DiscordSnowflake{40},
          false));
  REQUIRE(undeaf == nlohmann::json{{"op", 4},
                                   {"d",
                                    {{"guild_id", "10"},
                                     {"channel_id", "40"},
                                     {"self_mute", false},
                                     {"self_deaf", false}}}});
  auto self_deaf = nlohmann::json::parse(
      sanguinius::dpp_voice_gateway_detail::voice_state_update_payload(
          sanguinius::DiscordSnowflake{10}, sanguinius::DiscordSnowflake{40},
          true));
  REQUIRE(self_deaf["d"]["self_deaf"] == true);
}

TEST_CASE("DPP translates bounded Tarot integer adjustments",
          "[discord][commands][tarot]") {
  const auto translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(true, false, true), 42);
  const auto admin = std::ranges::find(translated, std::string{"sang-admin"},
                                       &dpp::slashcommand::name);
  REQUIRE(admin != translated.end());
  const auto tarot = std::ranges::find(admin->options, std::string{"tarot"},
                                       &dpp::command_option::name);
  REQUIRE(tarot != admin->options.end());
  REQUIRE(tarot->type == dpp::co_sub_command_group);
  const auto adjust = std::ranges::find(tarot->options, std::string{"adjust"},
                                        &dpp::command_option::name);
  REQUIRE(adjust != tarot->options.end());
  REQUIRE(adjust->options[0].type == dpp::co_integer);
  REQUIRE(std::get<std::int64_t>(adjust->options[0].min_value) ==
          sanguinius::minimum_tarot_adjustment);
  REQUIRE(std::get<std::int64_t>(adjust->options[0].max_value) ==
          sanguinius::maximum_tarot_adjustment);
}

TEST_CASE("DPP translates nested House catalog and incoming play options",
          "[discord][commands][tarot][house][incoming]") {
  const auto commands =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(true, false, true), 42);
  const auto tarot = std::ranges::find(commands, std::string{"tarot"},
                                       &dpp::slashcommand::name);
  REQUIRE(tarot != commands.end());
  const auto house = std::ranges::find(tarot->options, std::string{"house"},
                                       &dpp::command_option::name);
  REQUIRE(house != tarot->options.end());
  REQUIRE(house->type == dpp::co_sub_command_group);
  REQUIRE(house->options.size() == 3);
  const auto play = std::ranges::find(house->options, std::string{"play"},
                                      &dpp::command_option::name);
  REQUIRE(play != house->options.end());
  REQUIRE(play->type == dpp::co_sub_command);
  REQUIRE(play->options.size() == 5);
  REQUIRE(play->options[0].name == "template");
  REQUIRE(play->options[0].choices.size() == 4);
  REQUIRE(play->options[1].name == "choice");
  REQUIRE(play->options[2].name == "stake");
  REQUIRE(play->options[2].type == dpp::co_integer);

  dpp::command_interaction incoming;
  incoming.name = "tarot";
  dpp::command_data_option template_option;
  template_option.name = "template";
  template_option.type = dpp::co_string;
  template_option.value = std::string{"heralds-call"};
  dpp::command_data_option stake_option;
  stake_option.name = "stake";
  stake_option.type = dpp::co_integer;
  stake_option.value = std::int64_t{5};
  dpp::command_data_option play_option;
  play_option.name = "play";
  play_option.type = dpp::co_sub_command;
  play_option.options = {template_option, stake_option};
  dpp::command_data_option house_option;
  house_option.name = "house";
  house_option.type = dpp::co_sub_command_group;
  house_option.options = {play_option};
  incoming.options = {house_option};
  sanguinius::IncomingInteraction translated;
  sanguinius::dpp_adapter_detail::translate_slash_command(incoming, translated);
  REQUIRE(translated.command_name == "tarot");
  REQUIRE(translated.subcommand_group_name == "house");
  REQUIRE(translated.subcommand_name == "play");
  REQUIRE(translated.command_options.size() == 2);
  REQUIRE(std::get<std::string>(translated.command_options[0].value) ==
          "heralds-call");
  REQUIRE(std::get<std::int64_t>(translated.command_options[1].value) == 5);
}

TEST_CASE(
    "DPP retains owner appearance safety controls when administration is off",
    "[discord][commands][appearance][safety]") {
  const auto translated =
      sanguinius::dpp_adapter_detail::translate_command_catalog(
          sanguinius::command_catalog(false, false), 42);
  const auto admin = std::ranges::find(translated, std::string{"sang-admin"},
                                       &dpp::slashcommand::name);
  REQUIRE(admin != translated.end());
  REQUIRE(admin->options.size() == 1);
  REQUIRE(admin->options[0].name == "safety");
  REQUIRE(admin->options[0].options.size() == 2);
  REQUIRE(admin->options[0].options[0].name == "status");
  REQUIRE(admin->options[0].options[1].name == "set");
}

TEST_CASE(
    "DPP translates nested incoming commands and rejects malformed groups",
    "[discord][commands][chronicle][incoming]") {
  dpp::command_interaction command;
  command.name = "chronicle";
  dpp::command_data_option reference;
  reference.name = "reference";
  reference.type = dpp::co_string;
  reference.value = std::string{"00000000-0000-4000-8000-000000000101"};
  dpp::command_data_option close;
  close.name = "close";
  close.type = dpp::co_sub_command;
  close.options.push_back(reference);
  dpp::command_data_option session;
  session.name = "session";
  session.type = dpp::co_sub_command_group;
  session.options.push_back(close);
  command.options.push_back(session);

  sanguinius::IncomingInteraction translated;
  sanguinius::dpp_adapter_detail::translate_slash_command(command, translated);
  REQUIRE(translated.command_name == "chronicle");
  REQUIRE(translated.subcommand_group_name == "session");
  REQUIRE(translated.subcommand_name == "close");
  REQUIRE(translated.command_options.size() == 1);
  REQUIRE(translated.command_options.front().name == "reference");
  REQUIRE(std::get<std::string>(translated.command_options.front().value) ==
          "00000000-0000-4000-8000-000000000101");

  command.options.front().options.push_back(close);
  REQUIRE_THROWS_AS(sanguinius::dpp_adapter_detail::translate_slash_command(
                        command, translated),
                    std::invalid_argument);
}

TEST_CASE("DPP context snapshots retain only bounded Chronicle metadata",
          "[discord][chronicle][privacy]") {
  dpp::message message;
  message.id = 100;
  message.guild_id = 10;
  message.channel_id = 20;
  message.author.id = 30;
  message.author.username = "author";
  message.member.set_nickname("Display");
  message.content = std::string(2'001, 'a') + "secret tail";
  message.sent = 123;
  message.attachments.emplace_back(&message);
  auto &attachment = message.attachments.back();
  attachment.id = 200;
  attachment.filename = "SPOILER_proof.png";
  attachment.content_type = "image/png";
  attachment.size = 99;
  attachment.width = 10;
  attachment.height = 20;
  attachment.ephemeral = true;
  attachment.url = "https://secret.invalid/original";
  attachment.proxy_url = "https://secret.invalid/proxy";
  attachment.description = "not retained";
  attachment.waveform = "not retained";

  const auto snapshot =
      sanguinius::dpp_adapter_detail::context_message_snapshot(message);
  REQUIRE(snapshot.reference.message_id == 100);
  REQUIRE(snapshot.author.user_id == 30);
  REQUIRE(snapshot.author.display_name == "Display");
  REQUIRE(snapshot.content.size() == sanguinius::maximum_chronicle_source_size);
  REQUIRE(snapshot.content_truncated);
  REQUIRE(snapshot.occurred_at_ms == 123'000);
  REQUIRE(snapshot.attachments.size() == 1);
  REQUIRE(snapshot.attachments[0].filename == "SPOILER_proof.png");
  REQUIRE(snapshot.attachments[0].spoiler);
  REQUIRE(snapshot.attachments[0].ephemeral);

  for (std::size_t index = 0; index <= sanguinius::maximum_chronicle_mentions;
       ++index) {
    dpp::user mentioned;
    mentioned.id = 300 + index;
    mentioned.username = "participant";
    message.mentions.emplace_back(std::move(mentioned), dpp::guild_member{});
  }
  REQUIRE_THROWS_AS(
      sanguinius::dpp_adapter_detail::context_message_snapshot(message),
      std::invalid_argument);
}

TEST_CASE("callback fence waits for active work and suppresses late callbacks",
          "[discord][callbacks][shutdown]") {
  using namespace std::chrono_literals;

  auto fence = std::make_shared<sanguinius::CallbackFence>();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> active_invoked{false};
  std::jthread active{[fence, &entered, released, &active_invoked] {
    active_invoked.store(fence->invoke([&entered, released] {
      entered.set_value();
      released.wait();
    }));
  }};
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);

  std::promise<void> closing_started;
  auto closing_started_future = closing_started.get_future();
  auto closing = std::async(std::launch::async, [fence, &closing_started] {
    closing_started.set_value();
    fence->close_and_wait();
  });
  REQUIRE(closing_started_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(closing.wait_for(20ms) == std::future_status::timeout);
  release.set_value();
  REQUIRE(closing.wait_for(1s) == std::future_status::ready);
  active.join();
  REQUIRE(active_invoked.load());

  std::atomic<bool> late_invoked{false};
  REQUIRE_FALSE(fence->invoke([&late_invoked] { late_invoked.store(true); }));
  REQUIRE_FALSE(late_invoked.load());
}

TEST_CASE("durable Discord payload enforces nonce and suppresses mentions",
          "[discord][outbox][contract]") {
  REQUIRE(sanguinius::dpp_adapter_detail::durable_public_message_base_path() ==
          "/api/v10/channels");
  const sanguinius::PublicMessageRequest request{
      .guild_id = 10,
      .channel_id = 20,
      .message = {.content = "Neutral notice",
                  .embed = std::nullopt,
                  .buttons = {},
                  .allowed_user_mentions = {30}},
  };
  constexpr std::string_view nonce{"0123456789abcdef012345678"};
  const auto payload = nlohmann::json::parse(
      sanguinius::dpp_adapter_detail::durable_public_message_json(request,
                                                                  nonce));
  REQUIRE(payload.at("nonce") == nonce);
  REQUIRE(payload.at("enforce_nonce") == true);
  REQUIRE(payload.at("allowed_mentions").at("parse").empty());
  REQUIRE(payload.at("allowed_mentions").at("users").size() == 1);
  REQUIRE(payload.at("allowed_mentions").at("users")[0] == "30");
  REQUIRE(payload.size() == 4);
  REQUIRE(payload.contains("content"));
  REQUIRE_FALSE(payload.contains("channel_id"));
  REQUIRE_FALSE(payload.contains("guild_id"));
  REQUIRE_FALSE(payload.contains("type"));
  REQUIRE_FALSE(payload.contains("attachments"));
  REQUIRE_FALSE(payload.contains("flags"));
  REQUIRE_FALSE(payload.contains("tts"));
  REQUIRE_THROWS(sanguinius::dpp_adapter_detail::durable_public_message_json(
      request, "invalid"));
  auto incomplete = request;
  incomplete.channel_id = {};
  REQUIRE_THROWS(sanguinius::dpp_adapter_detail::durable_public_message_json(
      incomplete, nonce));
  auto empty = request;
  empty.message.content.clear();
  REQUIRE_THROWS(sanguinius::dpp_adapter_detail::durable_public_message_json(
      empty, nonce));
  dpp::message receipt;
  receipt.id = 777;
  REQUIRE(sanguinius::dpp_adapter_detail::provider_message_id(receipt) == 777);
  receipt.id = 0;
  REQUIRE_THROWS(sanguinius::dpp_adapter_detail::provider_message_id(receipt));
}

TEST_CASE("Discord HTTP delivery outcomes are classified conservatively",
          "[discord][outbox][contract]") {
  using sanguinius::DeliveryResult;
  using sanguinius::dpp_adapter_detail::classify_http_delivery;
  REQUIRE(classify_http_delivery(true, 200, false) == DeliveryResult::success);
  REQUIRE(classify_http_delivery(false, 0, true) ==
          DeliveryResult::unknown_outcome);
  REQUIRE(classify_http_delivery(false, 429, false) ==
          DeliveryResult::transient_failure);
  REQUIRE(classify_http_delivery(false, 503, false) ==
          DeliveryResult::transient_failure);
  REQUIRE(classify_http_delivery(false, 403, false) ==
          DeliveryResult::permanent_failure);
}
