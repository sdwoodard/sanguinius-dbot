#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"
#include "sanguinius/wagers.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

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

[[nodiscard]] std::string migration_uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "20000000-0000-4000-8000-" + suffix;
}

int deny_fts5_creation(void *, const int action, const char *,
                       const char *module_name, const char *, const char *) {
  if (action == SQLITE_CREATE_VTABLE && module_name != nullptr &&
      std::string_view{module_name} == "fts5")
    return SQLITE_DENY;
  return SQLITE_OK;
}

int deny_v9_posting_drop(void *, const int action, const char *object_name,
                         const char *, const char *, const char *) {
  if (action == SQLITE_DROP_TABLE && object_name != nullptr &&
      std::string_view{object_name} == "tarot_posting_v9")
    return SQLITE_DENY;
  return SQLITE_OK;
}

int deny_v11_chronicle_swap(void *, const int action, const char *object_name,
                            const char *, const char *, const char *) {
  if (action == SQLITE_DROP_TABLE && object_name != nullptr &&
      std::string_view{object_name} == "chronicle_entry")
    return SQLITE_DENY;
  return SQLITE_OK;
}

int deny_v12_transition_table(void *, const int action,
                              const char *object_name, const char *,
                              const char *, const char *) {
  if (action == SQLITE_CREATE_TABLE && object_name != nullptr &&
      std::string_view{object_name} == "voice_session_transition")
    return SQLITE_DENY;
  return SQLITE_OK;
}

int deny_v14_narration_table(void *, const int action,
                             const char *object_name, const char *,
                             const char *, const char *) {
  if (action == SQLITE_CREATE_TABLE && object_name != nullptr &&
      std::string_view{object_name} == "voice_narration_intent")
    return SQLITE_DENY;
  return SQLITE_OK;
}

} // namespace

TEST_CASE("production migration moves an empty database to version fifteen",
          "[migration]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{123}}};
  auto database = Database::open_migration(temporary.path(), 25ms);
  auto migrator = production_migrator(clock);

  const auto before = migrator.inspect(database.connection());
  REQUIRE(before.state == SchemaState::uninitialized);
  REQUIRE(before.current_version == 0);
  REQUIRE(before.target_version == 15);

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 15);
  REQUIRE(count(database.connection(), "schema_migrations") == 15);
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
  REQUIRE(count(database.connection(), "appearance_delivery_participant") == 0);
  REQUIRE(count(database.connection(), "tarot_account") == 0);
  REQUIRE(count(database.connection(), "tarot_transaction") == 0);
  REQUIRE(count(database.connection(), "tarot_posting") == 0);
  REQUIRE(count(database.connection(), "tarot_draw") == 0);
  REQUIRE(count(database.connection(), "tarot_recovery_claim") == 0);
  REQUIRE(count(database.connection(), "tarot_history_cursor") == 0);
  REQUIRE(count(database.connection(), "tarot_wager") == 0);
  REQUIRE(count(database.connection(), "tarot_wager_transfer") == 0);
  REQUIRE(count(database.connection(), "tarot_wager_evidence") == 0);
  REQUIRE(count(database.connection(), "voice_session") == 0);
  REQUIRE(count(database.connection(), "voice_session_transition") == 0);
  REQUIRE(count(database.connection(), "voice_interaction_receipt") == 0);
  REQUIRE(count(database.connection(), "voice_public_outbox_dependency") == 0);
  REQUIRE(count(database.connection(), "speech_item") == 0);
  REQUIRE(count(database.connection(), "speech_item_transition") == 0);
  REQUIRE(count(database.connection(), "tts_usage_attempt") == 0);
  REQUIRE(count(database.connection(), "tts_cache_entry") == 0);
  REQUIRE(count(database.connection(), "vox_voice_configuration") == 0);
  REQUIRE(count(database.connection(), "voice_narration_cursor") == 1);
  REQUIRE(count(database.connection(), "voice_narration_intent") == 0);
  REQUIRE(count(database.connection(), "voice_narration_transition") == 0);
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
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "appearance_live");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "tarot_ledger");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "peer_wagers");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "tarot_house_integration");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "vox_foundation");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "vox_tts");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "vox_narration");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE(history.step());
  REQUIRE(history.column_text(0) == "voice_input");
  REQUIRE(history.column_text(1).size() == 64);
  REQUIRE(history.column_int64(2) == 123'000);
  REQUIRE_FALSE(history.step());

  clock.set(std::chrono::sys_seconds{std::chrono::seconds{456}});
  const auto repeated = migrator.apply(database.connection());
  REQUIRE(repeated.state == SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 15);
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
  REQUIRE(before.pending_count == 14);

  const auto applied = migrator.apply(database.connection());
  REQUIRE(applied.state == SchemaState::current);
  REQUIRE(applied.current_version == 15);
  REQUIRE(count(database.connection(), "pending_notice") == 0);
  REQUIRE(count(database.connection(), "interaction_token") == 0);
  REQUIRE(count(database.connection(), "notice_reveal_attempt") == 0);
}

