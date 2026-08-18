#include "sanguinius/process_signals.hpp"

#include <pthread.h>

#include <cerrno>
#include <system_error>

namespace sanguinius {

ProcessSignals::ProcessSignals() {
  if (sigemptyset(&signals_) != 0 || sigaddset(&signals_, SIGINT) != 0 ||
      sigaddset(&signals_, SIGTERM) != 0) {
    throw std::system_error{errno, std::generic_category(),
                            "Unable to initialize process signal set"};
  }

  const int result = pthread_sigmask(SIG_BLOCK, &signals_, &previous_);
  if (result != 0) {
    throw std::system_error{result, std::generic_category(),
                            "Unable to block process shutdown signals"};
  }
  installed_ = true;
}

ProcessSignals::~ProcessSignals() {
  if (installed_) {
    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
  }
}

int ProcessSignals::wait() const {
  int signal = 0;
  const int result = sigwait(&signals_, &signal);
  if (result != 0) {
    throw std::system_error{result, std::generic_category(),
                            "Unable to wait for a process shutdown signal"};
  }
  return signal;
}

} // namespace sanguinius
