#pragma once

#include <cstdint>
#include <string>

namespace sanguinius {

struct BuildInfo {
  std::string version;
  std::string revision;
  std::string release_id{"development"};
  std::string build_timestamp{"unknown"};
  std::string toolchain_id{"local"};
  std::int64_t schema_target{16};
  std::uint32_t command_catalog_version{16};
};

[[nodiscard]] BuildInfo current_build_info();

} // namespace sanguinius
