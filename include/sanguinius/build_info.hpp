#pragma once

#include <string>

namespace sanguinius {

struct BuildInfo {
  std::string version;
  std::string revision;
};

[[nodiscard]] BuildInfo current_build_info();

} // namespace sanguinius
