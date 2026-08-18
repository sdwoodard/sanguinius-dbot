#pragma once

#include <csignal>

namespace sanguinius {

class ProcessSignals {
public:
  ProcessSignals();
  ~ProcessSignals();

  ProcessSignals(const ProcessSignals &) = delete;
  ProcessSignals &operator=(const ProcessSignals &) = delete;
  ProcessSignals(ProcessSignals &&) = delete;
  ProcessSignals &operator=(ProcessSignals &&) = delete;

  [[nodiscard]] int wait() const;

private:
  sigset_t signals_{};
  sigset_t previous_{};
  bool installed_{false};
};

} // namespace sanguinius
