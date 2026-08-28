#include "sanguinius/persistence/database.hpp"

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <limits>
#include <system_error>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr int maximum_value_size = 4 * 1024 * 1024;
constexpr int maximum_sql_size = 1024 * 1024;
constexpr int maximum_columns = 256;
constexpr int maximum_variables = 1024;
constexpr int maximum_compound_terms = 64;
constexpr int maximum_expression_depth = 100;
constexpr int maximum_like_pattern = 4096;
constexpr int maximum_trigger_depth = 32;
constexpr int maximum_attached = 0;
constexpr int maximum_worker_threads = 0;
constexpr mode_t private_lock_permissions =
    static_cast<mode_t>(S_IRUSR | S_IWUSR);
constexpr mode_t permission_bits =
    static_cast<mode_t>(S_IRWXU | S_IRWXG | S_IRWXO);

[[nodiscard]] std::filesystem::path
lock_path(const std::filesystem::path &database_path) {
  std::error_code canonical_error;
  auto result =
      std::filesystem::weakly_canonical(database_path, canonical_error);
  if (canonical_error) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Database lock path resolution failed (io)."};
  }

  std::error_code status_error;
  const auto status = std::filesystem::status(database_path, status_error);
  const bool absent = status_error == std::errc::no_such_file_or_directory ||
                      (!status_error && !std::filesystem::exists(status));
  if (status_error && !absent) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Database lock path inspection failed (io)."};
  }
  if (!absent && std::filesystem::is_regular_file(status)) {
    std::error_code link_error;
    const auto links =
        std::filesystem::hard_link_count(database_path, link_error);
    if (link_error) {
      throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                          SQLITE_CANTOPEN,
                          "Database link inspection failed (io)."};
    }
    if (links != 1) {
      throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_CANTOPEN,
                          SQLITE_CANTOPEN,
                          "Database hard-link aliases are not supported."};
    }
  }

  result += ".lock";
  return result;
}

[[nodiscard]] std::int64_t scalar_integer(SqliteConnection &connection,
                                          const std::string_view sql) {
  auto statement = connection.prepare(sql);
  if (!statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "SQLite configuration verification failed (schema)."};
  }
  const auto value = statement.column_int64(0);
  if (statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "SQLite configuration verification failed (schema)."};
  }
  return value;
}

[[nodiscard]] std::string scalar_text(SqliteConnection &connection,
                                      const std::string_view sql) {
  auto statement = connection.prepare(sql);
  if (!statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "SQLite configuration verification failed (schema)."};
  }
  auto value = statement.column_text(0);
  if (statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "SQLite configuration verification failed (schema)."};
  }
  return value;
}

void require_db_config(sqlite3 *handle, const int option, const int value) {
  int actual = 0;
  const int result = sqlite3_db_config(handle, option, value, &actual);
  if (result != SQLITE_OK || actual != value) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, result, result,
                        "SQLite runtime configuration is incompatible."};
  }
}

void set_limit(sqlite3 *handle, const int limit, const int value) {
  static_cast<void>(sqlite3_limit(handle, limit, value));
  const int actual = sqlite3_limit(handle, limit, -1);
  if (actual != value) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR, "SQLite runtime limit is incompatible."};
  }
}

void verify_connection_settings(SqliteConnection &connection,
                                const bool require_wal) {
  if (scalar_integer(connection, "PRAGMA foreign_keys") != 1 ||
      scalar_integer(connection, "PRAGMA synchronous") != 2 ||
      scalar_text(connection, "PRAGMA locking_mode") != "normal") {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite connection settings are incompatible."};
  }
  if (require_wal && scalar_text(connection, "PRAGMA journal_mode") != "wal") {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite database is not in required WAL mode."};
  }
}

} // namespace

bool sqlite_has_wal_reset_fix(const int version_number) noexcept {
  return version_number >= minimum_fixed_sqlite_version ||
         version_number == fixed_sqlite_backport_3_50_7 ||
         version_number == fixed_sqlite_backport_3_44_6;
}

void verify_sqlite_runtime() {
  if (sqlite3_threadsafe() == 0 ||
      !sqlite_has_wal_reset_fix(sqlite3_libversion_number())) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite runtime does not satisfy persistence safety "
                        "requirements."};
  }
}

