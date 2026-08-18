#include "sanguinius/callback_fence.hpp"
#include "sanguinius/dpp_command_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <dpp/dpp.h>

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
