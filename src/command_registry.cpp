#include "sanguinius/command_registry.hpp"
#include "sanguinius/chronicle.hpp"

#include <sstream>

namespace sanguinius {

CommandCatalog command_catalog(const bool admin_commands_enabled,
                               const bool chronicle_enabled) {
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
  if (chronicle_enabled) {
    catalog.commands.push_back(CommandDefinition{
        .name = "chronicle",
        .description = "Consult or amend the Living Chronicle.",
        .subcommands =
            {CommandSubcommandDefinition{
                 .name = "remember",
                 .description =
                     "Explicitly remember something after a private preview."},
             CommandSubcommandDefinition{
                 .name = "recall",
                 .description = "Privately recall up to five visible records.",
                 .options = {CommandOptionDefinition{
                     .kind = CommandOptionKind::string,
                     .name = "query",
                     .description = "Optional text to find.",
                     .required = false,
                     .minimum_length = 1,
                     .maximum_length = maximum_memory_text_size}}},
             CommandSubcommandDefinition{
                 .name = "timeline",
                 .description = "Show recent shared canon entries.",
                 .options = {CommandOptionDefinition{
                     .kind = CommandOptionKind::string,
                     .name = "period",
                     .description = "Time period to display.",
                     .required = false,
                     .minimum_length = 2,
                     .maximum_length = 3,
                     .choices = {{"Last 7 days", "7d"},
                                 {"Last 30 days", "30d"},
                                 {"All time", "all"}}}}},
             CommandSubcommandDefinition{
                 .name = "forget",
                 .description =
                     "Retract by reference or privately choose a record.",
                 .options = {CommandOptionDefinition{
                     .kind = CommandOptionKind::string,
                     .name = "reference",
                     .description = "Optional record reference prefix.",
                     .required = false,
                     .minimum_length = 4,
                     .maximum_length = 36}}},
             CommandSubcommandDefinition{
                 .name = "profile",
                 .description = "Show a private self or public-safe profile.",
                 .options = {CommandOptionDefinition{
                     .kind = CommandOptionKind::user,
                     .name = "user",
                     .description = "Optional member for a public-safe profile.",
                     .required = false}}},
             CommandSubcommandDefinition{
                 .name = "callbacks",
                 .description = "Privately enable or disable memory callbacks.",
                 .options = {CommandOptionDefinition{
                     .kind = CommandOptionKind::string,
                     .name = "mode",
                     .description = "Memory callback preference.",
                     .required = true,
                     .minimum_length = 2,
                     .maximum_length = 3,
                     .choices = {{"On", "on"}, {"Off", "off"}}}}},
            },
    });
    catalog.commands.push_back(CommandDefinition{
        .name = "Canonize in the Chronicle",
        .description = {},
        .subcommands = {},
        .kind = ApplicationCommandKind::message_context,
    });
  }
  if (admin_commands_enabled) {
    catalog.commands.push_back(CommandDefinition{
        .name = "sang-admin",
        .description = "Owner-only Sanguinius controls.",
        .subcommands =
            {
                {"health", "Show the private redacted health snapshot."},
                {"work-recent", "Inspect recent redacted durable work."},
                {"work-dead", "Inspect failed and dead durable work."},
                {"test-notice", "Create a private self-targeted test notice."},
                {"test-schedule-notice",
                 "Schedule a private self-targeted test notice."},
                {"test-public-retry",
                 "Exercise one synthetic public delivery retry."},
            },
    });
  }
  return catalog;
}

std::string canonical_command_snapshot(const CommandCatalog &catalog) {
  std::ostringstream output;
  output << "catalog_version=" << catalog.version << '\n';
  for (const auto &command : catalog.commands) {
    output << "command=" << static_cast<int>(command.kind) << '|'
           << command.name << '|' << command.description << '\n';
    for (const auto &subcommand : command.subcommands) {
      output << "subcommand=" << subcommand.name << '|'
             << subcommand.description << '\n';
      for (const auto &option : subcommand.options) {
        output << "option=" << static_cast<int>(option.kind) << '|'
               << option.name << '|' << option.description << '|'
               << option.required << '|' << option.minimum_length << '|'
               << option.maximum_length << '\n';
        for (const auto &choice : option.choices) {
          output << "choice=" << choice.name << '|' << choice.value << '\n';
        }
      }
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
