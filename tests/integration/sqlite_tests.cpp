#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <chrono>
#include <type_traits>

namespace {

using namespace std::chrono_literals;
using sanguinius::persistence::Database;
using sanguinius::persistence::DatabaseError;
using sanguinius::persistence::DatabaseErrorCategory;
using sanguinius::persistence::SqliteConnection;
using sanguinius::persistence::SqliteOpenMode;
using sanguinius::persistence::Transaction;
using sanguinius::persistence::TransactionMode;

[[nodiscard]] std::int64_t scalar(SqliteConnection &connection,
                                  const std::string_view sql) {
  auto statement = connection.prepare(sql);
  REQUIRE(statement.step());
  const auto result = statement.column_int64(0);
  REQUIRE_FALSE(statement.step());
  return result;
}

} // namespace

TEST_CASE("SQLite connections enforce WAL safety and defensive limits",
          "[sqlite]") {
  sanguinius::test::TemporaryDatabase temporary;
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto &connection = database.connection();

  const auto require_private = [](const std::filesystem::path &path) {
    const auto permissions = std::filesystem::status(path).permissions();
    REQUIRE((permissions & std::filesystem::perms::owner_read) !=
            std::filesystem::perms::none);
    REQUIRE((permissions & std::filesystem::perms::owner_write) !=
            std::filesystem::perms::none);
    REQUIRE((permissions & std::filesystem::perms::group_all) ==
            std::filesystem::perms::none);
    REQUIRE((permissions & std::filesystem::perms::others_all) ==
            std::filesystem::perms::none);
  };
  require_private(temporary.path());
  require_private(temporary.path().string() + ".lock");
  for (const std::string suffix : {"-wal", "-shm"}) {
    const auto sidecar = temporary.path().string() + suffix;
    if (std::filesystem::exists(sidecar)) {
      require_private(sidecar);
    }
  }

  auto text = connection.prepare("PRAGMA journal_mode");
  REQUIRE(text.step());
  REQUIRE(text.column_text(0) == "wal");
  REQUIRE(scalar(connection, "PRAGMA foreign_keys") == 1);
  REQUIRE(scalar(connection, "PRAGMA synchronous") == 2);
  REQUIRE(scalar(connection, "PRAGMA busy_timeout") == 25);
  REQUIRE(scalar(connection, "PRAGMA wal_autocheckpoint") == 1000);
  REQUIRE(scalar(connection, "PRAGMA journal_size_limit") == 16 * 1024 * 1024);
  REQUIRE(scalar(connection, "PRAGMA trusted_schema") == 0);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_LENGTH, -1) ==
          4 * 1024 * 1024);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_SQL_LENGTH,
                        -1) == 1024 * 1024);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_COLUMN, -1) ==
          256);
  REQUIRE(sqlite3_limit(connection.native_handle(),
                        SQLITE_LIMIT_VARIABLE_NUMBER, -1) == 1024);
  REQUIRE(sqlite3_limit(connection.native_handle(),
                        SQLITE_LIMIT_COMPOUND_SELECT, -1) == 64);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_EXPR_DEPTH,
                        -1) == 100);
  REQUIRE(sqlite3_limit(connection.native_handle(),
                        SQLITE_LIMIT_LIKE_PATTERN_LENGTH, -1) == 4096);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_TRIGGER_DEPTH,
                        -1) == 32);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_ATTACHED,
                        -1) == 0);
  REQUIRE(sqlite3_limit(connection.native_handle(), SQLITE_LIMIT_WORKER_THREADS,
                        -1) == 0);
  REQUIRE(sanguinius::persistence::sqlite_has_wal_reset_fix(3'053'004));
  REQUIRE(sanguinius::persistence::sqlite_has_wal_reset_fix(3'050'007));
  REQUIRE(sanguinius::persistence::sqlite_has_wal_reset_fix(3'044'006));
  REQUIRE_FALSE(sanguinius::persistence::sqlite_has_wal_reset_fix(3'051'002));

  for (const int option :
       {SQLITE_DBCONFIG_DEFENSIVE, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,
        SQLITE_DBCONFIG_TRUSTED_SCHEMA, SQLITE_DBCONFIG_DQS_DML,
        SQLITE_DBCONFIG_DQS_DDL}) {
    int actual = -1;
    REQUIRE(sqlite3_db_config(connection.native_handle(), option, -1,
                              &actual) == SQLITE_OK);
    REQUIRE(actual == (option == SQLITE_DBCONFIG_DEFENSIVE ? 1 : 0));
  }
}

