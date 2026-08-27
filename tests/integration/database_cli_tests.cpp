#include "sanguinius/database_cli.hpp"
#include "sanguinius/persistence/database.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <chrono>
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

void initialize_scope(sanguinius::persistence::SqliteConnection &connection) {
  connection.execute_script(
      "INSERT INTO "
      "discord_user(user_id,is_bot,first_seen_at_ms,last_seen_at_ms,"
      "created_at_ms,updated_at_ms) VALUES('30',0,1,1,1,1),"
      "('31',0,1,1,1,1);"
      "INSERT INTO user_preference(user_id,updated_at_ms) VALUES('30',1),"
      "('31',1);"
      "INSERT INTO guild_config(singleton,guild_id,primary_channel_id,"
      "owner_user_id,created_at_ms,updated_at_ms) "
      "VALUES(1,'10','20','30',1,1);");
}

void append_event(sanguinius::persistence::SqliteConnection &connection,
                  const std::string_view event_id,
                  const std::string_view event_type,
                  const std::string_view aggregate_type,
                  const std::string_view aggregate_id) {
  auto insert = connection.prepare(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,source_message_id,"
      "occurred_at_ms,recorded_at_ms,correlation_id,causation_id,"
      "idempotency_key,payload_json) VALUES(?,?,?,?,?,'10','20',NULL,1,1,"
      "'consumer-lag',NULL,?,'{}')");
  insert.bind(1, event_id);
  insert.bind(2, event_type);
  insert.bind(3, aggregate_type);
  insert.bind(4, aggregate_id);
  insert.bind(5, "31");
  insert.bind(6, "consumer-lag:" + std::string{event_id});
  insert.execute();
}

