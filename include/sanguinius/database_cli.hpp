#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/clock.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>

namespace sanguinius {

namespace persistence {
class SqliteConnection;
}

namespace database_cli_detail {

[[nodiscard]] bool
check_core_invariants(persistence::SqliteConnection &connection,
                      std::ostream &output);

} // namespace database_cli_detail

enum class DatabaseCommandType {
  status,
  check,
  migrate,
  integrity,
  backup,
  relationships_check,
  relationships_rebuild,
  tarot_check,
  tarot_rebuild,
  invariants_check,
  invariants_rebuild,
};

struct DatabaseCommand {
  DatabaseCommandType type{DatabaseCommandType::status};
  std::optional<std::filesystem::path> destination;
};

[[nodiscard]] int run_database_command(const DatabaseCommand &command,
                                       const std::filesystem::path &database,
                                       const BuildInfo &build,
                                       const Clock &clock, std::ostream &output,
                                       std::ostream &errors);

} // namespace sanguinius
