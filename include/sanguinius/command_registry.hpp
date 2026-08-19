#pragma once

#include "sanguinius/discord_types.hpp"

#include <mutex>
#include <string>

namespace sanguinius {

inline constexpr std::uint32_t command_catalog_version = 4;

[[nodiscard]] CommandCatalog command_catalog(bool admin_commands_enabled,
                                             bool chronicle_enabled = false);
[[nodiscard]] std::string
canonical_command_snapshot(const CommandCatalog &catalog);

enum class CommandCatalogFetchAction {
  none,
  update_required,
};

class CommandRegistrationCoordinator {
public:
  [[nodiscard]] bool begin();
  [[nodiscard]] CommandCatalogFetchAction catalog_fetched(bool success,
                                                          bool matches);
  void catalog_updated(bool success);
  void cancel() noexcept;
  [[nodiscard]] CommandRegistrationState state() const noexcept;

private:
  mutable std::mutex mutex_;
  bool in_flight_{};
  CommandRegistrationState state_{CommandRegistrationState::not_started};
};

} // namespace sanguinius
