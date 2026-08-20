#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/dpp_command_registry.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"

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
  REQUIRE(translated.size() == 4);
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
  REQUIRE(title_list->options.size() == 2);
  REQUIRE(title_list->options[1].name == "page");
  REQUIRE(title_list->options[1].type == dpp::co_string);
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
  REQUIRE(appearance->options.size() == 6);
  REQUIRE(appearance->options[0].name == "simulate");
  REQUIRE(appearance->options[0].options.size() == 1);
  REQUIRE(appearance->options[0].options[0].choices.size() == 13);
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
  REQUIRE(admin->options[0].name == "appearance");
  REQUIRE(admin->options[0].options.size() == 2);
  REQUIRE(admin->options[0].options[0].name == "disable");
  REQUIRE(admin->options[0].options[1].name == "enable");
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
  REQUIRE_THROWS(sanguinius::dpp_adapter_detail::durable_public_message_json(
      request, "invalid"));
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