TEST_CASE("voice input migration upgrades every accepted prior schema to version fifteen",
          "[migration][voice-input][upgrade-matrix]") {
  const auto production = sanguinius::persistence::production_migrations();
  for (std::size_t prior_version = 1; prior_version < production.size();
       ++prior_version) {
    DYNAMIC_SECTION("schema version " << prior_version) {
      sanguinius::test::TemporaryDatabase temporary;
      sanguinius::test::FakeClock clock;
      auto database = Database::open_migration(temporary.path(), 25ms);
      const Migrator prior{
          std::span<const Migration>{production.data(), prior_version},
          {"test-version", "test-revision"}, clock};
      REQUIRE(prior.apply(database.connection()).current_version ==
              static_cast<std::int64_t>(prior_version));

      const auto upgraded =
          production_migrator(clock).apply(database.connection());
      REQUIRE(upgraded.state == SchemaState::current);
      REQUIRE(upgraded.current_version == 15);
      auto foreign_keys =
          database.connection().prepare("PRAGMA foreign_key_check");
      REQUIRE_FALSE(foreign_keys.step());
      REQUIRE(count(database.connection(), "voice_session") == 0);
    }
  }
}

TEST_CASE("voice input migration preserves Tarot appearance source privacy",
          "[migration][voice-input][tarot][appearance][privacy]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_fourteen{
      std::span<const Migration>{production.data(), 14},
      {"test-version", "test-revision"},
      clock};
  REQUIRE(version_fourteen.apply(database.connection()).current_version == 14);

  const auto original_view_sql = [&] {
    auto original_view = database.connection().prepare(
        "SELECT sql FROM sqlite_schema WHERE type='view' AND "
        "name='appearance_candidate_source_user'");
    REQUIRE(original_view.step());
    return original_view.column_text(0);
  }();

  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO user_preference(user_id,appearance_callback_opt_in,"
      "updated_at_ms) VALUES('30',1,1);"
      "INSERT INTO appearance_policy_snapshot VALUES("
      "'m17-privacy',1,'{}',1);"
      "INSERT INTO tarot_catalog_snapshot VALUES("
      "'m17-deck','deck','{}','checksum-m17',1);"
      "INSERT INTO tarot_card_definition VALUES("
      "'m17-deck',0,'the-fool','The Fool','A beginning worth remembering.',"
      "'beginnings','Keep the interpretation grounded.','[\"Begin.\"]');"
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000017001','tarot.draw_created.v1',"
      "'tarot_card_draw','00000000-0000-4000-8000-000000017002','30','10',"
      "'20',10,10,'m17-migration','m17:event','{}');"
      "INSERT INTO tarot_card_draw VALUES("
      "'00000000-0000-4000-8000-000000017002','30','10','20','public',"
      "'m17-deck',0,0,10,20,0,"
      "'00000000-0000-4000-8000-000000017001');"
      "INSERT INTO appearance_candidate(candidate_id,candidate_type,guild_id,"
      "channel_id,policy_version,deduplication_key,context_json,"
      "owner_simulation,created_at_ms,expires_at_ms,context_expires_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000017003','tarot_event','10','20',"
      "'m17-privacy','m17-tarot-source','{}',0,10,100,100);"
      "INSERT INTO appearance_event_observation(source_event_id,event_type,"
      "aggregate_type,aggregate_id,guild_id,channel_id,actor_user_id,"
      "occurred_at_ms,recorded_at_ms,candidate_id) VALUES("
      "'00000000-0000-4000-8000-000000017001','tarot.draw_created.v1',"
      "'tarot_card_draw','00000000-0000-4000-8000-000000017002','10','20',"
      "'30',10,10,'00000000-0000-4000-8000-000000017003');"
      "INSERT INTO appearance_candidate_source VALUES("
      "'00000000-0000-4000-8000-000000017003','event',"
      "'00000000-0000-4000-8000-000000017001',0);");

  REQUIRE(
      production_migrator(clock).apply(database.connection()).current_version ==
      15);
  auto rebuilt_view = database.connection().prepare(
      "SELECT sql FROM sqlite_schema WHERE type='view' AND "
      "name='appearance_candidate_source_user'");
  REQUIRE(rebuilt_view.step());
  REQUIRE(rebuilt_view.column_text(0) == original_view_sql);

  auto mapped_user = database.connection().prepare(
      "SELECT user_id FROM appearance_candidate_source_user WHERE "
      "candidate_id='00000000-0000-4000-8000-000000017003'");
  REQUIRE(mapped_user.step());
  REQUIRE(mapped_user.column_text(0) == "30");
  REQUIRE_FALSE(mapped_user.step());
  auto foreign_keys = database.connection().prepare("PRAGMA foreign_key_check");
  REQUIRE_FALSE(foreign_keys.step());
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
  REQUIRE(before.pending_count == 13);
  REQUIRE(migrator.apply(database.connection()).current_version == 15);
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
  REQUIRE(migrator.inspect(database.connection()).pending_count == 12);
  REQUIRE(migrator.apply(database.connection()).current_version == 15);
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
  REQUIRE(pending.pending_count == 11);
  REQUIRE(migrator.apply(database.connection()).current_version == 15);
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
  REQUIRE(applied.current_version == 15);
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

