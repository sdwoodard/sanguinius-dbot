#include "sanguinius/database_cli.hpp"

#include "sanguinius/persistence/backup.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"

#include <filesystem>
#include <ostream>
#include <system_error>

namespace sanguinius {
namespace {

using persistence::Database;
using persistence::DatabaseError;
using persistence::DatabaseErrorCategory;
using persistence::DatabaseMaintenance;
using persistence::MigrationStatus;
using persistence::Migrator;
using persistence::SchemaState;

void print_status(std::ostream &output, const MigrationStatus &status) {
  output << "database=" << persistence::schema_state_name(status.state) << '\n'
         << "current_schema=" << status.current_version << '\n'
         << "target_schema=" << status.target_version << '\n'
         << "pending_migrations=" << status.pending_count << '\n'
         << "sqlite=" << persistence::sqlite_runtime_version() << '\n';
}

[[nodiscard]] bool wal_mode(persistence::SqliteConnection &connection) {
  auto statement = connection.prepare("PRAGMA journal_mode");
  return statement.step() && statement.column_text(0) == "wal" &&
         !statement.step();
}

} // namespace

int run_database_command(const DatabaseCommand &command,
                         const std::filesystem::path &database,
                         const BuildInfo &build, const Clock &clock,
                         std::ostream &output, std::ostream &errors) {
  try {
    persistence::verify_sqlite_runtime();
    const Migrator migrator{persistence::production_migrations(), build, clock};
    if (command.type == DatabaseCommandType::status) {
      std::error_code status_error;
      const auto file_status = std::filesystem::status(database, status_error);
      const bool absent =
          status_error == std::errc::no_such_file_or_directory ||
          (!status_error && !std::filesystem::exists(file_status));
      if (status_error && !absent) {
        throw DatabaseError{DatabaseErrorCategory::io, 0, 0,
                            "Database status inspection failed (io)."};
      }
      if (absent) {
        const auto status = migrator.version_zero_status();
        output << "database=absent\n"
               << "current_schema=" << status.current_version << '\n'
               << "target_schema=" << status.target_version << '\n'
               << "pending_migrations=" << status.pending_count << '\n'
               << "sqlite=" << persistence::sqlite_runtime_version() << '\n';
        return 0;
      }
      if (!std::filesystem::is_regular_file(file_status)) {
        throw DatabaseError{DatabaseErrorCategory::io, 0, 0,
                            "Database status inspection failed (io)."};
      }
    }

    switch (command.type) {
    case DatabaseCommandType::status: {
      auto opened = Database::open_inspection(database);
      auto status = migrator.inspect(opened.connection());
      if (status.state == SchemaState::current &&
          !wal_mode(opened.connection())) {
        status.state = SchemaState::incompatible;
      }
      print_status(output, status);
      return status.state == SchemaState::incompatible ? 1 : 0;
    }
    case DatabaseCommandType::check: {
      auto opened = Database::open_inspection(database);
      if (!wal_mode(opened.connection())) {
        throw DatabaseError{DatabaseErrorCategory::incompatible, 0, 0,
                            "Database is not in required WAL mode."};
      }
      migrator.require_current(opened.connection());
      print_status(output, migrator.inspect(opened.connection()));
      return 0;
    }
    case DatabaseCommandType::migrate: {
      auto opened = Database::open_migration(database);
      const auto status = migrator.apply(opened.connection());
      print_status(output, status);
      return 0;
    }
    case DatabaseCommandType::integrity: {
      auto opened = Database::open_inspection(database);
      const auto result =
          DatabaseMaintenance::integrity_check(opened.connection());
      output << "integrity=" << (result.integrity_ok ? "ok" : "failed") << '\n'
             << "foreign_keys=" << (result.foreign_keys_ok ? "ok" : "failed")
             << '\n';
      return result.ok() ? 0 : 1;
    }
    case DatabaseCommandType::backup: {
      if (!command.destination.has_value() || command.destination->empty()) {
        errors << "Database backup destination is required.\n";
        return 2;
      }
      auto opened = Database::open_inspection(database);
      const auto result = DatabaseMaintenance::backup(
          opened.connection(), database, *command.destination, migrator);
      output << "backup=verified\n"
             << "schema_state="
             << persistence::schema_state_name(result.migration.state) << '\n'
             << "schema=" << result.migration.current_version << '\n'
             << "size_bytes=" << result.size_bytes << '\n';
      return 0;
    }
    }
  } catch (const DatabaseError &error) {
    errors << "Database command failed ("
           << persistence::database_error_category_name(error.category())
           << ").\n";
    return 1;
  } catch (const std::exception &) {
    errors << "Database command failed (other).\n";
    return 1;
  }
  errors << "Database command failed (other).\n";
  return 1;
}

} // namespace sanguinius
