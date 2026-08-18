#include "sanguinius/diagnostics.hpp"

#include <iostream>

namespace sanguinius {
namespace {

[[nodiscard]] std::string_view label(const DiagnosticSeverity severity) {
  switch (severity) {
  case DiagnosticSeverity::debug:
    return "debug";
  case DiagnosticSeverity::info:
    return "info";
  case DiagnosticSeverity::warning:
    return "warning";
  case DiagnosticSeverity::error:
    return "error";
  }
  return "unknown";
}

} // namespace

void ConsoleDiagnostics::emit(const DiagnosticEvent &event) noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    auto &stream =
        event.severity == DiagnosticSeverity::error ? std::cerr : std::cout;
    stream << '[' << label(event.severity) << "] " << event.category;
    if (event.correlation_id.has_value()) {
      stream << " correlation_id=" << *event.correlation_id;
    }
    stream << ": " << event.message << '\n';
  } catch (...) {
  }
}

} // namespace sanguinius
