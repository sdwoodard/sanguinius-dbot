#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace sanguinius::persistence {

enum class DatabaseErrorCategory {
  busy,
  constraint,
  corrupt,
  io,
  schema,
  incompatible,
  other,
};

class DatabaseError final : public std::runtime_error {
public:
  DatabaseError(DatabaseErrorCategory category, int primary_code,
                int extended_code, std::string message);

  [[nodiscard]] DatabaseErrorCategory category() const noexcept;
  [[nodiscard]] int primary_code() const noexcept;
  [[nodiscard]] int extended_code() const noexcept;

private:
  DatabaseErrorCategory category_;
  int primary_code_;
  int extended_code_;
};

enum class SqliteOpenMode {
  read_only,
  read_write,
  create,
};

class SqliteStatement;

class SqliteConnection {
public:
  static SqliteConnection open(const std::filesystem::path &path,
                               SqliteOpenMode mode);

  ~SqliteConnection();
  SqliteConnection(const SqliteConnection &) = delete;
  SqliteConnection &operator=(const SqliteConnection &) = delete;
  SqliteConnection(SqliteConnection &&other) noexcept;
  SqliteConnection &operator=(SqliteConnection &&other) noexcept;

  [[nodiscard]] SqliteStatement prepare(std::string_view sql);
  void execute(std::string_view sql);
  void execute_script(std::string_view sql);
  [[nodiscard]] std::int64_t changes() const noexcept;
  [[nodiscard]] sqlite3 *native_handle() const noexcept;

private:
  explicit SqliteConnection(sqlite3 *handle) noexcept;
  sqlite3 *handle_{};
};

class SqliteStatement {
public:
  SqliteStatement(sqlite3 *connection, sqlite3_stmt *statement) noexcept;
  ~SqliteStatement();
  SqliteStatement(const SqliteStatement &) = delete;
  SqliteStatement &operator=(const SqliteStatement &) = delete;
  SqliteStatement(SqliteStatement &&other) noexcept;
  SqliteStatement &operator=(SqliteStatement &&other) noexcept;

  void bind(std::size_t index, std::int64_t value);
  void bind(std::size_t index, std::string_view value);
  void bind_null(std::size_t index);
  [[nodiscard]] bool step();
  void execute();
  void reset();

  [[nodiscard]] int column_count() const noexcept;
  [[nodiscard]] bool column_is_null(int index) const;
  [[nodiscard]] std::int64_t column_int64(int index) const;
  [[nodiscard]] std::string column_text(int index) const;

private:
  sqlite3 *connection_{};
  sqlite3_stmt *statement_{};
};

[[nodiscard]] DatabaseErrorCategory
database_error_category(int sqlite_code) noexcept;
[[nodiscard]] const char *
database_error_category_name(DatabaseErrorCategory category) noexcept;

} // namespace sanguinius::persistence
