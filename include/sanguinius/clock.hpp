#pragma once

#include <chrono>

namespace sanguinius {

class Clock {
public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual std::chrono::sys_seconds now() const = 0;
};

class SystemClock final : public Clock {
public:
  [[nodiscard]] std::chrono::sys_seconds now() const override;
};

} // namespace sanguinius
