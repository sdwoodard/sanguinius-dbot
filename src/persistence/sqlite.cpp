#include "sanguinius/persistence/sqlite.hpp"

#include <sqlite3.h>

#include <cctype>
#include <limits>
#include <utility>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] DatabaseError sqlite_error(sqlite3 *connection,
                                         const int fallback_code,
                                         const std::string_view operation) {
  const int extended = connection == nullptr
                           ? fallback_code
                           : sqlite3_extended_errcode(connection);
  const int primary = extended & 0xff;
  const std::string detail =
      connection == nullptr ? std::string{} : std::string{sqlite3_errmsg(connection)};
  return DatabaseError{
      database_error_category(primary), primary, extended,
      "SQLite " + std::string{operation} + " failed (" +
          database_error_category_name(database_error_category(primary)) +
          ")" + (detail.empty() ? std::string{"."}
                                : std::string{": "} + detail + ".")};
}

[[nodiscard]] bool only_space(const char *begin, const char *end) {
  while (begin != end) {
    if (std::isspace(static_cast<unsigned char>(*begin)) == 0) {
      return false;
    }
    ++begin;
  }
  return true;
}

void validate_column(sqlite3_stmt *statement, const int index) {
  if (index < 0 || index >= sqlite3_column_count(statement)) {
    throw DatabaseError{DatabaseErrorCategory::other, SQLITE_RANGE,
                        SQLITE_RANGE, "SQLite column access failed (other)."};
  }
}

} // namespace

DatabaseError::DatabaseError(const DatabaseErrorCategory category,
                             const int primary_code, const int extended_code,
                             std::string message)
    : std::runtime_error{std::move(message)}, category_{category},
      primary_code_{primary_code}, extended_code_{extended_code} {}

DatabaseErrorCategory DatabaseError::category() const noexcept {
  return category_;
}

int DatabaseError::primary_code() const noexcept { return primary_code_; }

int DatabaseError::extended_code() const noexcept { return extended_code_; }

SqliteConnection SqliteConnection::open(const std::filesystem::path &path,
                                        const SqliteOpenMode mode) {
  int flags = SQLITE_OPEN_FULLMUTEX;
  switch (mode) {
  case SqliteOpenMode::read_only:
    flags |= SQLITE_OPEN_READONLY;
    break;
  case SqliteOpenMode::read_write:
    flags |= SQLITE_OPEN_READWRITE;
    break;
  case SqliteOpenMode::create:
    flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    break;
  }

  sqlite3 *handle = nullptr;
  const auto native_path = path.string();
  const int result =
      sqlite3_open_v2(native_path.c_str(), &handle, flags, nullptr);
  if (result != SQLITE_OK) {
    auto error = sqlite_error(handle, result, "open");
    if (handle != nullptr) {
      static_cast<void>(sqlite3_close_v2(handle));
    }
    throw error;
  }
  static_cast<void>(sqlite3_extended_result_codes(handle, 1));
  return SqliteConnection{handle};
}

SqliteConnection::SqliteConnection(sqlite3 *handle) noexcept
    : handle_{handle} {}

SqliteConnection::~SqliteConnection() {
  if (handle_ != nullptr) {
    static_cast<void>(sqlite3_close_v2(handle_));
  }
}

SqliteConnection::SqliteConnection(SqliteConnection &&other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)} {}

SqliteConnection &
SqliteConnection::operator=(SqliteConnection &&other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      static_cast<void>(sqlite3_close_v2(handle_));
    }
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

SqliteStatement SqliteConnection::prepare(const std::string_view sql) {
  if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw DatabaseError{DatabaseErrorCategory::other, SQLITE_TOOBIG,
                        SQLITE_TOOBIG, "SQLite prepare failed (other)."};
  }

  sqlite3_stmt *statement = nullptr;
  const char *tail = nullptr;
  const int result = sqlite3_prepare_v3(
      handle_, sql.data(), static_cast<int>(sql.size()), 0, &statement, &tail);
  if (result != SQLITE_OK) {
    throw sqlite_error(handle_, result, "prepare");
  }
  if (statement == nullptr || !only_space(tail, sql.data() + sql.size())) {
    if (statement != nullptr) {
      static_cast<void>(sqlite3_finalize(statement));
    }
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_MISUSE,
                        SQLITE_MISUSE, "SQLite prepare failed (schema)."};
  }
  return SqliteStatement{handle_, statement};
}

void SqliteConnection::execute(const std::string_view sql) {
  auto statement = prepare(sql);
  statement.execute();
}

void SqliteConnection::execute_script(const std::string_view sql) {
  const char *cursor = sql.data();
  const char *const end = cursor + sql.size();
  while (cursor != end) {
    sqlite3_stmt *statement = nullptr;
    const char *tail = nullptr;
    const auto remaining = static_cast<std::size_t>(end - cursor);
    if (remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw DatabaseError{DatabaseErrorCategory::other, SQLITE_TOOBIG,
                          SQLITE_TOOBIG,
                          "SQLite script execution failed (other)."};
    }
    const int result = sqlite3_prepare_v3(
        handle_, cursor, static_cast<int>(remaining), 0, &statement, &tail);
    if (result != SQLITE_OK) {
      throw sqlite_error(handle_, result, "script execution");
    }
    cursor = tail;
    if (statement == nullptr) {
      continue;
    }
    SqliteStatement owned{handle_, statement};
    owned.execute();
  }
}

