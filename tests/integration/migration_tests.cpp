#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

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

int deny_fts5_creation(void *, const int action, const char *,
                       const char *module_name, const char *, const char *) {
  if (action == SQLITE_CREATE_VTABLE && module_name != nullptr &&
      std::string_view{module_name} == "fts5")
    return SQLITE_DENY;
  return SQLITE_OK;
}

} // namespace

TEST_CASE("production migration moves an empty database to version seven",
          "[migration]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{123}}};
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto migrator = production_migrator(clock);

  const auto before = migrator.inspect(database.connection());
  REQUIRE(before.state == SchemaState::uninitialized);
  REQUIRE(before.current_version == 0);
  REQUIRE(before.target_version == 7);

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 7);
  REQUIRE(count(database.connection(), "schema_migrations") == 7);
  REQUIRE(count(database.connection(), "application_instance") == 0);
  REQUIRE(count(database.connection(), "guild_config") == 0);
  REQUIRE(count(database.connection(), "pending_notice") == 0);
  REQUIRE(count(database.connection(), "interaction_token") == 0);
  REQUIRE(count(database.connection(), "notice_reveal_attempt") == 0);
  REQUIRE(count(database.connection(), "event_journal") == 0);
  REQUIRE(count(database.connection(), "scheduled_job") == 0);
  REQUIRE(count(database.connection(), "outbox_message") == 0);
  REQUIRE(count(database.connection(), "chronicle_entry") == 0);
  REQUIRE(count(database.connection(), "memory") == 0);
  REQUIRE(count(database.connection(), "relationship_event") == 0);
  REQUIRE(count(database.connection(), "relationship_state") == 0);
  REQUIRE(count(database.connection(), "ai_prompt_attempt") == 0);
  REQUIRE(count(database.connection(), "ai_prompt_attempt_memory") == 0);
  REQUIRE(count(database.connection(), "ai_prompt_attempt_chronicle_context") ==
          0);
  REQUIRE(count(database.connection(), "chronicle_session") == 0);
  REQUIRE(count(database.connection(), "chronicle_summary_draft") == 0);
  REQUIRE(count(database.connection(), "chronicle_title_grant") == 0);
  REQUIRE(count(database.connection(), "chronicle_anniversary_delivery") == 0);
  REQUIRE(count(database.connection(), "appearance_policy_snapshot") == 0);
  REQUIRE(count(database.connection(), "appearance_mode_state") == 1);
  REQUIRE(count(database.connection(), "appearance_channel_state") == 0);
  REQUIRE(count(database.connection(), "appearance_message_seen") == 0);
  REQUIRE(count(database.connection(), "appearance_message_activity") == 0);
  REQUIRE(count(database.connection(), "appearance_event_observation") == 0);
  REQUIRE(count(database.connection(), "appearance_candidate") == 0);
  REQUIRE(count(database.connection(), "appearance_decision") == 0);
  REQUIRE(count(database.connection(), "appearance_preview") == 0);
  auto relationship_indexes = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='index' AND name IN ("
      "'relationship_event_subject_time','relationship_event_source',"
      "'relationship_event_reason_time','ai_prompt_attempt_prepared',"
      "'ai_prompt_attempt_memory_cooldown','memory_prompt_candidates')");
  REQUIRE(relationship_indexes.step());
  REQUIRE(relationship_indexes.column_int64(0) == 6);
  auto relationship_triggers = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND name IN ("
      "'relationship_event_no_update','relationship_event_no_delete',"
      "'ai_prompt_attempt_transition_only','ai_prompt_attempt_no_delete',"
      "'ai_prompt_attempt_memory_no_update',"
      "'ai_prompt_attempt_memory_no_delete')");
  REQUIRE(relationship_triggers.step());
  REQUIRE(relationship_triggers.column_int64(0) == 6);
  auto continuity_triggers = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND name IN ("
      "'ai_prompt_attempt_chronicle_context_no_update',"
      "'ai_prompt_attempt_chronicle_context_no_delete',"
      "'chronicle_session_context_purge_after_opt_out')");
  REQUIRE(continuity_triggers.step());
  REQUIRE(continuity_triggers.column_int64(0) == 3);
  auto chronicle_outbox_index = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='index' AND "
      "name='outbox_chronicle_aggregate_sequence'");
  REQUIRE(chronicle_outbox_index.step());
  REQUIRE(chronicle_outbox_index.column_int64(0) == 1);

  auto history = database.connection().prepare(
      "SELECT name, checksum, applied_at_ms FROM schema_migrations "
      "ORDER BY version");
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "core_foundation");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "discord_interactions");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "durable_work");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "chronicle");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "relationships");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "chronicle_sessions");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "appearance_dry_run");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE_FALSE(history.step());

  clock.set(std::chrono::sys_seconds{std::chrono::seconds{456}});
  const auto repeated = migrator.apply(database.connection());
  REQUIRE(repeated.state == SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 7);
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
      "', name = 'renamed' WHERE version = 1");
  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);

  database.connection().execute("DELETE FROM schema_migrations");
  database.connection().execute(
      "INSERT INTO schema_migrations VALUES "
      "(6, 'future', "
      "'0000000000000000000000000000000000000000000000000000000000000000', "
      "0, 'future')");
  REQUIRE(migrator.inspect(database.connection()).state ==
          SchemaState::incompatible);
}