TEST_CASE("version nine upgrades an accepted version-six database atomically",
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
  REQUIRE(before.pending_count == 9);
  const auto applied = production_migrator(clock).apply(database.connection());
  REQUIRE(applied.current_version == 15);
  REQUIRE(count(database.connection(), "discord_user") == 1);
  REQUIRE(count(database.connection(), "appearance_candidate") == 0);
  auto guard = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND "
      "name='appearance_live_outbox_guard'");
  REQUIRE(guard.step());
  REQUIRE(guard.column_int64(0) == 1);
}

TEST_CASE("version eight preserves accepted dry-run audit rows",
          "[migration][appearance][preservation]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_seven{std::span<const Migration>{production.data(), 7},
                               {"test-version", "test-revision"},
                               clock};
  REQUIRE(version_seven.apply(database.connection()).current_version == 7);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO user_preference(user_id,appearance_callback_opt_in,"
      "updated_at_ms) VALUES('30',1,1);"
      "UPDATE appearance_mode_state SET mode='dry_run',updated_at_ms=10 "
      "WHERE singleton=1;"
      "INSERT INTO appearance_policy_snapshot VALUES('accepted-v7',1,'{}',1);"
      "INSERT INTO appearance_candidate(candidate_id,candidate_type,guild_id,"
      "channel_id,policy_version,deduplication_key,context_json,"
      "owner_simulation,created_at_ms,expires_at_ms,context_expires_at_ms,"
      "evaluation_started_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000008001','simulation','10','20',"
      "'accepted-v7','accepted-v7-candidate','{}',1,20,200,200,20);"
      "INSERT INTO appearance_candidate_actor VALUES("
      "'00000000-0000-4000-8000-000000008001','30');"
      "INSERT INTO appearance_decision(decision_id,candidate_id,policy_version,"
      "application_instance_id,revision,state,action,reason,gate_json,"
      "score_json,score,human_message_count,model_status,"
      "serious_categories_json,created_at_ms,finalized_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000008002',"
      "'00000000-0000-4000-8000-000000008001','accepted-v7','accepted',2,"
      "'final','hypothetical','hypothetical','[]','[]',80,8,"
      "'model_accepted','[]',30,30);"
      "INSERT INTO appearance_preview VALUES("
      "'00000000-0000-4000-8000-000000008002','Preserved preview.',"
      "'warm',30,300);");

  REQUIRE(
      production_migrator(clock).apply(database.connection()).current_version ==
      15);
  REQUIRE(count(database.connection(), "appearance_candidate") == 1);
  REQUIRE(count(database.connection(), "appearance_decision") == 1);
  REQUIRE(count(database.connection(), "appearance_preview") == 1);
  auto preserved = database.connection().prepare(
      "SELECT d.action,d.reason,p.preview_text,c.mode_activated_at_ms,"
      "m.activated_at_ms FROM appearance_decision d JOIN appearance_preview p "
      "ON p.decision_id=d.decision_id JOIN appearance_candidate c ON "
      "c.candidate_id=d.candidate_id JOIN appearance_mode_state m ON "
      "m.singleton=1");
  REQUIRE(preserved.step());
  REQUIRE(preserved.column_text(0) == "hypothetical");
  REQUIRE(preserved.column_text(1) == "hypothetical");
  REQUIRE(preserved.column_text(2) == "Preserved preview.");
  REQUIRE(preserved.column_int64(3) == 10);
  REQUIRE(preserved.column_int64(4) == 10);
  REQUIRE(count(database.connection(), "appearance_budget_reservation") == 0);
}

TEST_CASE(
    "version nine adds an empty ledger to an accepted version-eight database",
    "[migration][tarot][preservation]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_eight{std::span<const Migration>{production.data(), 8},
                               {"test-version", "test-revision"},
                               clock};
  REQUIRE(version_eight.apply(database.connection()).current_version == 8);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO user_preference(user_id,public_tarot_results_opt_in,"
      "updated_at_ms) VALUES('30',0,1)");
  const auto before = production_migrator(clock).inspect(database.connection());
  REQUIRE(before.current_version == 8);
  REQUIRE(before.pending_count == 7);
  REQUIRE(
      production_migrator(clock).apply(database.connection()).current_version ==
      15);
  REQUIRE(count(database.connection(), "tarot_account") == 0);
  REQUIRE(count(database.connection(), "tarot_transaction") == 0);
  auto preference =
      database.connection().prepare("SELECT public_tarot_results_opt_in FROM "
                                    "user_preference WHERE user_id='30'");
  REQUIRE(preference.step());
  REQUIRE(preference.column_int64(0) == 0);
}