std::int64_t SqliteConnection::changes() const noexcept {
  return sqlite3_changes64(handle_);
}

sqlite3 *SqliteConnection::native_handle() const noexcept { return handle_; }

SqliteStatement::SqliteStatement(sqlite3 *connection,
                                 sqlite3_stmt *statement) noexcept
    : connection_{connection}, statement_{statement} {}

SqliteStatement::~SqliteStatement() {
  if (statement_ != nullptr) {
    static_cast<void>(sqlite3_finalize(statement_));
  }
}

SqliteStatement::SqliteStatement(SqliteStatement &&other) noexcept
    : connection_{std::exchange(other.connection_, nullptr)},
      statement_{std::exchange(other.statement_, nullptr)} {}

SqliteStatement &SqliteStatement::operator=(SqliteStatement &&other) noexcept {
  if (this != &other) {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
    connection_ = std::exchange(other.connection_, nullptr);
    statement_ = std::exchange(other.statement_, nullptr);
  }
  return *this;
}

void SqliteStatement::bind(const std::size_t index, const std::int64_t value) {
  if (index > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      sqlite3_bind_int64(statement_, static_cast<int>(index), value) !=
          SQLITE_OK) {
    throw sqlite_error(connection_, SQLITE_RANGE, "bind");
  }
}

void SqliteStatement::bind(const std::size_t index,
                           const std::string_view value) {
  const char *const text = value.empty() ? "" : value.data();
  if (index > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      value.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      sqlite3_bind_text(statement_, static_cast<int>(index), text,
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw sqlite_error(connection_, SQLITE_RANGE, "bind");
  }
}

void SqliteStatement::bind_null(const std::size_t index) {
  if (index > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      sqlite3_bind_null(statement_, static_cast<int>(index)) != SQLITE_OK) {
    throw sqlite_error(connection_, SQLITE_RANGE, "bind");
  }
}

bool SqliteStatement::step() {
  const int result = sqlite3_step(statement_);
  if (result == SQLITE_ROW) {
    return true;
  }
  if (result == SQLITE_DONE) {
    return false;
  }
  throw sqlite_error(connection_, result, "step");
}

void SqliteStatement::execute() {
  if (step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_MISUSE,
                        SQLITE_MISUSE, "SQLite execute failed (schema)."};
  }
}

void SqliteStatement::reset() {
  const int result = sqlite3_reset(statement_);
  if (result != SQLITE_OK) {
    throw sqlite_error(connection_, result, "reset");
  }
  const int clear_result = sqlite3_clear_bindings(statement_);
  if (clear_result != SQLITE_OK) {
    throw sqlite_error(connection_, clear_result, "reset");
  }
}

int SqliteStatement::column_count() const noexcept {
  return sqlite3_column_count(statement_);
}

bool SqliteStatement::column_is_null(const int index) const {
  validate_column(statement_, index);
  return sqlite3_column_type(statement_, index) == SQLITE_NULL;
}

std::int64_t SqliteStatement::column_int64(const int index) const {
  validate_column(statement_, index);
  if (sqlite3_column_type(statement_, index) != SQLITE_INTEGER) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_MISMATCH,
                        SQLITE_MISMATCH, "SQLite column type failed (schema)."};
  }
  return sqlite3_column_int64(statement_, index);
}

std::string SqliteStatement::column_text(const int index) const {
  validate_column(statement_, index);
  if (sqlite3_column_type(statement_, index) != SQLITE_TEXT) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_MISMATCH,
                        SQLITE_MISMATCH, "SQLite column type failed (schema)."};
  }
  const auto *text = sqlite3_column_text(statement_, index);
  const int bytes = sqlite3_column_bytes(statement_, index);
  return std::string{reinterpret_cast<const char *>(text),
                     static_cast<std::size_t>(bytes)};
}

DatabaseErrorCategory database_error_category(const int sqlite_code) noexcept {
  switch (sqlite_code & 0xff) {
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    return DatabaseErrorCategory::busy;
  case SQLITE_CONSTRAINT:
    return DatabaseErrorCategory::constraint;
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    return DatabaseErrorCategory::corrupt;
  case SQLITE_IOERR:
  case SQLITE_CANTOPEN:
  case SQLITE_FULL:
  case SQLITE_READONLY:
    return DatabaseErrorCategory::io;
  case SQLITE_SCHEMA:
  case SQLITE_ERROR:
  case SQLITE_MISMATCH:
  case SQLITE_FORMAT:
    return DatabaseErrorCategory::schema;
  default:
    return DatabaseErrorCategory::other;
  }
}

const char *
database_error_category_name(const DatabaseErrorCategory category) noexcept {
  switch (category) {
  case DatabaseErrorCategory::busy:
    return "busy";
  case DatabaseErrorCategory::constraint:
    return "constraint";
  case DatabaseErrorCategory::corrupt:
    return "corrupt";
  case DatabaseErrorCategory::io:
    return "io";
  case DatabaseErrorCategory::schema:
    return "schema";
  case DatabaseErrorCategory::incompatible:
    return "incompatible";
  case DatabaseErrorCategory::other:
    return "other";
  }
  return "other";
}

} // namespace sanguinius::persistence
