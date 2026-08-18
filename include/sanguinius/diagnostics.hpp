#pragma once

#include <mutex>
#include <optional>
#include <string>

namespace sanguinius {

enum class DiagnosticSeverity {
  debug,
  info,
  warning,
  error,
};

struct DiagnosticEvent {
  DiagnosticSeverity severity;
  std::string category;
  std::string message;
  std::optional<std::string> correlation_id;
};

class Diagnostics {
public:
  virtual ~Diagnostics() = default;

  virtual void emit(const DiagnosticEvent &event) noexcept = 0;
};

class ConsoleDiagnostics final : public Diagnostics {
public:
  void emit(const DiagnosticEvent &event) noexcept override;

private:
  std::mutex mutex_;
};

} // namespace sanguinius
