#pragma once

#include "sanguinius/clock.hpp"

#include <chrono>

namespace sanguinius::test {

class FakeClock final : public Clock {
public:
  explicit FakeClock(const std::chrono::sys_seconds current = {})
      : current_{current} {}

  [[nodiscard]] std::chrono::sys_seconds now() const override {
    return current_;
  }

  void set(const std::chrono::sys_seconds current) { current_ = current; }

private:
  std::chrono::sys_seconds current_;
};

} // namespace sanguinius::test
