#include "sanguinius/service_notifier.hpp"

#include <algorithm>
#include <string>
#include <systemd/sd-daemon.h>

namespace sanguinius {
namespace {

constexpr std::size_t maximum_status_size = 256;

[[nodiscard]] std::string sanitized_status(const std::string_view message) {
  std::string result;
  result.reserve(std::min(message.size(), maximum_status_size));
  for (const char character : message.substr(0, maximum_status_size)) {
    result.push_back(character == '\n' || character == '\r' || character == '\0'
                         ? ' '
                         : character);
  }
  return result;
}

} // namespace

void SystemdServiceNotifier::status(const std::string_view message) noexcept {
  try {
    const auto value = "STATUS=" + sanitized_status(message);
    static_cast<void>(::sd_notify(0, value.c_str()));
  } catch (...) {
  }
}

void SystemdServiceNotifier::ready(const std::string_view message) noexcept {
  bool expected = false;
  if (!ready_sent_.compare_exchange_strong(expected, true))
    return;
  try {
    const auto value = "READY=1\nSTATUS=" + sanitized_status(message);
    static_cast<void>(::sd_notify(0, value.c_str()));
  } catch (...) {
  }
}

void SystemdServiceNotifier::stopping() noexcept {
  static_cast<void>(::sd_notify(0, "STOPPING=1\nSTATUS=Stopping cleanly"));
}

} // namespace sanguinius
