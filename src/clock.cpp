#include "sanguinius/clock.hpp"

namespace sanguinius {

std::chrono::sys_seconds SystemClock::now() const {
  return std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
}

} // namespace sanguinius