void catch_up_narration_cursor(
    sanguinius::persistence::SqliteConnection &connection) {
  connection.execute(
      "UPDATE voice_narration_cursor SET last_event_rowid="
      "COALESCE((SELECT MAX(rowid) FROM event_journal),0),updated_at_ms=1 "
      "WHERE singleton=1");
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
  REQUIRE(migrated.output.find("current_schema=16") != std::string::npos);

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

  const auto invariants =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(invariants.exit_code == 0);
  REQUIRE(invariants.output.find("ai_budgets=ok\n") != std::string::npos);
  REQUIRE(invariants.output.find("provider_circuits=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("runtime_controls=ok\n") != std::string::npos);
  REQUIRE(invariants.output.find("outbox_dependencies=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("speech_narration=ok\n") != std::string::npos);
  REQUIRE(invariants.output.find("consumer_relationships=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("consumer_tarot_house=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("consumer_tarot_integration=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("consumer_appearances=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("consumer_vox_narration=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("interaction_snapshots=ok\n") !=
          std::string::npos);
  REQUIRE(invariants.output.find("tarot_wagers=ok\n") != std::string::npos);
  const auto invariants_rebuilt =
      run({sanguinius::DatabaseCommandType::invariants_rebuild, std::nullopt},
          temporary.path(), clock);
  REQUIRE(invariants_rebuilt.exit_code == 0);
  REQUIRE(invariants_rebuilt.output.find("post_rebuild_check=ok\n") !=
          std::string::npos);

  const auto backed_up = run({sanguinius::DatabaseCommandType::backup, backup},
                             temporary.path(), clock);
  REQUIRE(backed_up.exit_code == 0);
  REQUIRE(backed_up.output.find("backup=verified") != std::string::npos);
  REQUIRE(backed_up.output.find(temporary.root().string()) ==
          std::string::npos);
  REQUIRE(std::filesystem::is_regular_file(backup));
}

TEST_CASE("database invariants reject every cross-feature consumer backlog",
          "[database-cli][invariants][orchestration][backlog]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  std::string expected_lag;
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    auto &connection = database.connection();
    initialize_scope(connection);

    SECTION("relationship source") {
      connection.execute_script(
          "UPDATE user_preference SET chronicle_opt_in=1 WHERE user_id='31';"
          "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,"
          "visibility,status,occurred_at_ms,created_at_ms,created_by_user_id,"
          "submitted_at_ms,approved_at_ms,approved_by_user_id,source_guild_id,"
          "source_channel_id,source_message_id,source_author_user_id,source_"
          "text,"
          "revision,source_kind) VALUES("
          "'00000000-0000-4000-8000-000000000510','incident','Lag source',"
          "'A valid shared Chronicle source.','shared','canon',1,1,'31',1,1,"
          "'30','10','20','510','31','A valid shared Chronicle source.',1,"
          "'discord_message');"
          "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES("
          "'00000000-0000-4000-8000-000000000510','31','source_author');");
      append_event(connection, "00000000-0000-4000-8000-000000000511",
                   "chronicle.entry_canonized.v1", "chronicle_entry",
                   "00000000-0000-4000-8000-000000000510");
      connection.execute(
          "UPDATE appearance_event_observation SET extraction_result="
          "'source_not_enabled',processed_at_ms=1 WHERE source_event_id="
          "'00000000-0000-4000-8000-000000000511'");
      catch_up_narration_cursor(connection);
      expected_lag = "consumer_relationships=lag(1)\n";
    }

    SECTION("House draw reconciliation") {
      connection.execute_script(
          "INSERT INTO tarot_catalog_snapshot(catalog_version,catalog_kind,"
          "canonical_json,checksum,installed_at_ms) VALUES"
          "('deck-lag','deck','{}','deck-lag-1',1),"
          "('house-lag','house','{}','house-lag-1',1);"
          "INSERT INTO tarot_card_definition(catalog_version,ordinal,slug,name,"
          "meaning,theme_tag,safety_prompt,flavor_json) VALUES("
          "'deck-lag',0,'lag-card','Lag Card','A sufficiently long meaning.',"
          "'lag-theme','A sufficiently safe prompt.','[\"A flavor.\"]');"
          "INSERT INTO tarot_house_template_definition(catalog_version,"
          "template_slug,canonical_json) "
          "VALUES('house-lag','lag-template','{}');");
      append_event(connection, "00000000-0000-4000-8000-000000000520",
                   "tarot.house_accepted.v1", "tarot_house_wager",
                   "00000000-0000-4000-8000-000000000522");
      append_event(connection, "00000000-0000-4000-8000-000000000521",
                   "tarot.draw_created.v1", "tarot_draw",
                   "00000000-0000-4000-8000-000000000523");
      connection.execute_script(
          "INSERT INTO tarot_card_draw(draw_id,user_id,guild_id,channel_id,"
          "visibility,catalog_version,card_ordinal,flavor_variant,drawn_at_ms,"
          "cooldown_until_ms,is_test,event_id) VALUES("
          "'00000000-0000-4000-8000-000000000523','31','10','20','private',"
          "'deck-lag',0,0,2,3,0,"
          "'00000000-0000-4000-8000-000000000521');"
          "INSERT INTO tarot_house_wager(wager_id,user_id,guild_id,channel_id,"
          "catalog_version,template_slug,proposition,choice_slug,choice_label,"
          "odds_numerator,odds_denominator,stake,profit,visibility,authority,"
          "state,accepted_at_ms,outcome_due_at_ms,terminal_cooldown_ms,"
          "cooldown_until_ms,recovery,is_test,accepted_event_id) VALUES("
          "'00000000-0000-4000-8000-000000000522','31','10','20','house-lag',"
          "'lag-template','A qualifying draw resolves this.','yes','Yes',0,1,"
          "0,0,'private','draw','accepted_funded',1,100,0,1,0,0,"
          "'00000000-0000-4000-8000-000000000520');");
      catch_up_narration_cursor(connection);
      expected_lag = "consumer_tarot_house=lag(1)\n";
    }

    SECTION("Tarot integration observation") {
      append_event(connection, "00000000-0000-4000-8000-000000000530",
                   "tarot.wager_resolved.v1", "tarot_wager",
                   "00000000-0000-4000-8000-000000000531");
      connection.execute(
          "INSERT INTO "
          "tarot_integration_observation(source_event_id,event_type,"
          "visibility,is_test,state,attempts,next_attempt_at_ms,last_error,"
          "created_at_ms,processed_at_ms) VALUES("
          "'00000000-0000-4000-8000-000000000530',"
          "'tarot.wager_resolved.v1','public',0,'pending',0,1,NULL,1,NULL)");
      catch_up_narration_cursor(connection);
      expected_lag = "consumer_tarot_integration=lag(1)\n";
    }

    SECTION("appearance observation") {
      append_event(connection, "00000000-0000-4000-8000-000000000540",
                   "chronicle.session_started.v1", "chronicle_session",
                   "00000000-0000-4000-8000-000000000541");
      catch_up_narration_cursor(connection);
      expected_lag = "consumer_appearances=lag(1)\n";
    }

    SECTION("Vox narration cursor") {
      append_event(connection, "00000000-0000-4000-8000-000000000550",
                   "system.consumer_test.v1", "system", "consumer-test");
      expected_lag = "consumer_vox_narration=lag(1)\n";
    }
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  CAPTURE(expected_lag, result.output, result.errors);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find(expected_lag) != std::string::npos);
}

TEST_CASE("database invariants accept disabled Tarot integration terminals",
          "[database-cli][invariants][orchestration][tarot][disabled]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    auto &connection = database.connection();
    initialize_scope(connection);
    append_event(connection, "00000000-0000-4000-8000-000000000560",
                 "tarot.wager_resolved.v1", "tarot_wager",
                 "00000000-0000-4000-8000-000000000561");
    connection.execute(
        "INSERT INTO tarot_integration_observation(source_event_id,event_type,"
        "visibility,is_test,state,attempts,next_attempt_at_ms,last_error,"
        "created_at_ms,processed_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000560',"
        "'tarot.wager_resolved.v1','public',0,'suppressed',1,1,"
        "'integration_disabled',1,1)");
    catch_up_narration_cursor(connection);
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  CAPTURE(result.output, result.errors);
  REQUIRE(result.exit_code == 0);
  REQUIRE(result.output.find("consumer_tarot_integration=ok\n") !=
          std::string::npos);
}

TEST_CASE("database invariants recompute successful AI charges",
          "[database-cli][invariants][ai][budget]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute_script(
        "INSERT INTO discord_user(user_id,is_bot,first_seen_at_ms,"
        "last_seen_at_ms,created_at_ms,updated_at_ms) "
        "VALUES('30',0,1,1,1,1);"
        "INSERT INTO guild_config(singleton,guild_id,primary_channel_id,"
        "owner_user_id,created_at_ms,updated_at_ms) "
        "VALUES(1,'10','20','30',1,1);"
        "INSERT INTO ai_generation_attempt(attempt_id,guild_id,"
        "requester_user_id,purpose,priority,model,"
        "input_rate_micro_usd_per_million,"
        "output_rate_micro_usd_per_million,reserved_input_tokens,"
        "reserved_output_tokens,reserved_micro_usd,actual_input_tokens,"
        "actual_output_tokens,actual_micro_usd,provider_sent,"
        "provider_request_id,state,result_code,idempotency_key,created_at_ms,"
        "submitted_at_ms,completed_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000410','10','30','direct',"
        "'direct','audited-test-model',1000000,2000000,100,50,200,10,5,0,1,"
        "'provider-request','succeeded','ok','ai:invariant:wrong-cost',1,1,1)");
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find("ai_budgets=failed\n") != std::string::npos);
}

TEST_CASE("database invariants reject concurrent nonterminal listening windows",
          "[database-cli][invariants][voice-input][privacy]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  auto database =
      sanguinius::persistence::Database::open_runtime(temporary.path());
  auto &connection = database.connection();
  connection.execute(
      "CREATE TEMP TABLE voice_listening_window(state TEXT NOT NULL) STRICT");

  const std::array states{"proposed", "arming_transport", "arming_indicator",
                          "active", "transcribing"};
  for (const std::string_view state : states) {
    connection.execute("DELETE FROM temp.voice_listening_window");
    auto insert = connection.prepare(
        "INSERT INTO temp.voice_listening_window(state) VALUES(?),(?)");
    insert.bind(1, state);
    insert.bind(2, state);
    insert.execute();
    std::ostringstream output;
    const auto valid = sanguinius::database_cli_detail::check_core_invariants(
        connection, output);
    CAPTURE(state, output.str());
    REQUIRE_FALSE(valid);
    REQUIRE(output.str().find("voice_input_privacy=failed\n") !=
            std::string::npos);
  }
}

TEST_CASE("database invariants reject voice privacy audit drift",
          "[database-cli][invariants][voice-input][privacy][audit]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  auto database =
      sanguinius::persistence::Database::open_runtime(temporary.path());
  auto &connection = database.connection();
  initialize_scope(connection);

  SECTION("kill switch state has no immutable change") {
    connection.execute(
        "UPDATE voice_input_control SET disabled=1,updated_at_ms=1 WHERE "
        "singleton=1");
  }

  SECTION("listening window state has no transition chain") {
    connection.execute_script(
        "INSERT INTO application_instance(instance_id,application_version,"
        "git_revision,hostname,process_id,started_at_ms,stopped_at_ms,"
        "stop_reason) VALUES("
        "'00000000-0000-4000-8000-000000000570','test','test','test-host',"
        "1,0,1,'clean_shutdown');"
        "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
        "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
        "state_version,connection_generation,started_at_ms,last_active_at_ms) "
        "VALUES('00000000-0000-4000-8000-000000000571','10','20','21','31',"
        "'00000000-0000-4000-8000-000000000570','ready',1,1,1,1);"
        "INSERT INTO voice_listening_window(window_id,vox_session_id,guild_id,"
        "text_channel_id,voice_channel_id,requester_user_id,state,state_"
        "version,"
        "connection_generation,requested_seconds,initial_human_count,"
        "reserved_micro_usd,created_at_ms,interaction_idempotency_key,"
        "request_fingerprint) VALUES("
        "'00000000-0000-4000-8000-000000000572',"
        "'00000000-0000-4000-8000-000000000571','10','20','21','31',"
        "'proposed',1,1,5,1,375,1,'voice:audit:missing-transition',"
        "'voice-audit-fingerprint');");
  }

  SECTION("listening window history contains an illegal state edge") {
    connection.execute_script(
        "INSERT INTO application_instance(instance_id,application_version,"
        "git_revision,hostname,process_id,started_at_ms,stopped_at_ms,"
        "stop_reason) VALUES("
        "'00000000-0000-4000-8000-000000000573','test','test','test-host',"
        "1,0,1,'clean_shutdown');"
        "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
        "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
        "state_version,connection_generation,started_at_ms,last_active_at_ms,"
        "ended_at_ms,end_reason) VALUES("
        "'00000000-0000-4000-8000-000000000574','10','20','21','31',"
        "'00000000-0000-4000-8000-000000000573','inactive',1,1,1,1,2,"
        "'test_complete');"
        "INSERT INTO voice_listening_window(window_id,vox_session_id,guild_id,"
        "text_channel_id,voice_channel_id,requester_user_id,state,state_"
        "version,"
        "connection_generation,requested_seconds,initial_human_count,"
        "reserved_micro_usd,created_at_ms,ended_at_ms,terminal_reason,"
        "interaction_idempotency_key,request_fingerprint) VALUES("
        "'00000000-0000-4000-8000-000000000575',"
        "'00000000-0000-4000-8000-000000000574','10','20','21','31',"
        "'completed',2,1,5,1,375,1,2,'test_complete',"
        "'voice:audit:illegal-transition','voice-audit-illegal');"
        "INSERT INTO voice_listening_transition(transition_id,window_id,"
        "from_state,to_state,from_version,to_version,reason,idempotency_key,"
        "occurred_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000576',"
        "'00000000-0000-4000-8000-000000000575','none','proposed',0,1,"
        "'command_accepted','voice:audit:illegal:first',1),"
        "('00000000-0000-4000-8000-000000000577',"
        "'00000000-0000-4000-8000-000000000575','proposed','completed',1,2,"
        "'test_complete','voice:audit:illegal:second',2);");
  }

  std::ostringstream output;
  REQUIRE_FALSE(sanguinius::database_cli_detail::check_core_invariants(
      connection, output));
  REQUIRE(output.str().find("voice_input_privacy=failed\n") !=
          std::string::npos);
}

TEST_CASE("database invariants accept a complete voice privacy audit chain",
          "[database-cli][invariants][voice-input][privacy][audit]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  auto database =
      sanguinius::persistence::Database::open_runtime(temporary.path());
  auto &connection = database.connection();
  initialize_scope(connection);
  connection.execute_script(
      "INSERT INTO application_instance(instance_id,application_version,"
      "git_revision,hostname,process_id,started_at_ms,stopped_at_ms,"
      "stop_reason) VALUES("
      "'00000000-0000-4000-8000-000000000580','test','test','test-host',1,0,"
      "1,'clean_shutdown');"
      "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
      "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
      "state_version,connection_generation,started_at_ms,last_active_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000000581','10','20','21','31',"
      "'00000000-0000-4000-8000-000000000580','ready',1,1,1,1);"
      "INSERT INTO voice_input_consent_attestation(attestation_id,attested,"
      "owner_user_id,recorded_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000000582',1,'30',1);"
      "INSERT INTO voice_listening_window(window_id,vox_session_id,guild_id,"
      "text_channel_id,voice_channel_id,requester_user_id,state,state_version,"
      "connection_generation,requested_seconds,initial_human_count,"
      "reserved_micro_usd,created_at_ms,interaction_idempotency_key,"
      "request_fingerprint) VALUES("
      "'00000000-0000-4000-8000-000000000583',"
      "'00000000-0000-4000-8000-000000000581','10','20','21','31',"
      "'proposed',1,1,5,1,375,1,'voice:audit:complete',"
      "'voice-audit-complete');"
      "INSERT INTO voice_listening_transition(transition_id,window_id,"
      "from_state,to_state,from_version,to_version,reason,idempotency_key,"
      "occurred_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000000584',"
      "'00000000-0000-4000-8000-000000000583','none','proposed',0,1,"
      "'command_accepted','voice:audit:complete:transition',1);");

  std::ostringstream valid_output;
  REQUIRE(sanguinius::database_cli_detail::check_core_invariants(connection,
                                                                 valid_output));
  REQUIRE(valid_output.str().find("voice_input_privacy=ok\n") !=
          std::string::npos);

  connection.execute_script(
      "UPDATE voice_input_control SET disabled=1,updated_at_ms=2 WHERE "
      "singleton=1;"
      "INSERT INTO voice_input_kill_change(change_id,disabled,actor_user_id,"
      "occurred_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000000585',1,'30',2);");
  std::ostringstream disabled_output;
  REQUIRE_FALSE(sanguinius::database_cli_detail::check_core_invariants(
      connection, disabled_output));
  REQUIRE(disabled_output.str().find("voice_input_privacy=failed\n") !=
          std::string::npos);
}

TEST_CASE("database invariants reject safety state without its audit chain",
          "[database-cli][invariants][safety][circuit]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute(
        "UPDATE runtime_feature_control SET disabled=1,revision=2,"
        "changed_at_ms=1 WHERE feature='text-ai'");
  }
  auto result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find("runtime_controls=failed\n") != std::string::npos);

  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute(
        "UPDATE runtime_feature_control SET disabled=0,revision=1,"
        "changed_at_ms=0 WHERE feature='text-ai'");
    database.connection().execute(
        "UPDATE provider_circuit_state SET revision=2,updated_at_ms=1 "
        "WHERE provider='openai_tts'");
  }
  result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find("provider_circuits=failed\n") !=
          std::string::npos);
}

