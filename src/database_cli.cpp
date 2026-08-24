#include "sanguinius/database_cli.hpp"

#include "sanguinius/persistence/backup.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"

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
    case DatabaseCommandType::relationships_check: {
      auto opened = Database::open_inspection(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      const auto result = relationships.check_projection();
      output << "relationships=" << (result.valid ? "ok" : "drift") << '\n'
             << "events=" << result.event_count << '\n'
             << "projections=" << result.projection_count << '\n'
             << "mismatches=" << result.mismatch_count << '\n';
      return result.valid ? 0 : 1;
    }
    case DatabaseCommandType::relationships_rebuild: {
      auto opened = Database::open_migration(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      const auto result = relationships.rebuild_projection();
      output << "relationships=rebuilt\n"
             << "events=" << result.event_count << '\n'
             << "projections=" << result.projection_count << '\n'
             << "mismatches=" << result.mismatch_count << '\n';
      return 0;
    }
    case DatabaseCommandType::tarot_check: {
      auto opened = Database::open_inspection(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteTarotRepository tarot{context};
      persistence::SqliteWagerRepository wagers{context};
      const auto result = tarot.check_invariants();
      const auto wager_result = wagers.check_invariants();
      output << "tarot=" << (result.valid ? "ok" : "failed") << '\n'
             << "accounts=" << result.account_count << '\n'
             << "transactions=" << result.committed_transaction_count << '\n'
             << "postings=" << result.posting_count << '\n'
             << "prepared=" << result.prepared_transaction_count << '\n'
             << "unbalanced=" << result.unbalanced_transaction_count << '\n'
             << "negative_history=" << result.negative_history_count << '\n'
             << "overflow=" << result.overflow_count << '\n'
             << "illegal_reversals=" << result.illegal_reversal_count << '\n'
             << "claim_mismatches=" << result.claim_mismatch_count << '\n'
             << "orphaned_links=" << result.orphaned_link_count << '\n'
             << "wagers=" << (wager_result.valid ? "ok" : "failed") << '\n'
             << "wager_open_funded="
             << wager_result.open_funded_obligation_count << '\n'
             << "wager_obligation_fate="
             << wager_result.open_funded_obligation_amount << '\n'
             << "wager_escrow_fate=" << wager_result.escrow_balance << '\n'
             << "wager_disputes=" << wager_result.disputed_count << '\n'
             << "wager_malformed_transfers="
             << wager_result.malformed_transfer_count << '\n'
             << "wager_invalid_deadline_action_links="
             << wager_result.invalid_deadline_action_link_count << '\n'
             << "wager_orphaned_links=" << wager_result.orphaned_link_count
             << '\n';
      return result.valid && wager_result.valid ? 0 : 1;
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
