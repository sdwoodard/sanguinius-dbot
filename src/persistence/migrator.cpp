#include "sanguinius/persistence/migrator.hpp"

#include "sanguinius/persistence/transaction.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] bool migration_table_exists(SqliteConnection &connection) {
  auto statement =
      connection.prepare("SELECT 1 FROM sqlite_schema "
                         "WHERE type = 'table' AND name = 'schema_migrations'");
  const bool exists = statement.step();
  if (exists && statement.step()) {
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Migration metadata query failed (schema)."};
  }
  return exists;
}

struct SchemaObject {
  std::string type;
  std::string name;
  std::string table_name;
  std::string sql;

  [[nodiscard]] bool operator==(const SchemaObject &) const = default;
};

[[nodiscard]] std::vector<SchemaObject>
schema_objects(SqliteConnection &connection) {
  std::vector<SchemaObject> result;
  auto statement = connection.prepare(
      "SELECT type, name, tbl_name, COALESCE(sql, '') FROM sqlite_schema "
      "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name, tbl_name");
  while (statement.step()) {
    result.push_back(SchemaObject{
        .type = statement.column_text(0),
        .name = statement.column_text(1),
        .table_name = statement.column_text(2),
        .sql = statement.column_text(3),
    });
  }
  return result;
}

[[nodiscard]] bool
schema_matches_applied_migrations(SqliteConnection &connection,
                                  const std::span<const Migration> migrations,
                                  const std::size_t applied_count) {
  auto expected = SqliteConnection::open(std::filesystem::path{":memory:"},
                                         SqliteOpenMode::create);
  for (std::size_t index = 0; index < applied_count; ++index) {
    expected.execute_script(migrations[index].sql);
  }
  return schema_objects(connection) == schema_objects(expected);
}

[[nodiscard]] MigrationStatus
incompatible_status(const std::span<const Migration> migrations) {
  return MigrationStatus{
      .state = SchemaState::incompatible,
      .current_version = 0,
      .target_version = migrations.empty() ? 0 : migrations.back().version,
      .pending_count = 0,
  };
}

} // namespace

Migrator::Migrator(const std::span<const Migration> migrations, BuildInfo build,
                   const Clock &clock)
    : migrations_{migrations}, build_{std::move(build)}, clock_{clock} {
  validate_manifest();
}

MigrationStatus Migrator::inspect(SqliteConnection &connection) const {
  try {
    return inspect_validated(connection);
  } catch (const DatabaseError &error) {
    if (error.category() == DatabaseErrorCategory::schema ||
        error.category() == DatabaseErrorCategory::constraint) {
      return incompatible_status(migrations_);
    }
    throw;
  }
}

void Migrator::require_current(SqliteConnection &connection) const {
  const auto status = inspect(connection);
  if (status.state != SchemaState::current) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Database schema is not the required current version."};
  }
}

MigrationStatus Migrator::apply(SqliteConnection &connection) const {
  auto initial = inspect(connection);
  if (initial.state == SchemaState::incompatible) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Database schema is incompatible with this release."};
  }
  if (initial.state == SchemaState::current) {
    return initial;
  }

  Transaction transaction{connection, TransactionMode::exclusive};
  const auto locked = inspect(connection);
  if (locked.state == SchemaState::incompatible) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Database schema changed during migration."};
  }

  const auto start = static_cast<std::size_t>(locked.current_version);
  for (std::size_t index = start; index < migrations_.size(); ++index) {
    const auto &migration = migrations_[index];
    connection.execute_script(migration.sql);
    auto insert = connection.prepare(
        "INSERT INTO schema_migrations "
        "(version, name, checksum, applied_at_ms, application_version) "
        "VALUES (?, ?, ?, ?, ?)");
    insert.bind(1, migration.version);
    insert.bind(2, migration.name);
    insert.bind(3, migration.checksum);
    insert.bind(4, unix_milliseconds(clock_));
    insert.bind(5, build_.version);
    insert.execute();
  }
  const auto final = inspect(connection);
  if (final.state != SchemaState::current) {
    throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Database migration did not reach the target schema."};
  }
  transaction.commit();
  return final;
}

MigrationStatus
Migrator::inspect_validated(SqliteConnection &connection) const {
  const auto target = migrations_.empty() ? 0 : migrations_.back().version;
  if (!migration_table_exists(connection)) {
    if (!schema_objects(connection).empty()) {
      return incompatible_status(migrations_);
    }
    return MigrationStatus{
        .state =
            target == 0 ? SchemaState::current : SchemaState::uninitialized,
        .current_version = 0,
        .target_version = target,
        .pending_count = migrations_.size(),
    };
  }

  auto statement = connection.prepare(
      "SELECT version, name, checksum FROM schema_migrations "
      "ORDER BY version");
  std::size_t index = 0;
  std::int64_t current = 0;
  while (statement.step()) {
    if (index >= migrations_.size()) {
      return incompatible_status(migrations_);
    }
    const auto version = statement.column_int64(0);
    const auto name = statement.column_text(1);
    const auto checksum = statement.column_text(2);
    const auto &expected = migrations_[index];
    if (version != expected.version || name != expected.name ||
        checksum != expected.checksum) {
      return incompatible_status(migrations_);
    }
    current = version;
    ++index;
  }

  if (current < 0 || static_cast<std::size_t>(current) != index) {
    return incompatible_status(migrations_);
  }
  if (!schema_matches_applied_migrations(connection, migrations_, index)) {
    return incompatible_status(migrations_);
  }
  const auto pending = migrations_.size() - index;
  return MigrationStatus{
      .state = pending == 0 ? SchemaState::current : SchemaState::pending,
      .current_version = current,
      .target_version = target,
      .pending_count = pending,
  };
}

void Migrator::validate_manifest() const {
  for (std::size_t index = 0; index < migrations_.size(); ++index) {
    const auto &migration = migrations_[index];
    const auto expected = static_cast<std::int64_t>(index + 1);
    const bool checksum_valid =
        migration.checksum.size() == 64 &&
        std::all_of(migration.checksum.begin(), migration.checksum.end(),
                    [](const char character) {
                      return (character >= '0' && character <= '9') ||
                             (character >= 'a' && character <= 'f');
                    });
    if (migration.version != expected || migration.name.empty() ||
        migration.sql.empty() || !checksum_valid) {
      throw DatabaseError{DatabaseErrorCategory::incompatible, SQLITE_SCHEMA,
                          SQLITE_SCHEMA,
                          "Embedded migration manifest is invalid."};
    }
  }
}

const char *schema_state_name(const SchemaState state) noexcept {
  switch (state) {
  case SchemaState::uninitialized:
    return "uninitialized";
  case SchemaState::current:
    return "current";
  case SchemaState::pending:
    return "pending";
  case SchemaState::incompatible:
    return "incompatible";
  }
  return "incompatible";
}

} // namespace sanguinius::persistence