TEST_CASE("database invariants detect incomplete interaction snapshots",
          "[database-cli][invariants][pagination]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute(
        "INSERT INTO discord_user(user_id,is_bot,first_seen_at_ms,"
        "last_seen_at_ms,created_at_ms,updated_at_ms) "
        "VALUES('30',0,1,1,1,1)");
    database.connection().execute(
        "INSERT INTO interaction_list_snapshot(snapshot_id,snapshot_kind,"
        "viewer_user_id,subject_user_id,owner_view,item_count,created_at_ms,"
        "expires_at_ms) VALUES('00000000-0000-4000-8000-000000000400',"
        "'chronicle_titles','30','30',0,1,1,10000)");
  }

  const auto invariants =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(invariants.exit_code == 1);
  REQUIRE(invariants.output.find("interaction_snapshots=failed\n") !=
          std::string::npos);
}

TEST_CASE("database invariants reject malformed appearance public outbox",
          "[database-cli][invariants][appearance][privacy]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    int triggers_were_enabled{};
    REQUIRE(sqlite3_db_config(database.connection().native_handle(),
                              SQLITE_DBCONFIG_ENABLE_TRIGGER, 0,
                              &triggers_were_enabled) == SQLITE_OK);
    REQUIRE(triggers_were_enabled == 0);
    database.connection().execute_script(
        "INSERT INTO discord_user(user_id,is_bot,first_seen_at_ms,"
        "last_seen_at_ms,created_at_ms,updated_at_ms) "
        "VALUES('30',0,1,1,1,1);"
        "INSERT INTO guild_config(singleton,guild_id,primary_channel_id,"
        "owner_user_id,created_at_ms,updated_at_ms) "
        "VALUES(1,'10','20','30',1,1);"
        "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
        "target_guild_id,target_channel_id,target_user_id,payload_json,state,"
        "attempt_count,max_attempts,idempotency_key,provider_nonce,created_at_"
        "ms,"
        "available_at_ms,updated_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000420',"
        "'discord.public.v1','appearance','malformed','10','20',NULL,'{}',"
        "'pending',0,5,'appearance:malformed',"
        "'0000000000004000000000420',1,1,1);");
    REQUIRE(sqlite3_db_config(database.connection().native_handle(),
                              SQLITE_DBCONFIG_ENABLE_TRIGGER, 1,
                              nullptr) == SQLITE_OK);
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_check, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find("appearance_provenance=failed\n") !=
          std::string::npos);
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