TEST_CASE("production migration upgrades version one atomically",
          "[migration][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_one{std::span<const Migration>{production.data(), 1},
                             {"test-version", "test-revision"},
                             clock};
  REQUIRE(version_one.apply(database.connection()).current_version == 1);

  auto migrator = production_migrator(clock);
  const auto before = migrator.inspect(database.connection());
  REQUIRE(before.state == SchemaState::pending);
  REQUIRE(before.current_version == 1);
  REQUIRE(before.pending_count == 6);

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 7);
  REQUIRE(count(database.connection(), "pending_notice") == 0);
  REQUIRE(count(database.connection(), "interaction_token") == 0);
  REQUIRE(count(database.connection(), "notice_reveal_attempt") == 0);
}

TEST_CASE("production migration upgrades version two through Chronicle",
          "[migration][durable]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_two{std::span<const Migration>{production.data(), 2},
                             {"test-version", "test-revision"},
                             clock};
  REQUIRE(version_two.apply(database.connection()).current_version == 2);

  auto migrator = production_migrator(clock);
  const auto before = migrator.inspect(database.connection());
  REQUIRE(before.state == SchemaState::pending);
  REQUIRE(before.current_version == 2);
  REQUIRE(before.pending_count == 5);
  REQUIRE(migrator.apply(database.connection()).current_version == 7);
  REQUIRE(count(database.connection(), "event_journal") == 0);
  REQUIRE(count(database.connection(), "scheduled_job") == 0);
  REQUIRE(count(database.connection(), "outbox_message") == 0);
}

TEST_CASE("version four imports existing Chronicle consent once",
          "[migration][chronicle][privacy]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_three{std::span<const Migration>{production.data(), 3},
                               {"test-version", "test-revision"},
                               clock};
  REQUIRE(version_three.apply(database.connection()).current_version == 3);
  database.connection().execute("INSERT INTO discord_user VALUES "
                                "('42','Existing','existing',0,1,1,1,1)");
  database.connection().execute(
      "INSERT INTO user_preference (user_id,updated_at_ms) VALUES ('42',1)");

  auto migrator = production_migrator(clock);
  REQUIRE(migrator.inspect(database.connection()).pending_count == 4);
  REQUIRE(migrator.apply(database.connection()).current_version == 7);
  auto imported = database.connection().prepare(
      "SELECT chronicle_opt_in,memory_callback_opt_in FROM user_preference "
      "WHERE user_id='42'");
  REQUIRE(imported.step());
  REQUIRE(imported.column_int64(0) == 1);
  REQUIRE(imported.column_int64(1) == 0);

  database.connection().execute("INSERT INTO discord_user VALUES "
                                "('43','Future','future',0,2,2,2,2)");
  database.connection().execute(
      "INSERT INTO user_preference (user_id,updated_at_ms) VALUES ('43',2)");
  auto future = database.connection().prepare(
      "SELECT chronicle_opt_in,memory_callback_opt_in FROM user_preference "
      "WHERE user_id='43'");
  REQUIRE(future.step());
  REQUIRE(future.column_int64(0) == 0);
  REQUIRE(future.column_int64(1) == 0);
}

