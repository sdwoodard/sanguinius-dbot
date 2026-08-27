#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/repositories.hpp"
#include "sanguinius/safety_controls.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace sanguinius {

class SanguiniusOverviewService {
public:
  SanguiniusOverviewService(FeatureConfiguration features, const Clock &clock);

  [[nodiscard]] InteractionMessage help(std::string_view topic) const;
  [[nodiscard]] InteractionMessage
  status(std::size_t pending_notices, std::string_view appearance_status,
         const UserPreferences &preferences,
         const MemberRuntimeStatus &runtime) const;
  [[nodiscard]] InteractionMessage
  privacy(const UserPreferences &preferences, std::size_t pending_notices,
          std::string_view appearance_status, std::string_view tarot_status,
          const MemberRuntimeStatus &runtime) const;
  [[nodiscard]] std::string
  owner_health(std::string operational_health,
               std::string_view appearance_status,
               const MemberRuntimeStatus &runtime,
               std::string_view appearance_diagnostics = {}) const;

private:
  FeatureConfiguration features_;
  const Clock &clock_;
};

} // namespace sanguinius