TEST_CASE(
    "version eleven preserves populated version-nine ledger order and rows",
    "[migration][wager][ledger][preservation]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_nine{std::span<const Migration>{production.data(), 9},
                              {"test-version", "test-revision"},
                              clock};
  REQUIRE(version_nine.apply(database.connection()).current_version == 9);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000009001','tarot.account_provisioned.v1',"
      "'tarot_account','00000000-0000-4000-8000-000000009003','30','10','20',"
      "10,10,'migration-test','migration:event','{}');"
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000009008','tarot.grace_started.v1',"
      "'tarot_recovery_claim','00000000-0000-4000-8000-000000009007','30',"
      "'10','20',11,11,'migration-test','migration:grace','{}');"
      "INSERT INTO "
      "tarot_account(account_id,account_kind,user_id,created_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000009002','MINT',NULL,10);"
      "INSERT INTO "
      "tarot_account(account_id,account_kind,user_id,created_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000009003','HUMAN','30',10);"
      "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
      "expected_posting_count,event_id,idempotency_key,actor_user_id,is_test,"
      "created_at_ms) VALUES('00000000-0000-4000-8000-000000009004',"
      "'STARTING_GRANT','prepared',2,"
      "'00000000-0000-4000-8000-000000009001','migration:grant','30',0,10);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000009005',"
      "'00000000-0000-4000-8000-000000009004',"
      "'00000000-0000-4000-8000-000000009002',-100,10);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000009006',"
      "'00000000-0000-4000-8000-000000009004',"
      "'00000000-0000-4000-8000-000000009003',100,10);"
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=10 "
      "WHERE transaction_id='00000000-0000-4000-8000-000000009004';"
      "INSERT INTO tarot_recovery_claim(claim_id,account_id,claim_type,state,"
      "visibility,is_test,eligibility_threshold,grace_target,"
      "eligibility_balance,reward,draw_id,transaction_id,started_event_id,"
      "event_id,outbox_id,start_idempotency_key,completion_idempotency_key,"
      "created_at_ms,expires_at_ms,completed_at_ms,cooldown_until_ms) VALUES("
      "'00000000-0000-4000-8000-000000009007',"
      "'00000000-0000-4000-8000-000000009003','GRACE','pending','private',"
      "0,10,25,0,NULL,NULL,NULL,"
      "'00000000-0000-4000-8000-000000009008',NULL,NULL,"
      "'migration:grace-start',NULL,11,1000,NULL,NULL);"
      "INSERT INTO tarot_interaction_receipt(idempotency_key,operation,"
      "account_id,request_json,result_json,claim_id,transaction_id,"
      "created_at_ms) VALUES('migration:visibility','standings_visibility',"
      "'00000000-0000-4000-8000-000000009003','{}','{}',NULL,NULL,12);"
      "INSERT INTO tarot_history_cursor(cursor_id,account_id,item_count,"
      "created_at_ms,expires_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000009009',"
      "'00000000-0000-4000-8000-000000009003',1,13,913);"
      "INSERT INTO tarot_history_item(cursor_id,position,transaction_id) "
      "VALUES('00000000-0000-4000-8000-000000009009',0,"
      "'00000000-0000-4000-8000-000000009004');");

  REQUIRE(
      production_migrator(clock).apply(database.connection()).current_version ==
      15);
  REQUIRE(count(database.connection(), "tarot_transaction") == 1);
  REQUIRE(count(database.connection(), "tarot_posting") == 2);
  REQUIRE(count(database.connection(), "tarot_recovery_claim") == 1);
  REQUIRE(count(database.connection(), "tarot_interaction_receipt") == 1);
  REQUIRE(count(database.connection(), "tarot_history_item") == 1);
  auto preserved = database.connection().prepare(
      "SELECT ledger_sequence,transaction_type,state FROM tarot_transaction");
  REQUIRE(preserved.step());
  REQUIRE(preserved.column_int64(0) == 1);
  REQUIRE(preserved.column_text(1) == "STARTING_GRANT");
  REQUIRE(preserved.column_text(2) == "committed");
  auto sequence = database.connection().prepare(
      "SELECT seq FROM sqlite_sequence WHERE name='tarot_transaction'");
  REQUIRE(sequence.step());
  REQUIRE(sequence.column_int64(0) == 1);
  auto dependents = database.connection().prepare(
      "SELECT claim.state,receipt.operation,item.position,item.transaction_id "
      "FROM tarot_recovery_claim claim JOIN tarot_interaction_receipt receipt "
      "ON receipt.account_id=claim.account_id JOIN tarot_history_cursor cursor "
      "ON cursor.account_id=claim.account_id JOIN tarot_history_item item ON "
      "item.cursor_id=cursor.cursor_id");
  REQUIRE(dependents.step());
  REQUIRE(dependents.column_text(0) == "pending");
  REQUIRE(dependents.column_text(1) == "standings_visibility");
  REQUIRE(dependents.column_int64(2) == 0);
  REQUIRE(dependents.column_text(3) == "00000000-0000-4000-8000-000000009004");
  auto foreign_keys = database.connection().prepare("PRAGMA foreign_key_check");
  REQUIRE_FALSE(foreign_keys.step());
  REQUIRE_THROWS(database.connection().execute(
      "UPDATE tarot_posting SET amount=99 WHERE amount=100"));
  REQUIRE_THROWS(database.connection().execute(
      "UPDATE tarot_interaction_receipt SET result_json='{\"changed\":true}'"));
}

