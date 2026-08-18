#include "sanguinius/discord_command_cli.hpp"

namespace sanguinius {

std::optional<DiscordCommandOperation>
parse_discord_command(const std::span<const std::string_view> arguments) {
  if (arguments.size() == 3 && arguments[0] == "discord" &&
      arguments[1] == "commands" && arguments[2] == "sync") {
    return DiscordCommandOperation::synchronize;
  }
  if (arguments.size() == 4 && arguments[0] == "discord" &&
      arguments[1] == "commands" && arguments[2] == "clear" &&
      arguments[3] == "--confirm") {
    return DiscordCommandOperation::clear;
  }
  return std::nullopt;
}

} // namespace sanguinius
