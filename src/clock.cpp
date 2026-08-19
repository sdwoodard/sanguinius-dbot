#include "sanguinius/clock.hpp"

#include "sanguinius/persistent_id.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <time.h>

namespace sanguinius {
namespace {

[[nodiscard]] bool valid_boot_session_id(const std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'z') || character == '-';
         });
}

[[nodiscard]] std::string load_boot_session_id() {
  std::ifstream input{"/proc/sys/kernel/random/boot_id"};
  std::string value;
  if (input >> value; input && valid_boot_session_id(value)) {
    return value;
  }
  // A process-local fallback deliberately changes across restarts. That loses
  // an automatic retry when the platform cannot identify its boot, but it
  // cannot misclassify an old ambiguous Discord submission as recent.
  static const std::string process_boot_session_id =
      UuidV4Generator{}.next_id();
  return process_boot_session_id;
}

} // namespace

SystemClock::SystemClock() : boot_session_id_{load_boot_session_id()} {}

std::chrono::sys_seconds SystemClock::now() const {
  return std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
}

std::int64_t SystemClock::elapsed_realtime_ms() const {
  timespec value{};
  if (::clock_gettime(CLOCK_BOOTTIME, &value) != 0) {
    throw std::system_error{errno, std::generic_category(),
                            "Unable to read elapsed realtime"};
  }
  constexpr std::int64_t milliseconds_per_second = 1'000;
  constexpr std::int64_t nanoseconds_per_millisecond = 1'000'000;
  return static_cast<std::int64_t>(value.tv_sec) * milliseconds_per_second +
         static_cast<std::int64_t>(value.tv_nsec) / nanoseconds_per_millisecond;
}

std::string_view SystemClock::boot_session_id() const noexcept {
  return boot_session_id_;
}

} // namespace sanguinius