TEST_CASE(
    "version eleven seeds accepted peer results without retroactive effects",
    "[migration][wager][tarot][integration][baseline]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  const auto production = sanguinius::persistence::production_migrations();
  {
    auto database = Database::open_migration(temporary.path(), 25ms);
    const Migrator version_ten{
        std::span<const Migration>{production.data(), 10},
        {"test-version", "test-revision"},
        clock};
    REQUIRE(version_ten.apply(database.connection()).current_version == 10);
  }

  {
    auto context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            Database::open_runtime(temporary.path()));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({31, "Target", "target", false, 100});

    sanguinius::persistence::SqliteTarotRepository tarot{context};
    tarot.initialize_system_accounts({migration_uuid(1), migration_uuid(2),
                                      migration_uuid(3), migration_uuid(4)},
                                     100);
    std::size_t next_id = 10;
    const auto ids = [&next_id] { return migration_uuid(next_id++); };
    const auto invocation = [](const std::uint64_t user, std::string key,
                               const std::int64_t now_ms) {
      return sanguinius::WagerInvocation{.user_id = user,
                                         .guild_id = 10,
                                         .channel_id = 20,
                                         .interaction_idempotency_key =
                                             std::move(key),
                                         .correlation_id = "migration-baseline",
                                         .now_ms = now_ms,
                                         .owner = user == 30,
                                         .test_mode = false};
    };
    const auto provisioned = tarot.ensure_account(
        {.invocation = {.user_id = 30,
                        .guild_id = 10,
                        .channel_id = 20,
                        .display_name = "Owner",
                        .interaction_idempotency_key = "baseline:provision",
                        .correlation_id = "migration-baseline",
                        .now_ms = 100},
         .starting_fate = 100,
         .account_id = ids(),
         .transaction_id = ids(),
         .event_id = ids(),
         .mint_posting_id = ids(),
         .human_posting_id = ids()});
    REQUIRE(provisioned.balance == 100);

    sanguinius::persistence::SqliteWagerRepository wagers{context};
    const auto draft = wagers.create_draft(
        {.invocation = invocation(30, "baseline:draft", 1'000),
         .target_user_id = 31,
         .judge_user_id = std::nullopt,
         .visibility = sanguinius::WagerVisibility::public_offer,
         .resolution_policy = sanguinius::WagerResolutionPolicy::mutual,
         .outcome_window_ms = 3'600'000,
         .resolution_grace_ms = 172'800'000,
         .draft_expires_at_ms = 901'000,
         .is_test = false,
         .next_id = ids});
    REQUIRE(draft.status == sanguinius::WagerMutationStatus::applied);
    REQUIRE(draft.controls.size() == 1);
    REQUIRE(draft.controls.front().custom_id.starts_with(
        sanguinius::wager_form_prefix));
    const auto form_token = draft.controls.front().custom_id.substr(
        sanguinius::wager_form_prefix.size());
    const auto preview = wagers.preview(
        {.invocation = invocation(30, "baseline:preview", 1'010),
         .token_id = form_token,
         .proposition = "The accepted baseline result remains historical",
         .stake = 5,
         .evidence_instructions = std::nullopt,
         .offer_expiry_ms = 86'400'000,
         .next_id = ids});
    REQUIRE(preview.status == sanguinius::WagerMutationStatus::applied);
    const auto confirmed =
        wagers.act({.invocation = invocation(30, "baseline:confirm", 1'020),
                    .wager_id = draft.wager->wager_id,
                    .token_id = std::nullopt,
                    .action = sanguinius::WagerAction::confirm,
                    .starting_fate = 100,
                    .offer_expiry_ms = 86'400'000,
                    .resolution_grace_ms = 172'800'000,
                    .next_id = ids});
    REQUIRE(confirmed.status == sanguinius::WagerMutationStatus::applied);
    const auto accepted =
        wagers.act({.invocation = invocation(31, "baseline:accept", 1'030),
                    .wager_id = draft.wager->wager_id,
                    .token_id = std::nullopt,
                    .action = sanguinius::WagerAction::accept,
                    .starting_fate = 100,
                    .offer_expiry_ms = 86'400'000,
                    .resolution_grace_ms = 172'800'000,
                    .next_id = ids});
    REQUIRE(accepted.status == sanguinius::WagerMutationStatus::applied);
    REQUIRE(wagers
                .submit_outcome({.invocation = invocation(
                                     30, "baseline:owner-outcome", 1'040),
                                 .wager_id = draft.wager->wager_id,
                                 .token_id = std::nullopt,
                                 .winner = sanguinius::WagerRole::creator,
                                 .next_id = ids})
                .status == sanguinius::WagerMutationStatus::applied);
    const auto settled = wagers.submit_outcome(
        {.invocation = invocation(31, "baseline:target-outcome", 1'050),
         .wager_id = draft.wager->wager_id,
         .token_id = std::nullopt,
         .winner = sanguinius::WagerRole::creator,
         .next_id = ids});
    REQUIRE(settled.status == sanguinius::WagerMutationStatus::applied);
    REQUIRE(settled.wager->state == sanguinius::WagerState::resolved);
  }

  auto database = Database::open_migration(temporary.path(), 25ms);
  REQUIRE(
      production_migrator(clock).apply(database.connection()).current_version ==
      15);
  REQUIRE(count(database.connection(), "tarot_player_event") == 2);
  auto events = database.connection().prepare(
      "SELECT user_id,result,wager_kind,is_test,baseline FROM "
      "tarot_player_event ORDER BY user_id");
  REQUIRE(events.step());
  REQUIRE(events.column_text(0) == "30");
  REQUIRE(events.column_text(1) == "win");
  REQUIRE(events.column_text(2) == "peer");
  REQUIRE(events.column_int64(3) == 0);
  REQUIRE(events.column_int64(4) == 1);
  REQUIRE(events.step());
  REQUIRE(events.column_text(0) == "31");
  REQUIRE(events.column_text(1) == "loss");
  REQUIRE(events.column_int64(4) == 1);
  REQUIRE_FALSE(events.step());
  auto stats = database.connection().prepare(
      "SELECT user_id,wins,losses,current_win_streak,current_loss_streak,"
      "settled_house_wagers FROM tarot_player_stats ORDER BY user_id");
  REQUIRE(stats.step());
  REQUIRE(stats.column_text(0) == "30");
  REQUIRE(stats.column_int64(1) == 1);
  REQUIRE(stats.column_int64(2) == 0);
  REQUIRE(stats.column_int64(3) == 1);
  REQUIRE(stats.column_int64(4) == 0);
  REQUIRE(stats.column_int64(5) == 0);
  REQUIRE(stats.step());
  REQUIRE(stats.column_text(0) == "31");
  REQUIRE(stats.column_int64(1) == 0);
  REQUIRE(stats.column_int64(2) == 1);
  REQUIRE(stats.column_int64(3) == 0);
  REQUIRE(stats.column_int64(4) == 1);
  REQUIRE(stats.column_int64(5) == 0);
  REQUIRE_FALSE(stats.step());

  REQUIRE(count(database.connection(), "tarot_title_source") == 0);
  REQUIRE(count(database.connection(), "tarot_integration_observation") == 0);
  REQUIRE(count(database.connection(), "tarot_chronicle_proposal") == 0);
  REQUIRE(count(database.connection(), "tarot_appearance_candidate") == 0);
  auto retired_vox_intents = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
      "name='tarot_vox_narration_intent'");
  REQUIRE(retired_vox_intents.step());
  REQUIRE(retired_vox_intents.column_int64(0) == 0);
  REQUIRE(count(database.connection(), "voice_narration_intent") == 0);
  REQUIRE(count(database.connection(), "relationship_event") == 0);
}

TEST_CASE("an injected version-eleven failure leaves version ten intact",
          "[migration][tarot][house][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_ten{std::span<const Migration>{production.data(), 10},
                             {"test-version", "test-revision"},
                             clock};
  REQUIRE(version_ten.apply(database.connection()).current_version == 10);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);");

  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(),
                                 deny_v11_chronicle_swap,
                                 nullptr) == SQLITE_OK);
  REQUIRE_THROWS_AS(production_migrator(clock).apply(database.connection()),
                    DatabaseError);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(), nullptr,
                                 nullptr) == SQLITE_OK);

  REQUIRE(version_ten.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 10);
  REQUIRE(count(database.connection(), "discord_user") == 1);
  REQUIRE(count(database.connection(), "guild_config") == 1);
  auto old_shape = database.connection().prepare(
      "SELECT count(*) FROM pragma_table_info('chronicle_entry') WHERE "
      "name='source_event_id'");
  REQUIRE(old_shape.step());
  REQUIRE(old_shape.column_int64(0) == 0);
  auto m13_tables = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name IN "
      "('chronicle_entry_m13_new','tarot_catalog_snapshot',"
      "'tarot_house_wager','tarot_integration_observation')");
  REQUIRE(m13_tables.step());
  REQUIRE(m13_tables.column_int64(0) == 0);
  auto temp_tables = database.connection().prepare(
      "SELECT count(*) FROM sqlite_temp_schema WHERE name LIKE "
      "'m13_chronicle_%'");
  REQUIRE(temp_tables.step());
  REQUIRE(temp_tables.column_int64(0) == 0);
  auto foreign_keys = database.connection().prepare("PRAGMA foreign_key_check");
  REQUIRE_FALSE(foreign_keys.step());
}

