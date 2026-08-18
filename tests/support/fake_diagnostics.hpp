#pragma once

#include "sanguinius/diagnostics.hpp"

#include <mutex>
#include <string_view>
#include <vector>

namespace sanguinius::test {

class FakeDiagnostics final : public Diagnostics {
public:
  void emit(const DiagnosticEvent &event) noexcept override {
    try {
      const std::scoped_lock lock{mutex_};
      events_.push_back(event);
    } catch (...) {
    }
  }

  [[nodiscard]] bool contains_category(const std::string_view category) const {
    const std::scoped_lock lock{mutex_};
    for (const auto &event : events_) {
      if (event.category == category) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::vector<DiagnosticEvent> events() const {
    const std::scoped_lock lock{mutex_};
    return events_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<DiagnosticEvent> events_;
};

} // namespace sanguinius::test
