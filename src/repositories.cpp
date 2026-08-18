#include "sanguinius/repositories.hpp"

namespace sanguinius {

const char *
application_stop_reason_name(const ApplicationStopReason reason) noexcept {
  switch (reason) {
  case ApplicationStopReason::clean_shutdown:
    return "clean_shutdown";
  case ApplicationStopReason::startup_failure:
    return "startup_failure";
  }
  return "startup_failure";
}

} // namespace sanguinius
