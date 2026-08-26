#pragma once

#include <string_view>

namespace sanguinius {

enum class AppearanceMode {
  off,
  dry_run,
  live,
};

struct ControlConfiguration {
  bool admin_commands_enabled{false};
  bool test_mode{false};
};

struct FeatureConfiguration {
  bool chronicle_enabled{false};
  bool tarot_enabled{false};
  AppearanceMode appearances_mode{AppearanceMode::off};
  bool vox_enabled{false};
  bool vox_narration_enabled{false};
  bool voice_input_enabled{false};
};

[[nodiscard]] std::string_view
appearance_mode_name(AppearanceMode mode) noexcept;

} // namespace sanguinius
