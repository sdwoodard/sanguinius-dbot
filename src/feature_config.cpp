#include "sanguinius/feature_config.hpp"

namespace sanguinius {

std::string_view appearance_mode_name(const AppearanceMode mode) noexcept {
  switch (mode) {
  case AppearanceMode::off:
    return "off";
  case AppearanceMode::dry_run:
    return "dry_run";
  }
  return "unknown";
}

} // namespace sanguinius
