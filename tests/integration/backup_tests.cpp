#include "sanguinius/persistence/backup.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

using namespace std::chrono_literals;

} // namespace

TEST_CASE("online backup produces a portable verified restorable database",
          "[backup]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto source =
      sanguinius::persistence::Database::open_migration(temporary.path(), 25ms);
  sanguinius::persistence::Migrator migrator{
      sanguinius::persistence::production_migrations(),
      {"test-version", "test-revision"},
      clock};
  static_cast<void>(migrator.apply(source.connection()));
  source.connection().execute("INSERT INTO discord_user VALUES "
                              "('42', 'User', 'user', 0, 1, 1, 1, 1)");
  source.connection().execute(
      "INSERT INTO user_preference (user_id, updated_at_ms) VALUES ('42', 1)");

  const auto backup_path = temporary.root() / "backup.sqlite3";
  const auto result = sanguinius::persistence::DatabaseMaintenance::backup(
      source.connection(), temporary.path(), backup_path, migrator, 25ms);
  REQUIRE(result.migration.state ==
          sanguinius::persistence::SchemaState::current);
  REQUIRE(result.migration.current_version == 3);
  REQUIRE(result.size_bytes > 0);
  REQUIRE(std::filesystem::is_regular_file(backup_path));
  const auto permissions = std::filesystem::status(backup_path).permissions();
  REQUIRE((permissions & std::filesystem::perms::owner_read) !=
          std::filesystem::perms::none);
  REQUIRE((permissions & std::filesystem::perms::owner_write) !=
          std::filesystem::perms::none);
  REQUIRE((permissions & std::filesystem::perms::group_all) ==
          std::filesystem::perms::none);
  REQUIRE((permissions & std::filesystem::perms::others_all) ==
          std::filesystem::perms::none);
  REQUIRE_FALSE(std::filesystem::exists(backup_path.string() + "-wal"));
  REQUIRE_FALSE(std::filesystem::exists(backup_path.string() + "-shm"));
  REQUIRE_FALSE(std::filesystem::exists(backup_path.string() + "-journal"));

  auto restored = sanguinius::persistence::SqliteConnection::open(
      backup_path, sanguinius::persistence::SqliteOpenMode::read_only);
  sanguinius::persistence::configure_connection(restored, 25ms, false, false);
  REQUIRE(
      sanguinius::persistence::DatabaseMaintenance::integrity_check(restored)
          .ok());
  migrator.require_current(restored);
  auto users = restored.prepare("SELECT count(*) FROM discord_user");
  REQUIRE(users.step());
  REQUIRE(users.column_int64(0) == 1);
  auto mode = restored.prepare("PRAGMA journal_mode");
  REQUIRE(mode.step());
  REQUIRE(mode.column_text(0) == "delete");
}

TEST_CASE("backup refuses overwrite and source destination aliasing",
          "[backup]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto source =
      sanguinius::persistence::Database::open_migration(temporary.path(), 25ms);
  sanguinius::persistence::Migrator migrator{
      sanguinius::persistence::production_migrations(),
      {"test", "revision"},
      clock};
  static_cast<void>(migrator.apply(source.connection()));

  REQUIRE_THROWS_AS(sanguinius::persistence::DatabaseMaintenance::backup(
                        source.connection(), temporary.path(), temporary.path(),
                        migrator, 25ms),
                    sanguinius::persistence::DatabaseError);
  const auto existing = temporary.root() / "existing.sqlite3";
  {
    auto file = sanguinius::persistence::SqliteConnection::open(
        existing, sanguinius::persistence::SqliteOpenMode::create);
  }
  REQUIRE_THROWS_AS(
      sanguinius::persistence::DatabaseMaintenance::backup(
          source.connection(), temporary.path(), existing, migrator, 25ms),
      sanguinius::persistence::DatabaseError);

  const auto reserved = temporary.root() / "reserved.sqlite3";
  const auto reserved_wal = reserved.string() + "-wal";
  {
    std::ofstream sidecar{reserved_wal};
    REQUIRE(sidecar.is_open());
    sidecar << "pre-existing sidecar";
  }
  REQUIRE_THROWS_AS(
      sanguinius::persistence::DatabaseMaintenance::backup(
          source.connection(), temporary.path(), reserved, migrator, 25ms),
      sanguinius::persistence::DatabaseError);
  REQUIRE_FALSE(std::filesystem::exists(reserved));
  REQUIRE(std::filesystem::is_regular_file(reserved_wal));
  {
    std::ifstream sidecar{reserved_wal};
    std::string contents;
    std::getline(sidecar, contents);
    REQUIRE(contents == "pre-existing sidecar");
  }
}

TEST_CASE("backup removes files created by an incomplete operation",
          "[backup]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto source =
      sanguinius::persistence::Database::open_migration(temporary.path(), 25ms);
  sanguinius::persistence::Migrator migrator{
      sanguinius::persistence::production_migrations(),
      {"test", "revision"},
      clock};
  static_cast<void>(migrator.apply(source.connection()));
  const auto incomplete = temporary.root() / "incomplete.sqlite3";

  REQUIRE_THROWS_AS(
      sanguinius::persistence::DatabaseMaintenance::backup(
          source.connection(), temporary.path(), incomplete, migrator, -1ms),
      sanguinius::persistence::DatabaseError);
  REQUIRE_FALSE(std::filesystem::exists(incomplete));
  REQUIRE_FALSE(std::filesystem::exists(incomplete.string() + "-wal"));
  REQUIRE_FALSE(std::filesystem::exists(incomplete.string() + "-shm"));
  REQUIRE_FALSE(std::filesystem::exists(incomplete.string() + "-journal"));
}

TEST_CASE("integrity rejects foreign-key violations and corrupted copies",
          "[backup][integrity]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto source =
      sanguinius::persistence::Database::open_migration(temporary.path(), 25ms);
  sanguinius::persistence::Migrator migrator{
      sanguinius::persistence::production_migrations(),
      {"test", "revision"},
      clock};
  static_cast<void>(migrator.apply(source.connection()));

  source.connection().execute("PRAGMA foreign_keys = OFF");
  source.connection().execute(
      "INSERT INTO user_preference (user_id, updated_at_ms) VALUES ('99', 1)");
  const auto incomplete = temporary.root() / "must-not-remain.sqlite3";
  REQUIRE_THROWS_AS(
      sanguinius::persistence::DatabaseMaintenance::backup(
          source.connection(), temporary.path(), incomplete, migrator, 25ms),
      sanguinius::persistence::DatabaseError);
  REQUIRE_FALSE(std::filesystem::exists(incomplete));

  source.connection().execute(
      "DELETE FROM user_preference WHERE user_id = '99'");
  source.connection().execute("PRAGMA foreign_keys = ON");
  const auto damaged = temporary.root() / "damaged.sqlite3";
  static_cast<void>(sanguinius::persistence::DatabaseMaintenance::backup(
      source.connection(), temporary.path(), damaged, migrator, 25ms));
  {
    std::fstream file{damaged, std::ios::in | std::ios::out | std::ios::binary};
    REQUIRE(file.is_open());
    file.write("not-a-sqlite-database", 21);
    REQUIRE(file.good());
  }
  REQUIRE_THROWS_AS(
      sanguinius::persistence::Database::open_inspection(damaged, 25ms),
      sanguinius::persistence::DatabaseError);
}
