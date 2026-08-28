#include "sanguinius/build_info.hpp"

#include "sanguinius/command_registry.hpp"

#ifndef SANGUINIUS_BUILD_VERSION
#define SANGUINIUS_BUILD_VERSION "unknown"
#endif

#ifndef SANGUINIUS_BUILD_REVISION
#define SANGUINIUS_BUILD_REVISION "unknown"
#endif

#ifndef SANGUINIUS_RELEASE_ID
#define SANGUINIUS_RELEASE_ID "development"
#endif

#ifndef SANGUINIUS_BUILD_TIMESTAMP
#define SANGUINIUS_BUILD_TIMESTAMP "unknown"
#endif

#ifndef SANGUINIUS_TOOLCHAIN_ID
#define SANGUINIUS_TOOLCHAIN_ID "local"
#endif

namespace sanguinius {

BuildInfo current_build_info() {
  return BuildInfo{
      .version = SANGUINIUS_BUILD_VERSION,
      .revision = SANGUINIUS_BUILD_REVISION,
      .release_id = SANGUINIUS_RELEASE_ID,
      .build_timestamp = SANGUINIUS_BUILD_TIMESTAMP,
      .toolchain_id = SANGUINIUS_TOOLCHAIN_ID,
      .schema_target = 16,
      .command_catalog_version = command_catalog_version,
  };
}

} // namespace sanguinius