DatabaseFileLock
DatabaseFileLock::acquire(const std::filesystem::path &database_path,
                          const DatabaseLockMode mode) {
  const auto native_path = lock_path(database_path).string();
  const int descriptor =
      ::open(native_path.c_str(),
             O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
             private_lock_permissions);
  if (descriptor < 0) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN, "Database lock open failed (io)."};
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1) {
    static_cast<void>(::close(descriptor));
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN, "Database lock file is incompatible."};
  }
  if ((status.st_mode & permission_bits) != private_lock_permissions &&
      ::fchmod(descriptor, private_lock_permissions) != 0) {
    static_cast<void>(::close(descriptor));
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                        "Database lock permissions failed (io)."};
  }
  const int operation =
      (mode == DatabaseLockMode::shared ? LOCK_SH : LOCK_EX) | LOCK_NB;
  if (::flock(descriptor, operation) != 0) {
    const int error = errno;
    static_cast<void>(::close(descriptor));
    const bool busy = error == EWOULDBLOCK || error == EAGAIN;
    const auto category =
        busy ? DatabaseErrorCategory::busy : DatabaseErrorCategory::io;
    const int code = busy ? SQLITE_BUSY : SQLITE_IOERR;
    throw DatabaseError{
        category, code, code,
        "Database lock acquisition failed (" +
            std::string{database_error_category_name(category)} + ")."};
  }
  return DatabaseFileLock{descriptor};
}

DatabaseFileLock::DatabaseFileLock(const int descriptor) noexcept
    : descriptor_{descriptor} {}

DatabaseFileLock::~DatabaseFileLock() {
  if (descriptor_ >= 0) {
    static_cast<void>(::flock(descriptor_, LOCK_UN));
    static_cast<void>(::close(descriptor_));
  }
}

DatabaseFileLock::DatabaseFileLock(DatabaseFileLock &&other) noexcept
    : descriptor_{std::exchange(other.descriptor_, -1)} {}

DatabaseFileLock &
DatabaseFileLock::operator=(DatabaseFileLock &&other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) {
      static_cast<void>(::flock(descriptor_, LOCK_UN));
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}

Database Database::open_runtime(const std::filesystem::path &path,
                                const std::chrono::milliseconds busy_timeout) {
  verify_sqlite_runtime();
  std::error_code file_error;
  const bool regular = std::filesystem::is_regular_file(path, file_error);
  if (file_error || !regular) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Configured database does not exist; run db migrate."};
  }
  auto lock = DatabaseFileLock::acquire(path, DatabaseLockMode::shared);
  auto connection = SqliteConnection::open(path, SqliteOpenMode::read_write);
  configure_connection(connection, busy_timeout, false, true);
  return Database{std::move(lock), std::move(connection)};
}

Database
Database::open_migration(const std::filesystem::path &path,
                         const std::chrono::milliseconds busy_timeout) {
  verify_sqlite_runtime();
  ensure_database_parent(path);
  auto lock = DatabaseFileLock::acquire(path, DatabaseLockMode::exclusive);
  auto connection = SqliteConnection::open(path, SqliteOpenMode::create);
  restrict_database_file(path);
  // Migrator::apply validates compatibility before making WAL persistent.
  configure_connection(connection, busy_timeout, false, false);
  restrict_database_file(path);
  return Database{std::move(lock), std::move(connection)};
}

Database
Database::open_inspection(const std::filesystem::path &path,
                          const std::chrono::milliseconds busy_timeout) {
  verify_sqlite_runtime();
  std::error_code file_error;
  const bool regular = std::filesystem::is_regular_file(path, file_error);
  if (file_error || !regular) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN, "Configured database does not exist."};
  }
  auto lock = DatabaseFileLock::acquire(path, DatabaseLockMode::shared);
  auto connection = SqliteConnection::open(path, SqliteOpenMode::read_only);
  configure_connection(connection, busy_timeout, false, false);
  return Database{std::move(lock), std::move(connection)};
}

Database::Database(DatabaseFileLock lock, SqliteConnection connection) noexcept
    : lock_{std::move(lock)}, connection_{std::move(connection)} {}

SqliteConnection &Database::connection() noexcept { return connection_; }

const SqliteConnection &Database::connection() const noexcept {
  return connection_;
}