TEST_CASE("an injected version-twelve failure leaves version eleven intact",
          "[migration][vox][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_eleven{
      std::span<const Migration>{production.data(), 11},
      {"test-version", "test-revision"}, clock};
  REQUIRE(version_eleven.apply(database.connection()).current_version == 11);

  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(),
                                 deny_v12_transition_table,
                                 nullptr) == SQLITE_OK);
  REQUIRE_THROWS_AS(production_migrator(clock).apply(database.connection()),
                    DatabaseError);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(), nullptr,
                                 nullptr) == SQLITE_OK);

  REQUIRE(version_eleven.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 11);
  auto vox_objects = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name IN "
      "('voice_session','voice_session_transition',"
      "'voice_interaction_receipt','voice_public_outbox_dependency',"
      "'voice_session_one_active_guild')");
  REQUIRE(vox_objects.step());
  REQUIRE(vox_objects.column_int64(0) == 0);
  auto foreign_keys = database.connection().prepare("PRAGMA foreign_key_check");
  REQUIRE_FALSE(foreign_keys.step());
}

TEST_CASE("version fourteen terminalizes legacy Vox intents without replay",
          "[migration][vox][narration][privacy]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_thirteen{
      std::span<const Migration>{production.data(), 13},
      {"test-version", "test-revision"}, clock};
  REQUIRE(version_thirteen.apply(database.connection()).current_version == 13);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,source_message_id,"
      "occurred_at_ms,recorded_at_ms,correlation_id,causation_id,"
      "idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000001401',"
      "'tarot.wager_resolved.v1','tarot_wager',"
      "'00000000-0000-4000-8000-000000001402','30','10','20',NULL,"
      "100,100,'legacy-narration',NULL,'legacy-narration','{}');"
      "INSERT INTO tarot_vox_narration_intent VALUES("
      "'00000000-0000-4000-8000-000000001403',"
      "'00000000-0000-4000-8000-000000001401','10','20',"
      "'Preserved public-safe legacy text.',0,'pending',100,86400100);");

  const auto applied = production_migrator(clock).apply(database.connection());
  REQUIRE(applied.current_version == 15);
  auto legacy = database.connection().prepare(
      "SELECT slot,feature,safe_input,state,terminal_reason,speech_id FROM "
      "voice_narration_intent WHERE intent_id="
      "'00000000-0000-4000-8000-000000001403'");
  REQUIRE(legacy.step());
  REQUIRE(legacy.column_text(0) == "legacy");
  REQUIRE(legacy.column_text(1) == "legacy");
  REQUIRE(legacy.column_text(2) == "Preserved public-safe legacy text.");
  REQUIRE(legacy.column_text(3) == "suppressed");
  REQUIRE(legacy.column_text(4) == "pre_m16_not_replayed");
  REQUIRE(legacy.column_is_null(5));
  REQUIRE(count(database.connection(), "voice_narration_transition") == 1);
  auto cursor = database.connection().prepare(
      "SELECT last_event_rowid FROM voice_narration_cursor WHERE singleton=1");
  REQUIRE(cursor.step());
  REQUIRE(cursor.column_int64(0) == 1);
  auto old_table = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
      "name='tarot_vox_narration_intent'");
  REQUIRE(old_table.step());
  REQUIRE(old_table.column_int64(0) == 0);
}

