#include "sanguinius/callback_fence.hpp"
#include "sanguinius/dpp_command_registry.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <dpp/dpp.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <future>
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
