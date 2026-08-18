#include "sanguinius/dpp_command_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace sanguinius::dpp_adapter_detail {
namespace {

[[nodiscard]] std::string command_projection(const dpp::slashcommand &command) {
  auto projection = command.to_json(false);
  for (const std::string_view assigned_field : {
           "id",
           "application_id",
           "guild_id",
           "version",
       }) {
    static_cast<void>(projection.erase(std::string{assigned_field}));
  }
  return projection.dump();
}

template <typename Commands>
[[nodiscard]] std::string canonical_snapshot(const Commands &commands) {
  std::vector<std::string> projections;
  projections.reserve(commands.size());
  for (const auto &entry : commands) {
    if constexpr (std::is_same_v<typename Commands::value_type,
                                 dpp::slashcommand>) {
      projections.push_back(command_projection(entry));
    } else {
      projections.push_back(command_projection(entry.second));
    }
  }
  std::ranges::sort(projections);
  std::string snapshot;
  for (const auto &projection : projections) {
    snapshot += projection;
    snapshot.push_back('\n');
  }
  return snapshot;
}

} // namespace

std::string
canonical_command_snapshot(const dpp::slashcommand_map &commands) {
  return canonical_snapshot(commands);
}

std::string canonical_command_snapshot(
    const std::vector<dpp::slashcommand> &commands) {
  return canonical_snapshot(commands);
}

bool commands_match(const dpp::slashcommand_map &existing,
                    const std::vector<dpp::slashcommand> &desired) {
  return canonical_command_snapshot(existing) ==
         canonical_command_snapshot(desired);
}

} // namespace sanguinius::dpp_adapter_detail