TEST_CASE(
    "production migration upgrades version four through sessions atomically",
    "[migration][relationship][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_four{std::span<const Migration>{production.data(), 4},
                              {"test-version", "test-revision"},
                              clock};
  REQUIRE(version_four.apply(database.connection()).current_version == 4);
  database.connection().execute("INSERT INTO discord_user VALUES "
                                "('42','Existing','existing',0,1,1,1,1)");
  database.connection().execute(
      "INSERT INTO user_preference (user_id,updated_at_ms) VALUES ('42',1)");

  auto migrator = production_migrator(clock);
  const auto pending = migrator.inspect(database.connection());
  REQUIRE(pending.current_version == 4);
  REQUIRE(pending.pending_count == 3);
  REQUIRE(migrator.apply(database.connection()).current_version == 7);
  REQUIRE(count(database.connection(), "relationship_event") == 0);
  REQUIRE(count(database.connection(), "relationship_state") == 0);
  REQUIRE(count(database.connection(), "ai_prompt_attempt") == 0);
  REQUIRE(count(database.connection(), "discord_user") == 1);

  sanguinius::test::TemporaryDatabase rollback_temporary;
  auto rollback_database =
      Database::open_migration(rollback_temporary.path(), 25ms);
  REQUIRE(version_four.apply(rollback_database.connection()).current_version ==
          4);
  constexpr std::string_view checksum{
      "0000000000000000000000000000000000000000000000000000000000000000"};
  const std::array<Migration, 5> broken{
      production[0], production[1], production[2], production[3],
      Migration{5, "relationships", checksum,
                "CREATE TABLE relationship_partial (id INTEGER) STRICT; "
                "THIS IS NOT SQL;"}};
  const Migrator broken_migrator{
      broken, {"test-version", "test-revision"}, clock};
  REQUIRE_THROWS_AS(broken_migrator.apply(rollback_database.connection()),
                    DatabaseError);
  auto partial = rollback_database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name='relationship_partial'");
  REQUIRE(partial.step());
  REQUIRE(partial.column_int64(0) == 0);
  REQUIRE(version_four.inspect(rollback_database.connection()).state ==
          SchemaState::current);
}

TEST_CASE("version six preserves populated Chronicle children and projections",
          "[migration][chronicle][relationship][fts]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_five{std::span<const Migration>{production.data(), 5},
                              {"test-version", "test-revision"},
                              clock};
  REQUIRE(version_five.apply(database.connection()).current_version == 5);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES "
      "('30','Owner','owner',0,1,1,1,1),"
      "('31','Member','member',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES ('10','20','30',1,1);"
      "INSERT INTO user_preference(user_id,chronicle_opt_in,updated_at_ms) "
      "VALUES ('30',1,1),('31',1,1);"
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,"
      "source_text_truncated,source_attachment_count,revision) VALUES "
      "('00000000-0000-4000-8000-000000000701','deed','Preserved title',"
      "'Preserved Chronicle body','shared','canon',10,10,'31',10,10,'30',"
      "'10','20','40','31','preserved source',0,1,1);"
      "INSERT INTO chronicle_participant VALUES "
      "('00000000-0000-4000-8000-000000000701','31','subject');"
      "INSERT INTO chronicle_tag VALUES "
      "('00000000-0000-4000-8000-000000000701','preserved');"
      "INSERT INTO chronicle_attachment VALUES "
      "('00000000-0000-4000-8000-000000000701',0,'41','proof.png',"
      "'image/png',123,10,20,0,0);"
      "INSERT INTO chronicle_approval(approval_id,entry_id,reviewer_user_id,"
      "approval_role,state,entry_revision,requested_at_ms) VALUES "
      "('00000000-0000-4000-8000-000000000702',"
      "'00000000-0000-4000-8000-000000000701','30','owner','pending',1,10);"
      "INSERT INTO relationship_state(subject_user_id,familiarity,esteem,mirth,"
      "reliability,wariness,interaction_count,last_interaction_at_ms,"
      "projection_version,updated_at_ms) VALUES ('31',4,3,2,1,0,4,10,1,10);");

  const auto applied = production_migrator(clock).apply(database.connection());
  REQUIRE(applied.current_version == 7);
  REQUIRE(count(database.connection(), "chronicle_entry") == 1);
  REQUIRE(count(database.connection(), "chronicle_participant") == 1);
  REQUIRE(count(database.connection(), "chronicle_tag") == 1);
  REQUIRE(count(database.connection(), "chronicle_attachment") == 1);
  REQUIRE(count(database.connection(), "chronicle_approval") == 1);
  REQUIRE(count(database.connection(), "relationship_state") == 1);
  auto rebuilt = database.connection().prepare(
      "SELECT source_kind,source_message_id FROM chronicle_entry");
  REQUIRE(rebuilt.step());
  REQUIRE(rebuilt.column_text(0) == "discord_message");
  REQUIRE(rebuilt.column_text(1) == "40");
  auto preference = database.connection().prepare(
      "SELECT anniversary_reminders_enabled FROM user_preference WHERE "
      "user_id='31'");
  REQUIRE(preference.step());
  REQUIRE(preference.column_int64(0) == 1);
  auto indexed = database.connection().prepare(
      "SELECT count(*) FROM chronicle_entry_fts WHERE "
      "chronicle_entry_fts MATCH '\"Preserved\"'");
  REQUIRE(indexed.step());
  REQUIRE(indexed.column_int64(0) == 1);
}