TEST_CASE("transactions and savepoints commit or roll back deterministically",
          "[sqlite][transaction]") {
  sanguinius::test::TemporaryDatabase temporary;
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto &connection = database.connection();
  connection.execute("CREATE TABLE sample (value INTEGER NOT NULL) STRICT");

  {
    Transaction transaction{connection, TransactionMode::immediate};
    connection.execute("INSERT INTO sample VALUES (1)");
    transaction.commit();
  }
  {
    Transaction transaction{connection};
    connection.execute("INSERT INTO sample VALUES (2)");
  }
  {
    Transaction transaction{connection};
    sanguinius::persistence::Savepoint savepoint{connection};
    connection.execute("INSERT INTO sample VALUES (3)");
    savepoint.rollback();
    connection.execute("INSERT INTO sample VALUES (4)");
    transaction.commit();
  }
  {
    Transaction transaction{connection};
    sanguinius::persistence::Savepoint savepoint{connection};
    connection.execute("INSERT INTO sample VALUES (6)");
    savepoint.release();
    transaction.commit();
  }
  try {
    Transaction transaction{connection};
    connection.execute("INSERT INTO sample VALUES (7)");
    throw std::runtime_error{"exercise exception rollback"};
  } catch (const std::runtime_error &) {
  }
  REQUIRE(scalar(connection, "SELECT count(*) FROM sample") == 3);
  REQUIRE(scalar(connection, "SELECT sum(value) FROM sample") == 11);
  REQUIRE_THROWS_AS(connection.prepare("SELECT 1; SELECT 2"), DatabaseError);

  Transaction transaction{connection, TransactionMode::immediate};
  {
    auto unfinished =
        connection.prepare("INSERT INTO sample VALUES (5) RETURNING value");
    REQUIRE(unfinished.step());
    try {
      transaction.commit();
      FAIL("commit unexpectedly succeeded with an unfinished write");
    } catch (const DatabaseError &error) {
      REQUIRE(error.category() == DatabaseErrorCategory::busy);
      REQUIRE(transaction.active());
    }
  }
  transaction.commit();
  REQUIRE(scalar(connection, "SELECT sum(value) FROM sample") == 16);
}

TEST_CASE("SQLite handles are move-only and statements reset typed bindings",
          "[sqlite][raii]") {
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<SqliteConnection>);
  STATIC_REQUIRE(std::is_move_constructible_v<SqliteConnection>);
  STATIC_REQUIRE_FALSE(
      std::is_copy_constructible_v<sanguinius::persistence::SqliteStatement>);

  sanguinius::test::TemporaryDatabase temporary;
  REQUIRE(std::filesystem::create_directories(temporary.root()));
  auto original =
      SqliteConnection::open(temporary.path(), SqliteOpenMode::create);
  auto moved = std::move(original);
  moved.execute("CREATE TABLE sample (number INTEGER, text TEXT) STRICT");
  auto insert = moved.prepare("INSERT INTO sample VALUES (?, ?)");
  insert.bind(1, 7);
  insert.bind(2, std::string_view{"first"});
  insert.execute();
  insert.reset();
  insert.bind_null(1);
  insert.bind(2, std::string_view{"second"});
  insert.execute();
  insert.reset();
  insert.bind_null(1);
  insert.bind(2, std::string_view{});
  insert.execute();

  auto rows = moved.prepare("SELECT number, text FROM sample ORDER BY rowid");
  REQUIRE(rows.step());
  REQUIRE(rows.column_int64(0) == 7);
  REQUIRE(rows.column_text(1) == "first");
  REQUIRE(rows.step());
  REQUIRE(rows.column_is_null(0));
  REQUIRE(rows.column_text(1) == "second");
  REQUIRE(rows.step());
  REQUIRE(rows.column_is_null(0));
  REQUIRE_FALSE(rows.column_is_null(1));
  REQUIRE(rows.column_text(1).empty());
  REQUIRE_FALSE(rows.step());
}

TEST_CASE("WAL readers keep snapshots and competing writers become busy",
          "[sqlite][concurrency]") {
  sanguinius::test::TemporaryDatabase temporary;
  {
    auto database = Database::open_migration(temporary.path(), 25ms);
    database.connection().execute(
        "CREATE TABLE sample (value INTEGER NOT NULL) STRICT");
    database.connection().execute("INSERT INTO sample VALUES (1)");
  }

  auto reader =
      SqliteConnection::open(temporary.path(), SqliteOpenMode::read_write);
  auto writer =
      SqliteConnection::open(temporary.path(), SqliteOpenMode::read_write);
  sanguinius::persistence::configure_connection(reader, 25ms, false, true);
  sanguinius::persistence::configure_connection(writer, 25ms, false, true);

  Transaction read_transaction{reader};
  REQUIRE(scalar(reader, "SELECT count(*) FROM sample") == 1);
  writer.execute("INSERT INTO sample VALUES (2)");
  REQUIRE(scalar(reader, "SELECT count(*) FROM sample") == 1);
  read_transaction.commit();
  REQUIRE(scalar(reader, "SELECT count(*) FROM sample") == 2);

  Transaction held_writer{reader, TransactionMode::immediate};
  try {
    Transaction competing{writer, TransactionMode::immediate};
    FAIL("competing writer unexpectedly acquired the write transaction");
  } catch (const DatabaseError &error) {
    REQUIRE(error.category() == DatabaseErrorCategory::busy);
  }
  held_writer.rollback();
}

TEST_CASE("runtime shared lock excludes migration but permits inspection",
          "[sqlite][lock]") {
  sanguinius::test::TemporaryDatabase temporary;
  {
    auto migration = Database::open_migration(temporary.path(), 25ms);
  }
  auto runtime = Database::open_runtime(temporary.path(), 25ms);
  REQUIRE_THROWS_AS(Database::open_migration(temporary.path(), 25ms),
                    DatabaseError);
  REQUIRE_NOTHROW(Database::open_inspection(temporary.path(), 25ms));
}
