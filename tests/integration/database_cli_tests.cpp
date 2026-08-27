#include "sanguinius/database_cli.hpp"
#include "sanguinius/persistence/database.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <sstream>
#include <string>

namespace {

struct CommandResult {
  int exit_code{};
  std::string output;
  std::string errors;
};

[[nodiscard]] CommandResult run(const sanguinius::DatabaseCommand &command,
                                const std::filesystem::path &database,
                                const sanguinius::Clock &clock) {
  std::ostringstream output;
  std::ostringstream errors;
  const auto exit_code = sanguinius::run_database_command(
      command, database, {"test-version", "test-revision"}, clock, output,
      errors);
  return {exit_code, output.str(), errors.str()};
}

} // namespace

TEST_CASE(
    "offline database commands migrate verify and back up without secrets",
    "[database-cli]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  const auto backup = temporary.root() / "verified-backup.sqlite3";

  const auto absent =
      run({sanguinius::DatabaseCommandType::status, std::nullopt},
          temporary.path(), clock);
  REQUIRE(absent.exit_code == 0);
  REQUIRE(absent.output.find("database=absent") != std::string::npos);
  REQUIRE(absent.output.find(temporary.root().string()) == std::string::npos);

  const auto absent_check =
      run({sanguinius::DatabaseCommandType::check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(absent_check.exit_code == 1);
  REQUIRE(absent_check.errors == "Database command failed (io).\n");

  const auto migrated =
      run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
          temporary.path(), clock);
  REQUIRE(migrated.exit_code == 0);
  REQUIRE(migrated.output.find("database=current") != std::string::npos);
  REQUIRE(migrated.output.find("current_schema=15") != std::string::npos);

  const auto checked =
      run({sanguinius::DatabaseCommandType::check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(checked.exit_code == 0);
  REQUIRE(checked.errors.empty());

  const auto integrity =
      run({sanguinius::DatabaseCommandType::integrity, std::nullopt},
          temporary.path(), clock);
  REQUIRE(integrity.exit_code == 0);
  REQUIRE(integrity.output == "integrity=ok\nforeign_keys=ok\n");

  const auto relationships =
      run({sanguinius::DatabaseCommandType::relationships_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(relationships.exit_code == 0);
  REQUIRE(relationships.output ==
          "relationships=ok\nevents=0\nprojections=0\nmismatches=0\n");
  const auto tarot =
      run({sanguinius::DatabaseCommandType::tarot_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(tarot.exit_code == 0);
  REQUIRE(tarot.output.find("tarot=ok\n") == 0);
  REQUIRE(tarot.output.find("prepared=0\n") != std::string::npos);
  REQUIRE(tarot.output.find("tarot_player_projection=ok\n") !=
          std::string::npos);
  REQUIRE(tarot.output.find("tarot_player_mismatches=0\n") !=
          std::string::npos);
  const auto tarot_rebuilt =
      run({sanguinius::DatabaseCommandType::tarot_rebuild, std::nullopt},
          temporary.path(), clock);
  REQUIRE(tarot_rebuilt.exit_code == 0);
  REQUIRE(tarot_rebuilt.output == "tarot_player_projection=rebuilt\n"
                                  "tarot_player_events=0\n"
                                  "tarot_player_projections=0\n"
                                  "tarot_player_mismatches=0\n");
  const auto rebuilt = run(
      {sanguinius::DatabaseCommandType::relationships_rebuild, std::nullopt},
      temporary.path(), clock);
  REQUIRE(rebuilt.exit_code == 0);
  REQUIRE(rebuilt.output ==
          "relationships=rebuilt\nevents=0\nprojections=0\nmismatches=0\n");

  const auto backed_up = run({sanguinius::DatabaseCommandType::backup, backup},
                             temporary.path(), clock);
  REQUIRE(backed_up.exit_code == 0);
  REQUIRE(backed_up.output.find("backup=verified") != std::string::npos);
  REQUIRE(backed_up.output.find(temporary.root().string()) ==
          std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(backup));
}

TEST_CASE("database backup command requires its destination",
          "[database-cli][usage]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  for (const auto &destination :
       {std::optional<std::filesystem::path>{std::nullopt},
        std::optional<std::filesystem::path>{std::filesystem::path{}}}) {
    const auto result =
        run({sanguinius::DatabaseCommandType::backup, destination},
            temporary.path(), clock);
    REQUIRE(result.exit_code == 2);
    REQUIRE(result.output.empty());
    REQUIRE(result.errors == "Database backup destination is required.\n");
  }
}

TEST_CASE("incompatible migration fails without changing journal mode",
          "[database-cli][migration][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  {
    auto database =
        sanguinius::persistence::Database::open_migration(temporary.path());
    database.connection().execute(
        "CREATE TABLE unmanaged (id INTEGER PRIMARY KEY) STRICT");
    auto mode = database.connection().prepare("PRAGMA journal_mode");
    REQUIRE(mode.step());
    REQUIRE(mode.column_text(0) == "delete");
  }

  const auto status =
      run({sanguinius::DatabaseCommandType::status, std::nullopt},
          temporary.path(), clock);
  REQUIRE(status.exit_code == 1);
  REQUIRE(status.output.find("database=incompatible") != std::string::npos);
  REQUIRE(status.errors.empty());

  const auto migration =
      run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
          temporary.path(), clock);
  REQUIRE(migration.exit_code == 1);
  REQUIRE(migration.output.empty());
  REQUIRE(migration.errors == "Database command failed (incompatible).\n");

  auto reopened = sanguinius::persistence::SqliteConnection::open(
      temporary.path(), sanguinius::persistence::SqliteOpenMode::read_only);
  auto mode = reopened.prepare("PRAGMA journal_mode");
  REQUIRE(mode.step());
  REQUIRE(mode.column_text(0) == "delete");
  REQUIRE_FALSE(std::filesystem::exists(temporary.path().string() + "-wal"));
  REQUIRE_FALSE(std::filesystem::exists(temporary.path().string() + "-shm"));
}
