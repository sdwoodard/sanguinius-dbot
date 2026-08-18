#pragma once

#include <dpp/appcommand.h>

#include <string>
#include <vector>

namespace sanguinius::dpp_adapter_detail {

[[nodiscard]] std::string
canonical_command_snapshot(const dpp::slashcommand_map &commands);
[[nodiscard]] std::string
canonical_command_snapshot(const std::vector<dpp::slashcommand> &commands);
[[nodiscard]] bool
commands_match(const dpp::slashcommand_map &existing,
               const std::vector<dpp::slashcommand> &desired);

} // namespace sanguinius::dpp_adapter_detail