TEST_CASE("an injected version-fourteen failure leaves version thirteen intact",
          "[migration][vox][narration][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_thirteen{
      std::span<const Migration>{production.data(), 13},
      {"test-version", "test-revision"}, clock};
  REQUIRE(version_thirteen.apply(database.connection()).current_version == 13);

  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(),
                                 deny_v14_narration_table,
                                 nullptr) == SQLITE_OK);
  REQUIRE_THROWS_AS(production_migrator(clock).apply(database.connection()),
                    DatabaseError);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(), nullptr,
                                 nullptr) == SQLITE_OK);

  REQUIRE(version_thirteen.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 13);
  auto v14_objects = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name IN "
      "('voice_narration_cursor','voice_narration_intent',"
      "'voice_narration_transition')");
  REQUIRE(v14_objects.step());
  REQUIRE(v14_objects.column_int64(0) == 0);
  auto speech_rank = database.connection().prepare(
      "SELECT count(*) FROM pragma_table_info('speech_item') WHERE "
      "name='narration_rank'");
  REQUIRE(speech_rank.step());
  REQUIRE(speech_rank.column_int64(0) == 0);
  REQUIRE(count(database.connection(), "tarot_vox_narration_intent") == 0);
}

TEST_CASE("a failed wager migration leaves accepted version nine intact",
          "[migration][wager][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_nine{std::span<const Migration>{production.data(), 9},
                              {"test-version", "test-revision"},
                              clock};
  REQUIRE(version_nine.apply(database.connection()).current_version == 9);
  database.connection().execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);"
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000009101','tarot.account_provisioned.v1',"
      "'tarot_account','00000000-0000-4000-8000-000000009103','30','10','20',"
      "10,10,'rollback-test','rollback:event','{}');"
      "INSERT INTO "
      "tarot_account(account_id,account_kind,user_id,created_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000009102','MINT',NULL,10);"
      "INSERT INTO "
      "tarot_account(account_id,account_kind,user_id,created_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000009103','HUMAN','30',10);"
      "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
      "expected_posting_count,event_id,idempotency_key,actor_user_id,is_test,"
      "created_at_ms) VALUES('00000000-0000-4000-8000-000000009104',"
      "'STARTING_GRANT','prepared',2,"
      "'00000000-0000-4000-8000-000000009101','rollback:grant','30',0,10);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000009105',"
      "'00000000-0000-4000-8000-000000009104',"
      "'00000000-0000-4000-8000-000000009102',-100,10);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000009106',"
      "'00000000-0000-4000-8000-000000009104',"
      "'00000000-0000-4000-8000-000000009103',100,10);"
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=10 "
      "WHERE transaction_id='00000000-0000-4000-8000-000000009104';");

  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(),
                                 deny_v9_posting_drop, nullptr) == SQLITE_OK);
  REQUIRE_THROWS_AS(production_migrator(clock).apply(database.connection()),
                    DatabaseError);
  REQUIRE(sqlite3_set_authorizer(database.connection().native_handle(), nullptr,
                                 nullptr) == SQLITE_OK);
  REQUIRE(version_nine.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 9);
  REQUIRE(count(database.connection(), "tarot_transaction") == 1);
  REQUIRE(count(database.connection(), "tarot_posting") == 2);
  REQUIRE(count(database.connection(), "tarot_recovery_claim") == 0);
  auto renamed = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name IN "
      "('tarot_transaction_v9','tarot_posting_v9',"
      "'tarot_recovery_claim_v9','tarot_interaction_receipt_v9',"
      "'tarot_history_item_v9')");
  REQUIRE(renamed.step());
  REQUIRE(renamed.column_int64(0) == 0);
  auto wager = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name='tarot_wager'");
  REQUIRE(wager.step());
  REQUIRE(wager.column_int64(0) == 0);
  auto preserved = database.connection().prepare(
      "SELECT ledger_sequence,state FROM tarot_transaction WHERE "
      "transaction_id='00000000-0000-4000-8000-000000009104'");
  REQUIRE(preserved.step());
  REQUIRE(preserved.column_int64(0) == 1);
  REQUIRE(preserved.column_text(1) == "committed");
}

