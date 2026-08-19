#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace sanguinius {

class Clock {
public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual std::chrono::sys_seconds now() const = 0;
  [[nodiscard]] virtual std::int64_t elapsed_realtime_ms() const = 0;
  [[nodiscard]] virtual std::string_view boot_session_id() const noexcept = 0;
};

class SystemClock final : public Clock {
public:
  SystemClock();

  [[nodiscard]] std::chrono::sys_seconds now() const override;
  [[nodiscard]] std::int64_t elapsed_realtime_ms() const override;
  [[nodiscard]] std::string_view boot_session_id() const noexcept override;

private:
  std::string boot_session_id_;
};

} // namespace sanguinius