TEST_CASE("version six rolls back when FTS5 preflight is unavailable",
          "[migration][chronicle][fts][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_five{std::span<const Migration>{production.data(), 5},
                              {"test-version", "test-revision"},
                              clock};
  REQUIRE(version_five.apply(database.connection()).current_version == 5);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(),
                                 deny_fts5_creation, nullptr) == SQLITE_OK);

  REQUIRE_THROWS_AS(production_migrator(clock).apply(database.connection()),
                    DatabaseError);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(), nullptr,
                                 nullptr) == SQLITE_OK);
  REQUIRE(version_five.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 5);
  auto v6_tables = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name IN ("
      "'chronicle_session','chronicle_entry_fts',"
      "'ai_prompt_attempt_chronicle_context')");
  REQUIRE(v6_tables.step());
  REQUIRE(v6_tables.column_int64(0) == 0);
  auto anniversary_column = database.connection().prepare(
      "SELECT count(*) FROM pragma_table_info('user_preference') WHERE "
      "name='anniversary_reminders_enabled'");
  REQUIRE(anniversary_column.step());
  REQUIRE(anniversary_column.column_int64(0) == 0);
}

TEST_CASE("version seven upgrades an accepted version-six database atomically",
          "[migration][appearance][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_six{std::span<const Migration>{production.data(), 6},
                             {"test-version", "test-revision"},
                             clock};
  REQUIRE(version_six.apply(database.connection()).current_version == 6);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO "
      "guild_config(guild_id,primary_channel_id,owner_user_id,created_at_ms,"
      "updated_at_ms) "
      "VALUES('10','20','30',1,1);"
      "INSERT INTO user_preference(user_id,updated_at_ms) VALUES('30',1)");
  const auto before = production_migrator(clock).inspect(database.connection());
  REQUIRE(before.current_version == 6);
  REQUIRE(before.pending_count == 1);
  const auto applied = production_migrator(clock).apply(database.connection());
  REQUIRE(applied.current_version == 7);
  REQUIRE(count(database.connection(), "discord_user") == 1);
  REQUIRE(count(database.connection(), "appearance_candidate") == 0);
  auto guard = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND "
      "name='appearance_dry_run_outbox_guard'");
  REQUIRE(guard.step());
  REQUIRE(guard.column_int64(0) == 1);
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

  const auto version_zero = migrator.version_zero_status();
  REQUIRE(version_zero.state == SchemaState::uninitialized);
  REQUIRE(version_zero.current_version == 0);
  REQUIRE(version_zero.target_version == 2);
  REQUIRE(version_zero.pending_count == 2);

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
