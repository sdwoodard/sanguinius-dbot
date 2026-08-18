#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>

namespace {

using namespace std::chrono_literals;
using sanguinius::persistence::Database;
using sanguinius::persistence::DatabaseError;
using sanguinius::persistence::Migration;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SchemaState;

[[nodiscard]] Migrator production_migrator(const sanguinius::Clock &clock) {
  return Migrator{sanguinius::persistence::production_migrations(),
                  {"test-version", "test-revision"},
                  clock};
}

[[nodiscard]] std::int64_t count(sanguinius::persistence::SqliteConnection &db,
                                 const std::string_view table) {
  auto statement = db.prepare("SELECT count(*) FROM " + std::string{table});
  REQUIRE(statement.step());
  return statement.column_int64(0);
}

} // namespace

TEST_CASE("production migration moves an empty database to version one",
          "[migration]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{123}}};
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto migrator = production_migrator(clock);

  const auto before = migrator.inspect(database.connection());
  REQUIRE(before.state == SchemaState::uninitialized);
  REQUIRE(before.current_version == 0);
  REQUIRE(before.target_version == 1);

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 1);
  REQUIRE(count(database.connection(), "schema_migrations") == 1);
  REQUIRE(count(database.connection(), "application_instance") == 0);
  REQUIRE(count(database.connection(), "guild_config") == 0);

  auto history = database.connection().prepare(
      "SELECT name, checksum, applied_at_ms FROM schema_migrations");
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "core_foundation");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);

  clock.set(std::chrono::sys_seconds{std::chrono::seconds{456}});
  const auto repeated = migrator.apply(database.connection());
  REQUIRE(repeated.state == SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 1);
  auto unchanged = database.connection().prepare(
      "SELECT applied_at_ms FROM schema_migrations");
  REQUIRE(unchanged.step());
  REQUIRE(unchanged.column_int64(0) == 123'000);
}

TEST_CASE("migration metadata detects checksum gaps and newer schemas",
          "[migration]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto migrator = production_migrator(clock);
  static_cast<void>(migrator.apply(database.connection()));

  database.connection().execute(
      "UPDATE schema_migrations SET checksum = "
      "'0000000000000000000000000000000000000000000000000000000000000000'");
  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);
  REQUIRE_THROWS_AS(migrator.require_current(database.connection()),
                    DatabaseError);

  database.connection().execute(
      "UPDATE schema_migrations SET checksum = '" +
      std::string{
          sanguinius::persistence::production_migrations()[0].checksum} +
      "', name = 'renamed'");
  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);

  database.connection().execute("DELETE FROM schema_migrations");
  database.connection().execute(
      "INSERT INTO schema_migrations VALUES "
      "(2, 'future', "
      "'0000000000000000000000000000000000000000000000000000000000000000', "
      "0, 'future')");
  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);
}

TEST_CASE("unmanaged schema and malformed migration table fail closed",
          "[migration]") {
  sanguinius::test::TemporaryDatabase unmanaged;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(unmanaged.path(), 25ms);
  database.connection().execute("CREATE TABLE unrelated (id INTEGER) STRICT");
  REQUIRE(production_migrator(clock).inspect(database.connection()).state ==
          SchemaState::incompatible);

  sanguinius::test::TemporaryDatabase malformed;
  auto malformed_database = Database::open_migration(malformed.path(), 25ms);
  malformed_database.connection().execute(
      "CREATE TABLE schema_migrations (wrong INTEGER) STRICT");
  REQUIRE(production_migrator(clock)
              .inspect(malformed_database.connection())
              .state == SchemaState::incompatible);

  sanguinius::test::TemporaryDatabase wrong_layout;
  auto wrong_layout_database =
      Database::open_migration(wrong_layout.path(), 25ms);
  auto migrator = production_migrator(clock);
  static_cast<void>(migrator.apply(wrong_layout_database.connection()));
  wrong_layout_database.connection().execute("DROP TABLE user_preference");
  REQUIRE(migrator.inspect(wrong_layout_database.connection()).state ==
          SchemaState::incompatible);
}

