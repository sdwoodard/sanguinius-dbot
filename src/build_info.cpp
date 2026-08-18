#include "sanguinius/build_info.hpp"

#ifndef SANGUINIUS_BUILD_VERSION
#define SANGUINIUS_BUILD_VERSION "unknown"
#endif

#ifndef SANGUINIUS_BUILD_REVISION
#define SANGUINIUS_BUILD_REVISION "unknown"
#endif

namespace sanguinius {

BuildInfo current_build_info() {
  return BuildInfo{SANGUINIUS_BUILD_VERSION, SANGUINIUS_BUILD_REVISION};
}

} // namespace sanguinius
