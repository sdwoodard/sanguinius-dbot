#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/persistence/sqlite.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius::persistence {

struct Migration {
  std::int64_t version;
  std::string_view name;
  std::string_view checksum;
  std::string_view sql;
};

enum class SchemaState {
  uninitialized,
  current,
  pending,
  incompatible,
};

struct MigrationStatus {
  SchemaState state{SchemaState::uninitialized};
  std::int64_t current_version{};
  std::int64_t target_version{};
  std::size_t pending_count{};
};

class Migrator {
public:
  Migrator(std::span<const Migration> migrations, BuildInfo build,
           const Clock &clock);

  [[nodiscard]] MigrationStatus inspect(SqliteConnection &connection) const;
  void require_current(SqliteConnection &connection) const;
  [[nodiscard]] MigrationStatus apply(SqliteConnection &connection) const;

private:
  [[nodiscard]] MigrationStatus
  inspect_validated(SqliteConnection &connection) const;
  void validate_manifest() const;

  std::span<const Migration> migrations_;
  BuildInfo build_;
  const Clock &clock_;
};

[[nodiscard]] const std::vector<Migration> &production_migrations();
[[nodiscard]] const char *schema_state_name(SchemaState state) noexcept;

} // namespace sanguinius::persistence
