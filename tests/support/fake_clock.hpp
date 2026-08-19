#pragma once

#include "sanguinius/clock.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace sanguinius::test {

class FakeClock final : public Clock {
public:
  explicit FakeClock(
      const std::chrono::sys_seconds current = {},
      std::string boot_session_id = "00000000-0000-4000-8000-000000000001")
      : current_{current},
        elapsed_realtime_ms_{std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   current.time_since_epoch())
                   .count())},
        boot_session_id_{std::move(boot_session_id)} {}

  [[nodiscard]] std::chrono::sys_seconds now() const override {
    const std::scoped_lock lock{mutex_};
    return current_;
  }

  [[nodiscard]] std::int64_t elapsed_realtime_ms() const override {
    const std::scoped_lock lock{mutex_};
    return elapsed_realtime_ms_;
  }

  [[nodiscard]] std::string_view boot_session_id() const noexcept override {
    return boot_session_id_;
  }

  void set(const std::chrono::sys_seconds current) {
    const std::scoped_lock lock{mutex_};
    const auto forward = current - current_;
    if (forward > std::chrono::sys_seconds::duration::zero()) {
      elapsed_realtime_ms_ +=
          std::chrono::duration_cast<std::chrono::milliseconds>(forward)
              .count();
    }
    current_ = current;
  }

  void advance_elapsed(const std::chrono::milliseconds amount) {
    const std::scoped_lock lock{mutex_};
    elapsed_realtime_ms_ += amount.count();
  }

private:
  mutable std::mutex mutex_;
  std::chrono::sys_seconds current_;
  std::int64_t elapsed_realtime_ms_{};
  const std::string boot_session_id_;
};

} // namespace sanguinius::test
