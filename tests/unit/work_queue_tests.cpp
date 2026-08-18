#include "sanguinius/work_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

class StopAwareGate {
public:
  void wait(const std::stop_token stop_token) {
    std::unique_lock lock{mutex_};
    entered_ = true;
    changed_.notify_all();
    std::stop_callback notify_on_stop{stop_token,
                                      [this] { changed_.notify_all(); }};
    changed_.wait(lock, [this, stop_token] {
      return released_ || stop_token.stop_requested();
    });
    cancelled_ = stop_token.stop_requested();
  }

  [[nodiscard]] bool wait_until_entered() {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, 2s, [this] { return entered_; });
  }

  void release() {
    {
      const std::scoped_lock lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool cancelled() const {
    const std::scoped_lock lock{mutex_};
    return cancelled_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{false};
  bool released_{false};
  bool cancelled_{false};
};

} // namespace

TEST_CASE("bounded executor validates its configuration", "[queue]") {
  REQUIRE_THROWS_AS((sanguinius::BoundedExecutor{0, 1}), std::invalid_argument);
  REQUIRE_THROWS_AS((sanguinius::BoundedExecutor{1, 0}), std::invalid_argument);
}

TEST_CASE("bounded executor reports deterministic saturation", "[queue]") {
  StopAwareGate gate;
  sanguinius::BoundedExecutor executor{1, 1};
  executor.start();

  REQUIRE(executor.try_submit([&gate](const std::stop_token stop_token) {
    gate.wait(stop_token);
  }) == sanguinius::SubmitResult::accepted);
  REQUIRE(gate.wait_until_entered());
  REQUIRE(executor.try_submit([](std::stop_token) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(executor.try_submit([](std::stop_token) {}) ==
          sanguinius::SubmitResult::full);

  gate.release();
  executor.stop();
  REQUIRE(executor.try_submit([](std::stop_token) {}) ==
          sanguinius::SubmitResult::stopping);
}

TEST_CASE("bounded executor cancels active work and discards pending work",
          "[queue][shutdown]") {
  StopAwareGate gate;
  std::atomic<int> pending_runs{0};
  sanguinius::BoundedExecutor executor{1, 1};
  executor.start();

  REQUIRE(executor.try_submit([&gate](const std::stop_token stop_token) {
    gate.wait(stop_token);
  }) == sanguinius::SubmitResult::accepted);
  REQUIRE(gate.wait_until_entered());
  REQUIRE(executor.try_submit([&pending_runs](std::stop_token) {
    ++pending_runs;
  }) == sanguinius::SubmitResult::accepted);

  executor.stop();
  REQUIRE(gate.cancelled());
  REQUIRE(pending_runs.load() == 0);
}

TEST_CASE("bounded executor contains task exceptions", "[queue]") {
  auto completion = std::make_shared<std::promise<void>>();
  auto completed = completion->get_future();
  sanguinius::BoundedExecutor executor{2, 1};
  executor.start();

  REQUIRE(executor.try_submit([](std::stop_token) {
    throw std::runtime_error{"scripted task failure"};
  }) == sanguinius::SubmitResult::accepted);
  REQUIRE(executor.try_submit([completion](std::stop_token) {
    completion->set_value();
  }) == sanguinius::SubmitResult::accepted);
  REQUIRE(completed.wait_for(2s) == std::future_status::ready);

  executor.stop();
  REQUIRE_THROWS_AS(executor.start(), std::logic_error);
}
