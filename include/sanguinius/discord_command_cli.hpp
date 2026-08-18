#pragma once

#include <optional>
#include <span>
#include <string_view>

namespace sanguinius {

enum class DiscordCommandOperation {
  synchronize,
  clear,
};

[[nodiscard]] std::optional<DiscordCommandOperation>
parse_discord_command(std::span<const std::string_view> arguments);

} // namespace sanguinius