TEST_CASE("projection rebuild refuses a heartbeat from a long-running process",
          "[database-cli][invariants][heartbeat]") {
  using namespace std::chrono_literals;
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{1h}};
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute(
        "INSERT INTO application_instance(instance_id,application_version,"
        "git_revision,hostname,process_id,started_at_ms,heartbeat_at_ms) "
        "VALUES('00000000-0000-4000-8000-000000000410','test','revision',"
        "'host',1,1,3600000)");
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_rebuild, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.errors ==
          "Projection rebuild refused: an application instance was recently "
          "active.\n");
}

TEST_CASE("failed umbrella rebuild rolls back every projection change",
          "[database-cli][invariants][rollback]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  REQUIRE(run({sanguinius::DatabaseCommandType::migrate, std::nullopt},
              temporary.path(), clock)
              .exit_code == 0);
  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path());
    database.connection().execute(
        "INSERT INTO discord_user(user_id,is_bot,first_seen_at_ms,"
        "last_seen_at_ms,created_at_ms,updated_at_ms) "
        "VALUES('30',0,1,1,1,1)");
    database.connection().execute(
        "INSERT INTO relationship_state(subject_user_id,familiarity,esteem,"
        "mirth,reliability,wariness,interaction_count,last_interaction_at_ms,"
        "projection_version,updated_at_ms) VALUES('30',9,0,0,0,0,1,1,1,1)");
    database.connection().execute(
        "INSERT INTO interaction_list_snapshot(snapshot_id,snapshot_kind,"
        "viewer_user_id,subject_user_id,owner_view,item_count,created_at_ms,"
        "expires_at_ms) VALUES('00000000-0000-4000-8000-000000000411',"
        "'chronicle_titles','30','30',0,1,1,10000)");
  }

  const auto result =
      run({sanguinius::DatabaseCommandType::invariants_rebuild, std::nullopt},
          temporary.path(), clock);
  REQUIRE(result.exit_code == 1);
  REQUIRE(result.output.find("post_rebuild_check=failed\n") !=
          std::string::npos);
  auto database =
      sanguinius::persistence::Database::open_runtime(temporary.path());
  auto projection = database.connection().prepare(
      "SELECT familiarity FROM relationship_state WHERE subject_user_id='30'");
  REQUIRE(projection.step());
  REQUIRE(projection.column_int64(0) == 9);
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
