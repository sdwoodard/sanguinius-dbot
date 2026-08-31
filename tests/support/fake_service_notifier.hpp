#pragma once

#include "sanguinius/service_notifier.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace sanguinius::test {

class FakeServiceNotifier final : public ServiceNotifier {
public:
  void status(const std::string_view message) noexcept override {
    const std::scoped_lock lock{mutex_};
    statuses_.emplace_back(message);
  }
  void ready(const std::string_view message) noexcept override {
    const std::scoped_lock lock{mutex_};
    ready_.emplace_back(message);
  }
  void watchdog(const std::string_view message) noexcept override {
    const std::scoped_lock lock{mutex_};
    watchdog_.emplace_back(message);
  }
  void stopping() noexcept override {
    const std::scoped_lock lock{mutex_};
    ++stopping_;
  }
  [[nodiscard]] std::size_t ready_count() const {
    const std::scoped_lock lock{mutex_};
    return ready_.size();
  }
  [[nodiscard]] std::size_t stopping_count() const {
    const std::scoped_lock lock{mutex_};
    return stopping_;
  }
  [[nodiscard]] std::size_t watchdog_count() const {
    const std::scoped_lock lock{mutex_};
    return watchdog_.size();
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> statuses_;
  std::vector<std::string> ready_;
  std::vector<std::string> watchdog_;
  std::size_t stopping_{};
};

} // namespace sanguinius::test