void configure_connection(SqliteConnection &connection,
                          const std::chrono::milliseconds busy_timeout,
                          const bool enable_wal, const bool require_wal) {
  verify_sqlite_runtime();
  if (busy_timeout.count() < 0 ||
      busy_timeout.count() > std::numeric_limits<int>::max() ||
      sqlite3_busy_timeout(connection.native_handle(),
                           static_cast<int>(busy_timeout.count())) !=
          SQLITE_OK) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite busy timeout configuration failed."};
  }
  if (scalar_integer(connection, "PRAGMA busy_timeout") !=
      busy_timeout.count()) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite busy timeout verification failed."};
  }

  require_db_config(connection.native_handle(), SQLITE_DBCONFIG_DEFENSIVE, 1);
  require_db_config(connection.native_handle(),
                    SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0);
  require_db_config(connection.native_handle(), SQLITE_DBCONFIG_TRUSTED_SCHEMA,
                    0);
  require_db_config(connection.native_handle(), SQLITE_DBCONFIG_DQS_DML, 0);
  require_db_config(connection.native_handle(), SQLITE_DBCONFIG_DQS_DDL, 0);

  set_limit(connection.native_handle(), SQLITE_LIMIT_LENGTH,
            maximum_value_size);
  set_limit(connection.native_handle(), SQLITE_LIMIT_SQL_LENGTH,
            maximum_sql_size);
  set_limit(connection.native_handle(), SQLITE_LIMIT_COLUMN, maximum_columns);
  set_limit(connection.native_handle(), SQLITE_LIMIT_VARIABLE_NUMBER,
            maximum_variables);
  set_limit(connection.native_handle(), SQLITE_LIMIT_COMPOUND_SELECT,
            maximum_compound_terms);
  set_limit(connection.native_handle(), SQLITE_LIMIT_EXPR_DEPTH,
            maximum_expression_depth);
  set_limit(connection.native_handle(), SQLITE_LIMIT_LIKE_PATTERN_LENGTH,
            maximum_like_pattern);
  set_limit(connection.native_handle(), SQLITE_LIMIT_TRIGGER_DEPTH,
            maximum_trigger_depth);
  set_limit(connection.native_handle(), SQLITE_LIMIT_ATTACHED,
            maximum_attached);
  set_limit(connection.native_handle(), SQLITE_LIMIT_WORKER_THREADS,
            maximum_worker_threads);

  connection.execute("PRAGMA foreign_keys = ON");
  connection.execute("PRAGMA synchronous = FULL");
  if (scalar_text(connection, "PRAGMA locking_mode = NORMAL") != "normal") {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite could not enable normal locking mode."};
  }
  if (enable_wal) {
    enable_wal_mode(connection);
  } else if (require_wal) {
    if (scalar_integer(connection, "PRAGMA wal_autocheckpoint = 1000") !=
            1000 ||
        scalar_integer(connection, "PRAGMA journal_size_limit = 16777216") !=
            16777216) {
      throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                          SQLITE_ERROR,
                          "SQLite WAL settings are incompatible."};
    }
  }
  verify_connection_settings(connection, require_wal);
}

void enable_wal_mode(SqliteConnection &connection) {
  if (scalar_text(connection, "PRAGMA journal_mode = WAL") != "wal") {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR,
                        "SQLite could not enable required WAL mode."};
  }
  if (scalar_integer(connection, "PRAGMA wal_autocheckpoint = 1000") != 1000 ||
      scalar_integer(connection, "PRAGMA journal_size_limit = 16777216") !=
          16777216) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_ERROR,
                        SQLITE_ERROR, "SQLite WAL settings are incompatible."};
  }
  verify_connection_settings(connection, true);
}

void ensure_database_parent(const std::filesystem::path &path) {
  if (!path.has_parent_path()) {
    return;
  }
  const auto parent = path.parent_path();
  std::error_code error;
  const bool created = std::filesystem::create_directories(parent, error);
  if (error) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Database state directory creation failed (io)."};
  }
  const bool directory = std::filesystem::is_directory(parent, error);
  if (error || !directory) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_CANTOPEN,
                        SQLITE_CANTOPEN,
                        "Database state directory creation failed (io)."};
  }
  if (created) {
    std::filesystem::permissions(parent, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                          "Database state directory permissions failed (io)."};
    }
  }
}

void restrict_database_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    throw DatabaseError{DatabaseErrorCategory::io, SQLITE_IOERR, SQLITE_IOERR,
                        "Database file permissions failed (io)."};
  }
}

std::string sqlite_runtime_version() { return sqlite3_libversion(); }

std::string sqlite_runtime_source_id() { return sqlite3_sourceid(); }

} // namespace sanguinius::persistence