TEST_CASE(
    "a failed Tarot migration leaves the accepted version-eight schema intact",
    "[migration][tarot][rollback]") {
  constexpr std::string_view checksum{
      "0000000000000000000000000000000000000000000000000000000000000000"};
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database = Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const Migrator version_eight{std::span<const Migration>{production.data(), 8},
                               {"test-version", "test-revision"},
                               clock};
  REQUIRE(version_eight.apply(database.connection()).current_version == 8);

  std::vector<Migration> broken{production.begin(), production.begin() + 8};
  broken.push_back(
      Migration{9, "broken_tarot", checksum,
                "CREATE TABLE partial_tarot_table (id INTEGER) STRICT; "
                "THIS IS NOT SQL;"});
  const Migrator broken_tarot{broken, {"test-version", "test-revision"}, clock};
  REQUIRE_THROWS_AS(broken_tarot.apply(database.connection()), DatabaseError);
  REQUIRE(version_eight.inspect(database.connection()).state ==
          SchemaState::current);
  REQUIRE(count(database.connection(), "schema_migrations") == 8);
  auto partial = database.connection().prepare(
      "SELECT count(*) FROM sqlite_schema WHERE name='partial_tarot_table'");
  REQUIRE(partial.step());
  REQUIRE(partial.column_int64(0) == 0);
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
