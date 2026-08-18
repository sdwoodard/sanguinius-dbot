#include "sanguinius/persistence/backup.hpp"

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <system_error>

namespace sanguinius::persistence {
namespace {

class PartialBackupCleanup {
public:
  explicit PartialBackupCleanup(std::filesystem::path path)
      : path_{std::move(path)} {}

  ~PartialBackupCleanup() {
    if (!committed_) {
      std::error_code ignored;
      std::filesystem::remove(path_, ignored);
      auto wal = path_;
      wal += "-wal";
      std::filesystem::remove(wal, ignored);
      auto shared_memory = path_;
      shared_memory += "-shm";
      std::filesystem::remove(shared_memory, ignored);
      auto journal = path_;
      journal += "-journal";
      std::filesystem::remove(journal, ignored);
    }
  }

  void commit() noexcept { committed_ = true; }

private:
  std::filesystem::path path_;
  bool committed_{false};
};

[[nodiscard]] std::filesystem::path
normalized_path(const std::filesystem::path &path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (error) {
    result = std::filesystem::absolute(path, error).lexically_normal();
  }
  if (error) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN, "Backup path validation failed (io)."};
  }
  return result;
}

[[nodiscard]] int reserve_destination(const std::filesystem::path &path) {
  const auto native = path.string();
  const int descriptor = ::open(
      native.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Backup destination creation failed (io)."};
  }
  if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
    static_cast<void>(::close(descriptor));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                        "Backup destination permissions failed (io)."};
  }
  return descriptor;
}

void require_backup_sidecars_absent(const std::filesystem::path &path) {
  constexpr std::array<std::string_view, 3> suffixes{"-wal", "-shm",
                                                     "-journal"};
  for (const auto suffix : suffixes) {
    auto sidecar = path;
    sidecar += suffix;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(sidecar, error);
    if (error == std::errc::no_such_file_or_directory) {
      continue;
    }
    if (error) {
      throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                          SQLITE_CANTOPEN,
                          "Backup destination validation failed (io)."};
    }
    if (status.type() != std::filesystem::file_type::not_found) {
      throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                          SQLITE_CANTOPEN,
                          "Backup destination namespace is already in use."};
    }
  }
}

void copy_database(sqlite3 *source, sqlite3 *destination,
                   const std::chrono::milliseconds busy_timeout) {
  sqlite3_backup *backup =
      sqlite3_backup_init(destination, "main", source, "main");
  if (backup == nullptr) {
    throw DatabaseError{database_error_category(sqlite3_errcode(destination)),
                        sqlite3_errcode(destination),
                        sqlite3_extended_errcode(destination),
                        "SQLite backup initialization failed."};
  }

  int result = SQLITE_OK;
  std::chrono::milliseconds waited{0};
  constexpr auto retry_interval = std::chrono::milliseconds{25};
  do {
    result = sqlite3_backup_step(backup, 64);
    if (result == SQLITE_BUSY || result == SQLITE_LOCKED) {
      if (waited >= busy_timeout) {
        break;
      }
      static_cast<void>(
          sqlite3_sleep(static_cast<int>(retry_interval.count())));
      waited += retry_interval;
    }
  } while (result == SQLITE_OK || result == SQLITE_BUSY ||
           result == SQLITE_LOCKED);

  const int finish_result = sqlite3_backup_finish(backup);
  if (result != SQLITE_DONE || finish_result != SQLITE_OK) {
    const int code = result == SQLITE_DONE ? finish_result : result;
    throw DatabaseError{database_error_category(code), code,
                        sqlite3_extended_errcode(destination),
                        "SQLite backup copy failed (" +
                            std::string{database_error_category_name(
                                database_error_category(code))} +
                            ")."};
  }
}

} // namespace

IntegrityResult
DatabaseMaintenance::integrity_check(SqliteConnection &connection) {
  bool integrity_ok = false;
  auto integrity = connection.prepare("PRAGMA integrity_check");
  if (integrity.step()) {
    integrity_ok = integrity.column_text(0) == "ok" && !integrity.step();
  }

  auto foreign_keys = connection.prepare("PRAGMA foreign_key_check");
  const bool foreign_keys_ok = !foreign_keys.step();
  return IntegrityResult{
      .integrity_ok = integrity_ok,
      .foreign_keys_ok = foreign_keys_ok,
  };
}

BackupResult DatabaseMaintenance::backup(
    SqliteConnection &source, const std::filesystem::path &source_path,
    const std::filesystem::path &destination_path, const Migrator &migrator,
    const std::chrono::milliseconds busy_timeout) {
  if (normalized_path(source_path) == normalized_path(destination_path)) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Backup source and destination must differ."};
  }
  const auto source_integrity = integrity_check(source);
  if (!source_integrity.ok()) {
    throw DatabaseError{DatabaseErrorCategory::corrupt, SQLITE_CORRUPT,
                        SQLITE_CORRUPT,
                        "Backup source failed database integrity checks."};
  }

  ensure_database_parent(destination_path);
  require_backup_sidecars_absent(destination_path);
  const int destination_descriptor = reserve_destination(destination_path);
  PartialBackupCleanup cleanup{destination_path};
  if (::close(destination_descriptor) != 0) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                        "Backup destination creation failed (io)."};
  }

  {
    auto destination =
        SqliteConnection::open(destination_path, SqliteOpenMode::read_write);
    configure_connection(destination, busy_timeout, false, false);
    copy_database(source.native_handle(), destination.native_handle(),
                  busy_timeout);
  }
  {
    auto portable =
        SqliteConnection::open(destination_path, SqliteOpenMode::read_write);
    configure_connection(portable, busy_timeout, false, false);
    auto mode = portable.prepare("PRAGMA journal_mode = DELETE");
    if (!mode.step() || mode.column_text(0) != "delete" || mode.step()) {
      throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                          SQLITE_ERROR, "Backup journal conversion failed."};
    }
  }
  restrict_database_file(destination_path);

  MigrationStatus status;
  {
    auto verification =
        SqliteConnection::open(destination_path, SqliteOpenMode::read_only);
    configure_connection(verification, busy_timeout, false, false);
    if (!integrity_check(verification).ok()) {
      throw DatabaseError{DatabaseErrorCategory::corrupt, SQLITE_CORRUPT,
                          SQLITE_CORRUPT,
                          "Backup destination failed database integrity "
                          "checks."};
    }
    status = migrator.inspect(verification);
  }

  std::error_code error;
  const auto size = std::filesystem::file_size(destination_path, error);
  if (error) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                        "Backup size verification failed (io)."};
  }
  cleanup.commit();
  return BackupResult{.migration = status, .size_bytes = size};
}

} // namespace sanguinius::persistence