TEST_CASE("only a completely empty schema is migration version zero",
          "[migration]") {
  sanguinius::test::FakeClock clock;

  SECTION("an unmanaged view is not an empty database") {
    sanguinius::test::TemporaryDatabase temporary;
    auto database = Database::open_migration(temporary.path(), 25ms);
    database.connection().execute(
        "CREATE VIEW unmanaged_view AS SELECT 1 AS value");
    auto migrator = production_migrator(clock);

    REQUIRE(migrator.inspect(database.connection()).state ==
            SchemaState::incompatible);
    REQUIRE_THROWS_AS(migrator.apply(database.connection()), DatabaseError);
  }

  SECTION("an empty migration-history table is malformed state") {
    sanguinius::test::TemporaryDatabase temporary;
    auto database = Database::open_migration(temporary.path(), 25ms);
    auto migrator = production_migrator(clock);
    static_cast<void>(migrator.apply(database.connection()));
    database.connection().execute("DROP TABLE user_preference");
    database.connection().execute("DROP TABLE guild_config");
    database.connection().execute("DROP TABLE discord_user");
    database.connection().execute("DROP TABLE application_instance");
    database.connection().execute("DELETE FROM schema_migrations");

    REQUIRE(migrator.inspect(database.connection()).state ==
            SchemaState::incompatible);
    REQUIRE_THROWS_AS(migrator.apply(database.connection()), DatabaseError);
  }
}

TEST_CASE("migration validation includes constraints defaults and foreign keys",
          "[migration]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto migrator = production_migrator(clock);
  static_cast<void>(migrator.apply(database.connection()));

  database.connection().execute_script(
      "ALTER TABLE user_preference RENAME TO user_preference_original;"
      "CREATE TABLE user_preference ("
      "user_id TEXT PRIMARY KEY,"
      "chronicle_opt_in INTEGER NOT NULL,"
      "memory_callback_opt_in INTEGER NOT NULL,"
      "appearance_callback_opt_in INTEGER NOT NULL,"
      "voice_input_opt_in INTEGER NOT NULL,"
      "public_tarot_results_opt_in INTEGER NOT NULL,"
      "quiet_until_ms INTEGER,"
      "updated_at_ms INTEGER NOT NULL) STRICT;"
      "DROP TABLE user_preference_original;");

  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);
  REQUIRE_THROWS_AS(migrator.require_current(database.connection()),
                    DatabaseError);
}

TEST_CASE("schema validation follows every applied migration version",
          "[migration]") {
  constexpr std::string_view checksum{
      "0000000000000000000000000000000000000000000000000000000000000000"};
  const auto &production =
      sanguinius::persistence::production_migrations().front();
  const std::array<Migration, 2> migrations{
      production,
      Migration{2, "future_table", checksum,
                "CREATE TABLE future_table (id INTEGER PRIMARY KEY) STRICT;"},
  };
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const Migrator migrator{migrations, {"test", "revision"}, clock};

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 2);
  REQUIRE(count(database.connection(), "future_table") == 0);
}

TEST_CASE("a failing migration batch rolls every pending migration back",
          "[migration][rollback]") {
  constexpr std::string_view checksum{
      "0000000000000000000000000000000000000000000000000000000000000000"};
  const std::array<Migration, 2> migrations{
      Migration{1, "first", checksum,
                "CREATE TABLE schema_migrations ("
                "version INTEGER PRIMARY KEY, name TEXT NOT NULL, "
                "checksum TEXT NOT NULL, applied_at_ms INTEGER NOT NULL, "
                "application_version TEXT NOT NULL) STRICT; "
                "CREATE TABLE first_table (id INTEGER) STRICT;"},
      Migration{2, "second", checksum,
                "CREATE TABLE second_table (id INTEGER) STRICT; "
                "THIS IS NOT SQL;"},
  };
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const Migrator migrator{migrations, {"test", "revision"}, clock};
  REQUIRE_THROWS_AS(migrator.apply(database.connection()), DatabaseError);

  auto tables = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type = 'table' "
      "AND name NOT LIKE 'sqlite_%'");
  REQUIRE(tables.step());
  REQUIRE(tables.column_int64(0) == 0);
}

TEST_CASE("final migration validation occurs before commit",
          "[migration][rollback]") {
  constexpr std::string_view checksum{
      "0000000000000000000000000000000000000000000000000000000000000000"};
  const std::array<Migration, 1> migrations{Migration{
      1, "only", checksum,
      "CREATE TABLE schema_migrations ("
      "version INTEGER PRIMARY KEY, name TEXT NOT NULL, "
      "checksum TEXT NOT NULL, applied_at_ms INTEGER NOT NULL, "
      "application_version TEXT NOT NULL) STRICT; "
      "CREATE TABLE should_rollback (id INTEGER) STRICT; "
      "INSERT INTO schema_migrations VALUES ("
      "2, 'forged', "
      "'0000000000000000000000000000000000000000000000000000000000000000', "
      "0, 'forged');"}};
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const Migrator migrator{migrations, {"test", "revision"}, clock};

  REQUIRE_THROWS_AS(migrator.apply(database.connection()), DatabaseError);
  auto tables = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'");
  REQUIRE(tables.step());
  REQUIRE(tables.column_int64(0) == 0);
}
