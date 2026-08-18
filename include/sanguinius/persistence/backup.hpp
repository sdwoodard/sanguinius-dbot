#pragma once

#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"

#include <cstdint>
#include <filesystem>

namespace sanguinius::persistence {

struct IntegrityResult {
  bool integrity_ok{};
  bool foreign_keys_ok{};

  [[nodiscard]] bool ok() const noexcept {
    return integrity_ok && foreign_keys_ok;
  }
};

struct BackupResult {
  MigrationStatus migration;
  std::uintmax_t size_bytes{};
};

class DatabaseMaintenance {
public:
  [[nodiscard]] static IntegrityResult
  integrity_check(SqliteConnection &connection);

  [[nodiscard]] static BackupResult
  backup(SqliteConnection &source, const std::filesystem::path &source_path,
         const std::filesystem::path &destination_path,
         const Migrator &migrator,
         std::chrono::milliseconds busy_timeout = production_busy_timeout);
};

} // namespace sanguinius::persistence
