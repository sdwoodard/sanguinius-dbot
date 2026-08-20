#include "sanguinius/build_info.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/command_registry.hpp"
#include "sanguinius/composition_root.hpp"
#include "sanguinius/config.hpp"
#include "sanguinius/database_cli.hpp"
#include "sanguinius/discord_command_cli.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"
#include "sanguinius/process_signals.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream &stream, const std::string_view executable) {
  stream << "Usage: " << executable << " [--check-config|--help]\n"
         << "       " << executable << " db <status|check|migrate|integrity>\n"
         << "       " << executable << " db backup <destination>\n"
         << "       " << executable << " db relationships check\n"
         << "       " << executable << " db tarot check\n"
         << "       " << executable
         << " db relationships rebuild --confirm\n"
         << "       " << executable << " discord commands sync\n"
         << "       " << executable << " discord commands clear --confirm\n";
}

[[nodiscard]] std::optional<sanguinius::DatabaseCommand>
database_command(const int argc, char **argv) {
  if (argc < 3 || std::string_view{argv[1]} != "db") {
    return std::nullopt;
  }
  const std::string_view operation{argv[2]};
  if (argc == 3 && operation == "status") {
    return sanguinius::DatabaseCommand{sanguinius::DatabaseCommandType::status,
                                       std::nullopt};
  }
  if (argc == 3 && operation == "check") {
    return sanguinius::DatabaseCommand{sanguinius::DatabaseCommandType::check,
                                       std::nullopt};
  }
  if (argc == 3 && operation == "migrate") {
    return sanguinius::DatabaseCommand{sanguinius::DatabaseCommandType::migrate,
                                       std::nullopt};
  }
  if (argc == 3 && operation == "integrity") {
    return sanguinius::DatabaseCommand{
        sanguinius::DatabaseCommandType::integrity, std::nullopt};
  }
  if (argc == 4 && operation == "backup") {
    const std::filesystem::path destination{argv[3]};
    if (destination.empty()) {
      return std::nullopt;
    }
    return sanguinius::DatabaseCommand{sanguinius::DatabaseCommandType::backup,
                                       destination};
  }
  if (argc == 4 && operation == "relationships" &&
      std::string_view{argv[3]} == "check") {
    return sanguinius::DatabaseCommand{
        sanguinius::DatabaseCommandType::relationships_check, std::nullopt};
  }
  if (argc == 4 && operation == "tarot" &&
      std::string_view{argv[3]} == "check") {
    return sanguinius::DatabaseCommand{
        sanguinius::DatabaseCommandType::tarot_check, std::nullopt};
  }
  if (argc == 5 && operation == "relationships" &&
      std::string_view{argv[3]} == "rebuild" &&
      std::string_view{argv[4]} == "--confirm") {
    return sanguinius::DatabaseCommand{
        sanguinius::DatabaseCommandType::relationships_rebuild, std::nullopt};
  }
  return std::nullopt;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    if (argc >= 2 && std::string_view{argv[1]} == "db") {
      const auto command = database_command(argc, argv);
      if (!command.has_value()) {
        print_usage(std::cerr, argv[0]);
        return 2;
      }
      const sanguinius::SystemClock clock;
      return sanguinius::run_database_command(
          *command, sanguinius::database_file_from_environment(),
          sanguinius::current_build_info(), clock, std::cout, std::cerr);
    }
    if (argc >= 2 && std::string_view{argv[1]} == "discord") {
      std::vector<std::string_view> arguments;
      arguments.reserve(static_cast<std::size_t>(argc - 1));
      for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
      }
      const auto operation = sanguinius::parse_discord_command(arguments);
      if (!operation.has_value()) {
        print_usage(std::cerr, argv[0]);
        return 2;
      }
      auto command_config =
          sanguinius::discord_command_configuration_from_environment();
      return sanguinius::run_discord_command_operator(
          *operation, std::move(command_config.token),
          command_config.request_timeout,
          sanguinius::DiscordId{command_config.guild_id.value()},
          sanguinius::command_catalog(command_config.admin_commands_enabled,
                                      command_config.chronicle_enabled,
                                      command_config.tarot_enabled),
          std::cout, std::cerr);
    }
    if (argc > 2) {
      print_usage(std::cerr, argv[0]);
      return 2;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
      print_usage(std::cout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view{argv[1]} != "--check-config") {
      print_usage(std::cerr, argv[0]);
      return 2;
    }

    const auto config = sanguinius::Config::from_environment();
    if (argc == 2) {
      std::cout << sanguinius::redacted_config_summary(
          config, sanguinius::current_build_info());
      return EXIT_SUCCESS;
    }

    sanguinius::ProcessSignals signals;
    auto application = sanguinius::make_application(config);
    application->start();
    static_cast<void>(signals.wait());
    application->stop();
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Fatal error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
