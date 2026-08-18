#include "sanguinius/command_registry.hpp"

#include <sstream>

namespace sanguinius {

CommandCatalog command_catalog(const bool admin_commands_enabled) {
  CommandCatalog catalog{
      .version = command_catalog_version,
      .commands =
          {
              CommandDefinition{
                  .name = "sanguinius",
                  .description = "Consult Sanguinius.",
                  .subcommands =
                      {
                          {"status", "Show a private status summary."},
                          {"inbox", "Open the next sealed notice privately."},
                          {"privacy",
                           "Review private-data and voice settings."},
                      },
              },
          },
  };
  if (admin_commands_enabled) {
    catalog.commands.push_back(CommandDefinition{
        .name = "sang-admin",
        .description = "Owner-only Sanguinius controls.",
        .subcommands =
            {
                {"health", "Show the private redacted health snapshot."},
                {"test-notice", "Create a private self-targeted test notice."},
            },
    });
  }
  return catalog;
}

std::string canonical_command_snapshot(const CommandCatalog &catalog) {
  std::ostringstream output;
  output << "catalog_version=" << catalog.version << '\n';
  for (const auto &command : catalog.commands) {
    output << "command=" << command.name << '|' << command.description << '\n';
    for (const auto &subcommand : command.subcommands) {
      output << "subcommand=" << subcommand.name << '|'
             << subcommand.description << '\n';
    }
  }
  return output.str();
}

bool CommandRegistrationCoordinator::begin() {
  const std::scoped_lock lock{mutex_};
  if (in_flight_) {
    return false;
  }
  in_flight_ = true;
  state_ = CommandRegistrationState::synchronizing;
  return true;
}

CommandCatalogFetchAction
CommandRegistrationCoordinator::catalog_fetched(const bool success,
                                                const bool matches) {
  const std::scoped_lock lock{mutex_};
  if (!in_flight_) {
    return CommandCatalogFetchAction::none;
  }
  if (!success) {
    in_flight_ = false;
    state_ = CommandRegistrationState::failed;
    return CommandCatalogFetchAction::none;
  }
  if (matches) {
    in_flight_ = false;
    state_ = CommandRegistrationState::synchronized;
    return CommandCatalogFetchAction::none;
  }
  return CommandCatalogFetchAction::update_required;
}

void CommandRegistrationCoordinator::catalog_updated(const bool success) {
  const std::scoped_lock lock{mutex_};
  if (!in_flight_) {
    return;
  }
  in_flight_ = false;
  state_ = success ? CommandRegistrationState::synchronized
                   : CommandRegistrationState::failed;
}

void CommandRegistrationCoordinator::cancel() noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    in_flight_ = false;
  } catch (...) {
  }
}

CommandRegistrationState
CommandRegistrationCoordinator::state() const noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    return state_;
  } catch (...) {
    return CommandRegistrationState::failed;
  }
}

} // namespace sanguinius
