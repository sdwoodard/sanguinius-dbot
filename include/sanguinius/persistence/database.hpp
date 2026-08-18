#pragma once

#include "sanguinius/persistence/sqlite.hpp"

#include <chrono>
#include <filesystem>

namespace sanguinius::persistence {

inline constexpr int minimum_fixed_sqlite_version = 3'051'003;
inline constexpr int fixed_sqlite_backport_3_50_7 = 3'050'007;
inline constexpr int fixed_sqlite_backport_3_44_6 = 3'044'006;
inline constexpr auto production_busy_timeout = std::chrono::milliseconds{5000};

[[nodiscard]] bool sqlite_has_wal_reset_fix(int version_number) noexcept;
void verify_sqlite_runtime();

enum class DatabaseLockMode {
  shared,
  exclusive,
};

class DatabaseFileLock {
public:
  static DatabaseFileLock acquire(const std::filesystem::path &database_path,
                                  DatabaseLockMode mode);

  ~DatabaseFileLock();
  DatabaseFileLock(const DatabaseFileLock &) = delete;
  DatabaseFileLock &operator=(const DatabaseFileLock &) = delete;
  DatabaseFileLock(DatabaseFileLock &&other) noexcept;
  DatabaseFileLock &operator=(DatabaseFileLock &&other) noexcept;

private:
  explicit DatabaseFileLock(int descriptor) noexcept;
  int descriptor_{-1};
};

class Database {
public:
  static Database open_runtime(
      const std::filesystem::path &path,
      std::chrono::milliseconds busy_timeout = production_busy_timeout);
  static Database open_migration(
      const std::filesystem::path &path,
      std::chrono::milliseconds busy_timeout = production_busy_timeout);
  static Database open_inspection(
      const std::filesystem::path &path,
      std::chrono::milliseconds busy_timeout = production_busy_timeout);

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;
  Database(Database &&) noexcept = default;
  Database &operator=(Database &&) noexcept = default;

  [[nodiscard]] SqliteConnection &connection() noexcept;
  [[nodiscard]] const SqliteConnection &connection() const noexcept;

private:
  Database(DatabaseFileLock lock, SqliteConnection connection) noexcept;

  DatabaseFileLock lock_;
  SqliteConnection connection_;
};

void configure_connection(SqliteConnection &connection,
                          std::chrono::milliseconds busy_timeout,
                          bool enable_wal, bool require_wal);
void enable_wal_mode(SqliteConnection &connection);
void ensure_database_parent(const std::filesystem::path &path);
void restrict_database_file(const std::filesystem::path &path);
[[nodiscard]] std::string sqlite_runtime_version();
[[nodiscard]] std::string sqlite_runtime_source_id();

} // namespace sanguinius::persistence
