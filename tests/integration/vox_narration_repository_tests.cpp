#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_speech_repository.hpp"
#include "sanguinius/persistence/sqlite_vox_narration_repository.hpp"
#include "sanguinius/persistence/sqlite_vox_repository.hpp"
#include "sanguinius/tts.hpp"

#include "support/fake_ai_client.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <barrier>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "60000000-0000-4000-8000-" + suffix;
}

void insert_scope(sanguinius::persistence::SqliteConnection &connection) {
  connection.execute_script(
      "INSERT INTO discord_user VALUES('30','Owner','owner',0,1,1,1,1);"
      "INSERT INTO guild_config(guild_id,primary_channel_id,owner_user_id,"
      "created_at_ms,updated_at_ms) VALUES('10','20','30',1,1);");
}

void insert_event(sanguinius::persistence::SqliteConnection &connection,
                  const std::string_view event_id,
                  const std::string_view event_type,
                  const std::string_view aggregate_id,
                  const std::int64_t recorded_at_ms,
                  const std::string_view payload = "{}") {
  auto insert = connection.prepare(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,source_message_id,"
      "occurred_at_ms,recorded_at_ms,correlation_id,causation_id,"
      "idempotency_key,payload_json) VALUES(?,?,'test',?,'30','10','20',"
      "NULL,?,?,'narration-test',NULL,?,?)");
  insert.bind(1, event_id);
  insert.bind(2, event_type);
  insert.bind(3, aggregate_id);
  insert.bind(4, recorded_at_ms);
  insert.bind(5, recorded_at_ms);
  insert.bind(6, "event:" + std::string{event_id});
  insert.bind(7, payload);
  insert.execute();
}

void insert_tts_attempt(sanguinius::persistence::SqliteConnection &connection,
                        const std::size_t id,
                        const std::int64_t submitted_at_ms) {
  auto insert = connection.prepare(
      "INSERT INTO tts_usage_attempt(attempt_id,speech_id,attempt_number,"
      "provider,model,voice_id,scalar_count,estimated_micro_usd,state,"
      "submitted_at_ms,completed_at_ms) VALUES(?,?,1,'openai','tts-1','onyx',"
      "1,15,'succeeded',?,?)");
  insert.bind(1, uuid(id));
  insert.bind(2, uuid(id + 1));
  insert.bind(3, submitted_at_ms);
  insert.bind(4, submitted_at_ms);
  insert.execute();
}

void insert_queued_speech(sanguinius::persistence::SqliteConnection &connection,
                          const std::string_view session_id,
                          const std::size_t id,
                          const std::int64_t created_at_ms) {
  auto insert = connection.prepare(
      "INSERT INTO speech_item(speech_id,voice_session_id,source_kind,text,"
      "text_hash,scalar_count,provider,model,voice_id,priority,narration_rank,"
      "state,state_version,earliest_at_ms,interruptible,deduplication_key,"
      "created_at_ms) VALUES(?,?,'interactive','a',?,1,'openai','tts-1',"
      "'onyx',300,0,'pending',1,?,0,?,?)");
  insert.bind(1, uuid(id));
  insert.bind(2, session_id);
  insert.bind(3, std::string(64, 'a'));
  insert.bind(4, created_at_ms);
  insert.bind(5, "speech:test:" + std::to_string(id));
  insert.bind(6, created_at_ms);
  insert.execute();
}

void insert_public_outbox(sanguinius::persistence::SqliteConnection &connection,
                          const std::string_view outbox_id,
                          const std::string_view aggregate_id,
                          const std::string_view causation_event_id,
                          const std::int64_t created_at_ms,
                          const char nonce_digit) {
  auto insert = connection.prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,target_user_id,payload_json,state,"
      "max_attempts,idempotency_key,provider_nonce,created_at_ms,"
      "available_at_ms,updated_at_ms) VALUES(?, 'test.public', 'test', ?,"
      "'10','20',NULL,json_object('content','public card',"
      "'causation_event_id',?),'pending',20,?,?,?, ?, ?)");
  insert.bind(1, outbox_id);
  insert.bind(2, aggregate_id);
  insert.bind(3, causation_event_id);
  insert.bind(4, "outbox:test:" + std::string{outbox_id});
  insert.bind(5, std::string(25, nonce_digit));
  insert.bind(6, created_at_ms);
  insert.bind(7, created_at_ms);
  insert.bind(8, created_at_ms);
  insert.execute();
}

struct NarrationFixture {
  NarrationFixture() {
    {
      auto migration = sanguinius::persistence::Database::open_migration(
          temporary.path(), 25ms);
      sanguinius::persistence::Migrator migrator{
          sanguinius::persistence::production_migrations(),
          {"test-version", "test-revision"},
          clock};
      REQUIRE(migrator.apply(migration.connection()).current_version == 15);
      insert_scope(migration.connection());
      migration.connection().execute(
          "INSERT INTO application_instance(instance_id,application_version,"
          "git_revision,hostname,process_id,started_at_ms) VALUES("
          "'60000000-0000-4000-8000-000000000099','test','revision','host',1,"
          "1)");
    }
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            sanguinius::persistence::Database::open_runtime(temporary.path(),
                                                            25ms));
    repository =
        std::make_unique<sanguinius::persistence::SqliteVoxNarrationRepository>(
            context);
  }

  [[nodiscard]] std::function<std::string()> ids() {
    return [this] { return uuid(++id_sequence); };
  }

  void insert_ready_voice_session(const std::string_view session_id) {
    auto &connection = context->connection();
    auto insert = connection.prepare(
        "INSERT INTO voice_session(session_id,guild_id,text_channel_id,"
        "voice_channel_id,summoner_user_id,deployment_instance_id,state,"
        "state_version,connection_generation,reconnect_count,fixture_state,"
        "narration_event_rowid_floor,started_at_ms,last_active_at_ms) VALUES("
        "?,'10','20','40','30','60000000-0000-4000-8000-000000000099',"
        "'ready',2,1,0,'pending',(SELECT COALESCE(max(rowid),0) FROM "
        "event_journal),50,50)");
    insert.bind(1, session_id);
    insert.execute();
  }

  void insert_pending_intent(
      const std::string_view intent_id, const std::string_view source_event_id,
      const std::string_view session_id,
      const std::string_view feature = "chronicle",
      const std::string_view event_type = "chronicle.session_started.v1",
      const std::int64_t rank = 60, const std::int64_t created_at_ms = 100,
      const std::int64_t expires_at_ms = 120'100) {
    std::string safe_input{"The shared Chronicle session is open."};
    std::string fallback{"The Chronicle opens for this gathering."};
    if (event_type == "chronicle.title_awarded.v1") {
      auto source = context->connection().prepare(
          "SELECT aggregate_id FROM event_journal WHERE event_id=?");
      source.bind(1, source_event_id);
      REQUIRE(source.step());
      const auto grant_id = source.column_text(0);
      auto definition = context->connection().prepare(
          "INSERT OR IGNORE INTO "
          "chronicle_title_definition(definition_id,title,"
          "description,provenance,proposed_by_user_id,created_at_ms) VALUES(?,"
          "'Keeper','Public title','owner_curated','30',90)");
      definition.bind(1, source_event_id);
      definition.execute();
      auto grant = context->connection().prepare(
          "INSERT OR IGNORE INTO chronicle_title_grant(grant_id,definition_id,"
          "recipient_user_id,state,featured,revision,source_idempotency_key,"
          "proposed_at_ms,decided_at_ms,decided_by_user_id) VALUES(?,?,'30',"
          "'active',0,1,?,90,90,'30')");
      grant.bind(1, grant_id);
      grant.bind(2, source_event_id);
      grant.bind(3, "title:test:" + std::string{source_event_id});
      grant.execute();
      safe_input =
          "Public active title: Keeper. Recipient display name: Owner.";
      fallback = "A worthy name is entered into the Chronicle.";
    } else if (event_type == "chronicle.session_started.v1" ||
               event_type == "chronicle.session_completed.v1") {
      auto source = context->connection().prepare(
          "SELECT aggregate_id FROM event_journal WHERE event_id=?");
      source.bind(1, source_event_id);
      REQUIRE(source.step());
      const auto chronicle_id = source.column_text(0);
      const auto completed = event_type == "chronicle.session_completed.v1";
      auto chronicle = context->connection().prepare(
          "INSERT OR IGNORE INTO chronicle_session(session_id,guild_id,"
          "channel_id,opened_by_user_id,state,opened_at_ms,closed_at_ms,"
          "revision,start_idempotency_key) VALUES(?,'10','20','30',?,90,?,1,"
          "?)");
      chronicle.bind(1, chronicle_id);
      chronicle.bind(2, completed ? "closed" : "open");
      if (completed)
        chronicle.bind(3, 100);
      else
        chronicle.bind_null(3);
      chronicle.bind(4, "chronicle:test:" + std::string{source_event_id});
      chronicle.execute();
      if (completed) {
        safe_input = "The shared Chronicle session is closed.";
        fallback = "The Chronicle closes for this gathering.";
      }
    }
    auto insert = context->connection().prepare(
        "INSERT INTO voice_narration_intent(intent_id,source_event_id,slot,"
        "feature,event_type,guild_id,channel_id,safe_input,fallback_line,"
        "narration_rank,created_at_ms,expires_at_ms,session_id,"
        "counterpart_required,model_status,is_test,state,state_version) VALUES("
        "?,?,'feature',?,?,'10','20',?,?,?,?,?,?,0,"
        "'not_requested',0,'pending',1)");
    insert.bind(1, intent_id);
    insert.bind(2, source_event_id);
    insert.bind(3, feature);
    insert.bind(4, event_type);
    insert.bind(5, safe_input);
    insert.bind(6, fallback);
    insert.bind(7, rank);
    insert.bind(8, created_at_ms);
    insert.bind(9, expires_at_ms);
    insert.bind(10, session_id);
    insert.execute();
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteVoxNarrationRepository>
      repository;
  std::size_t id_sequence{100};
};

} // namespace

TEST_CASE("Vox narration cursor starts at the migration journal head",
          "[persistence][vox][narration][restart]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  auto database =
      sanguinius::persistence::Database::open_migration(temporary.path(), 25ms);
  const auto production = sanguinius::persistence::production_migrations();
  const sanguinius::persistence::Migrator version_thirteen{
      std::span<const sanguinius::persistence::Migration>{production.data(),
                                                          13},
      {"test-version", "test-revision"},
      clock};
  REQUIRE(version_thirteen.apply(database.connection()).current_version == 13);
  insert_scope(database.connection());
  insert_event(database.connection(), uuid(1), "tarot.wager_resolved.v1",
               uuid(2), 100);
  sanguinius::persistence::Migrator current{
      production, {"test-version", "test-revision"}, clock};
  REQUIRE(current.apply(database.connection()).current_version == 15);
  auto context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          std::move(database));
  sanguinius::persistence::SqliteVoxNarrationRepository repository{context};
  std::size_t sequence{10};

  CHECK(repository.observe_batch(
            {.now_ms = 1'000,
             .enabled = true,
             .test_mode = false,
             .limit = 32,
             .next_id = [&sequence] { return uuid(++sequence); }}) == 0);
  const auto health = repository.health();
  CHECK(health.cursor_rowid == 1);
  CHECK(health.journal_head_rowid == 1);
  CHECK(health.pending == 0);
}

TEST_CASE("Narration owner controls replay their exact durable response",
          "[persistence][vox][narration][idempotency]") {
  NarrationFixture fixture;
  const sanguinius::VoxNarrationControlContext preview{
      .idempotency_key = "vox:interaction:900",
      .operation = "narration_preview",
      .actor_user_id = "30",
      .guild_id = "10",
      .channel_id = "20",
      .request_fingerprint = uuid(1),
      .now_ms = 100};

  CHECK_FALSE(fixture.repository->control_receipt(preview));
  CHECK(fixture.repository->record_control_receipt(
            preview, "A safe preview.") == "A safe preview.");
  REQUIRE(fixture.repository->control_receipt(preview));
  CHECK(*fixture.repository->control_receipt(preview) == "A safe preview.");
  CHECK(fixture.repository->record_control_receipt(
            preview, "A changed preview.") == "A safe preview.");

  auto reused = preview;
  reused.request_fingerprint = uuid(2);
  CHECK_THROWS_AS(fixture.repository->control_receipt(reused),
                  std::invalid_argument);
  reused = preview;
  reused.operation = "narration_enqueue";
  CHECK_THROWS_AS(fixture.repository->control_receipt(reused),
                  std::invalid_argument);

  auto enqueue = preview;
  enqueue.idempotency_key = "vox:interaction:901";
  enqueue.operation = "narration_enqueue";
  CHECK(fixture.repository->record_control_receipt(
            enqueue, "Enqueue accepted.") == "Enqueue accepted.");
  auto count = fixture.context->connection().prepare(
      "SELECT count(*) FROM voice_interaction_receipt WHERE operation LIKE "
      "'narration_%'");
  REQUIRE(count.step());
  CHECK(count.column_int64(0) == 2);
}

TEST_CASE("owner enqueue durably observes exactly one referenced test event",
          "[persistence][vox][narration][control][idempotency]") {
  NarrationFixture fixture;
  const auto session_id = uuid(300);
  const auto event_id = uuid(301);
  const auto draw_id = uuid(302);
  const auto outbox_id = uuid(303);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), event_id, "tarot.draw_created.v1",
               draw_id, 100);
  fixture.context->connection().execute_script(
      "INSERT INTO tarot_catalog_snapshot(catalog_version,catalog_kind,"
      "canonical_json,checksum,installed_at_ms) VALUES("
      "'test-deck','deck','{}','checksum-1',1);"
      "INSERT INTO tarot_card_definition(catalog_version,ordinal,slug,name,"
      "meaning,theme_tag,safety_prompt,flavor_json) VALUES("
      "'test-deck',0,'the-angel','The Angel','A hopeful sign.','hope',"
      "'Keep the reading public.','[\"Hope endures.\"]');");
  auto draw = fixture.context->connection().prepare(
      "INSERT INTO tarot_card_draw(draw_id,user_id,guild_id,channel_id,"
      "visibility,catalog_version,card_ordinal,flavor_variant,drawn_at_ms,"
      "cooldown_until_ms,is_test,event_id) VALUES(?,'30','10','20','public',"
      "'test-deck',0,0,100,200,1,?)");
  draw.bind(1, draw_id);
  draw.bind(2, event_id);
  draw.execute();
  insert_public_outbox(fixture.context->connection(), outbox_id, draw_id,
                       event_id, 100, 'c');
  auto delivery = fixture.context->connection().prepare(
      "INSERT INTO tarot_draw_public_delivery(draw_id,outbox_id,created_at_ms) "
      "VALUES(?,?,100)");
  delivery.bind(1, draw_id);
  delivery.bind(2, outbox_id);
  delivery.execute();

  const auto preview = fixture.repository->preview(event_id, 110);
  REQUIRE(preview);
  CHECK(preview->source_event_id == event_id);
  CHECK(preview->is_test);

  const auto test_mode_disabled = fixture.repository->enqueue_reference(
      {.source_event_id = event_id,
       .now_ms = 110,
       .enabled = true,
       .test_mode = false,
       .next_id = fixture.ids()});
  CHECK(test_mode_disabled.status ==
        sanguinius::VoxNarrationEnqueueStatus::rejected);
  CHECK(test_mode_disabled.reason == "test_mode_disabled");

  const auto request =
      sanguinius::VoxNarrationEnqueueRequest{.source_event_id = event_id,
                                             .now_ms = 110,
                                             .enabled = true,
                                             .test_mode = true,
                                             .next_id = fixture.ids()};
  const sanguinius::VoxNarrationControlContext control{
      .idempotency_key = "vox:interaction:atomic-enqueue",
      .operation = "narration_enqueue",
      .actor_user_id = "30",
      .guild_id = "10",
      .channel_id = "20",
      .request_fingerprint = event_id,
      .now_ms = 110};
  fixture.context->connection().execute(
      "CREATE TEMP TRIGGER fail_narration_receipt BEFORE INSERT ON "
      "voice_interaction_receipt BEGIN SELECT RAISE(ABORT,'injected receipt "
      "failure'); END");
  CHECK_THROWS(
      fixture.repository->enqueue_reference_with_receipt(request, control));
  auto rolled_back = fixture.context->connection().prepare(
      "SELECT (SELECT count(*) FROM voice_narration_intent WHERE "
      "source_event_id=?),(SELECT count(*) FROM voice_interaction_receipt "
      "WHERE idempotency_key=?)");
  rolled_back.bind(1, event_id);
  rolled_back.bind(2, control.idempotency_key);
  REQUIRE(rolled_back.step());
  CHECK(rolled_back.column_int64(0) == 0);
  CHECK(rolled_back.column_int64(1) == 0);
  fixture.context->connection().execute("DROP TRIGGER fail_narration_receipt");

  const sanguinius::VoxNarrationEnqueueResult accepted{
      .status = sanguinius::VoxNarrationEnqueueStatus::accepted,
      .reason = "eligible"};
  const auto accepted_response =
      sanguinius::vox_narration_enqueue_response(accepted);
  CHECK(fixture.repository->enqueue_reference_with_receipt(request, control) ==
        accepted_response);
  CHECK(fixture.repository->enqueue_reference_with_receipt(request, control) ==
        accepted_response);
  const auto replay = fixture.repository->enqueue_reference(request);
  CHECK(replay.status == sanguinius::VoxNarrationEnqueueStatus::replay);
  CHECK(replay.reason == "already_observed");

  CHECK(fixture.repository->observe_batch({.now_ms = 111,
                                           .enabled = true,
                                           .test_mode = true,
                                           .limit = 32,
                                           .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT count(*),min(state),min(source_event_id) FROM "
      "voice_narration_intent");
  REQUIRE(intent.step());
  CHECK(intent.column_int64(0) == 1);
  CHECK(intent.column_text(1) == "pending");
  CHECK(intent.column_text(2) == event_id);
}

TEST_CASE("narration schema rejects skipped state-machine transitions",
          "[persistence][vox][narration][schema]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(330));
  insert_event(fixture.context->connection(), uuid(331),
               "chronicle.session_started.v1", uuid(332), 100);
  fixture.insert_pending_intent(uuid(333), uuid(331), uuid(330));
  auto invalid = fixture.context->connection().prepare(
      "UPDATE voice_narration_intent SET state='played',"
      "state_version=state_version+1,terminal_reason='marker_completed' WHERE "
      "intent_id=?");
  invalid.bind(1, uuid(333));
  CHECK_THROWS(invalid.execute());

  REQUIRE(fixture.repository->claim_next({.now_ms = 110,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(334),
                                          .transition_id = uuid(335),
                                          .lease_until_ms = 30'110,
                                          .test_mode = false}));
  auto invalid_renewal = fixture.context->connection().prepare(
      "UPDATE voice_narration_intent SET state_version=state_version+1,"
      "model_status='generated',lease_token=? WHERE intent_id=?");
  invalid_renewal.bind(1, uuid(336));
  invalid_renewal.bind(2, uuid(333));
  CHECK_THROWS(invalid_renewal.execute());
}

TEST_CASE("narration health scopes feature usage to the active Vox session",
          "[persistence][vox][narration][health]") {
  NarrationFixture fixture;
  const auto old_session = uuid(340);
  fixture.insert_ready_voice_session(old_session);
  insert_event(fixture.context->connection(), uuid(341),
               "chronicle.session_started.v1", uuid(342), 100);
  fixture.insert_pending_intent(uuid(343), uuid(341), old_session);
  REQUIRE(fixture.repository->claim_next({.now_ms = 110,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(344),
                                          .transition_id = uuid(345),
                                          .lease_until_ms = 30'110,
                                          .test_mode = false}));
  fixture.context->connection().execute(
      "UPDATE voice_session SET state='inactive',state_version=state_version+1,"
      "last_active_at_ms=120,ended_at_ms=120,end_reason='test_complete' WHERE "
      "session_id='60000000-0000-4000-8000-000000000340'");

  const auto active_session = uuid(346);
  fixture.insert_ready_voice_session(active_session);
  insert_event(fixture.context->connection(), uuid(347),
               "chronicle.session_started.v1", uuid(348), 130);
  fixture.insert_pending_intent(uuid(349), uuid(347), active_session,
                                "chronicle", "chronicle.session_started.v1", 60,
                                130, 120'130);
  REQUIRE(fixture.repository->claim_next({.now_ms = 140,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(350),
                                          .transition_id = uuid(351),
                                          .lease_until_ms = 30'140,
                                          .test_mode = false}));

  const auto health = fixture.repository->health();
  CHECK(health.generating == 2);
  CHECK(health.session_feature_count == 1);
}

TEST_CASE("disabled Vox narration advances and terminalizes observations",
          "[persistence][vox][narration][rollback]") {
  NarrationFixture fixture;
  insert_event(fixture.context->connection(), uuid(10),
               "tarot.wager_resolved.v1", uuid(11), 100,
               R"({"sealed":"SEALED_CANARY","balance":999})");

  CHECK(fixture.repository->observe_batch({.now_ms = 1'000,
                                           .enabled = false,
                                           .test_mode = false,
                                           .limit = 32,
                                           .next_id = fixture.ids()}) == 1);
  CHECK(fixture.repository->observe_batch({.now_ms = 1'001,
                                           .enabled = false,
                                           .test_mode = false,
                                           .limit = 32,
                                           .next_id = fixture.ids()}) == 0);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,safe_input,counterpart_outbox_id FROM "
      "voice_narration_intent");
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "feature_disabled");
  CHECK(intent.column_is_null(2));
  CHECK(intent.column_is_null(3));
  auto outbox = fixture.context->connection().prepare(
      "SELECT count(*) FROM outbox_message");
  REQUIRE(outbox.step());
  CHECK(outbox.column_int64(0) == 0);
}

TEST_CASE("disabled narration suppresses work already being generated",
          "[persistence][vox][narration][rollback][restart]") {
  NarrationFixture fixture;
  const auto session_id = uuid(40);
  const auto event_id = uuid(41);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), event_id,
               "chronicle.session_started.v1", uuid(42), 100);
  fixture.insert_pending_intent(uuid(43), event_id, session_id);
  REQUIRE(fixture.repository->claim_next({.now_ms = 200,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(44),
                                          .transition_id = uuid(45),
                                          .lease_until_ms = 30'200,
                                          .test_mode = false}));

  CHECK(fixture.repository->observe_batch({.now_ms = 201,
                                           .enabled = false,
                                           .test_mode = false,
                                           .limit = 32,
                                           .next_id = fixture.ids()}) == 1);
  auto state = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,lease_owner FROM voice_narration_intent "
      "WHERE intent_id=?");
  state.bind(1, uuid(43));
  REQUIRE(state.step());
  CHECK(state.column_text(0) == "suppressed");
  CHECK(state.column_text(1) == "feature_disabled");
  CHECK(state.column_is_null(2));
}

TEST_CASE("stale Chronicle observations do not create late state cards",
          "[persistence][vox][narration][outbox][restart][expiry]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(45));
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES(?,'10','20','30','open',90,1,'stale-chronicle-session')");
  chronicle.bind(1, uuid(46));
  chronicle.execute();
  insert_event(fixture.context->connection(), uuid(47),
               "chronicle.session_started.v1", uuid(46), 100);

  REQUIRE(fixture.repository->observe_batch({.now_ms = 120'100,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,counterpart_outbox_id FROM "
      "voice_narration_intent WHERE source_event_id=?");
  intent.bind(1, uuid(47));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "stale");
  CHECK(intent.column_is_null(2));
  auto cards = fixture.context->connection().prepare(
      "SELECT count(*) FROM outbox_message WHERE "
      "aggregate_type='voice_narration'");
  REQUIRE(cards.step());
  CHECK(cards.column_int64(0) == 0);
}

TEST_CASE("abandoned Chronicle starts never create narration state cards",
          "[persistence][vox][narration][chronicle][privacy]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(160));
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,closed_at_ms,revision,"
      "start_idempotency_key) VALUES(?,'10','20','30','abandoned',90,105,2,"
      "'abandoned-chronicle-session')");
  chronicle.bind(1, uuid(161));
  chronicle.execute();
  insert_event(fixture.context->connection(), uuid(162),
               "chronicle.session_started.v1", uuid(161), 100);

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,counterpart_outbox_id FROM "
      "voice_narration_intent WHERE source_event_id=?");
  intent.bind(1, uuid(162));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "chronicle_session_not_public");
  CHECK(intent.column_is_null(2));
  auto cards = fixture.context->connection().prepare(
      "SELECT count(*) FROM outbox_message WHERE "
      "aggregate_type='voice_narration'");
  REQUIRE(cards.step());
  CHECK(cards.column_int64(0) == 0);
}

TEST_CASE("events committed before a Vox session cannot bind to that session",
          "[persistence][vox][narration][session][causality]") {
  NarrationFixture fixture;
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES(?,'10','20','30','open',90,1,'pre-vox-session')");
  chronicle.bind(1, uuid(180));
  chronicle.execute();
  insert_event(fixture.context->connection(), uuid(181),
               "chronicle.session_started.v1", uuid(180), 100);
  fixture.insert_ready_voice_session(uuid(182));

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,session_id FROM voice_narration_intent "
      "WHERE source_event_id=?");
  intent.bind(1, uuid(181));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "disconnected");
  CHECK(intent.column_is_null(2));
}

TEST_CASE("public counterpart lookup requires exact event causation",
          "[persistence][vox][narration][outbox][causality]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(190));
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_title_definition(definition_id,title,description,"
      "provenance,proposed_by_user_id,created_at_ms) VALUES("
      "'60000000-0000-4000-8000-000000000191','Keeper','Public title',"
      "'owner_curated','30',90);"
      "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
      "recipient_user_id,state,featured,revision,source_idempotency_key,"
      "proposed_at_ms,decided_at_ms,decided_by_user_id) VALUES("
      "'60000000-0000-4000-8000-000000000192',"
      "'60000000-0000-4000-8000-000000000191','30','active',0,1,"
      "'title:test:exact',90,90,'30')");
  insert_event(fixture.context->connection(), uuid(193),
               "chronicle.title_awarded.v1", uuid(192), 100);
  insert_public_outbox(fixture.context->connection(), uuid(194), uuid(192),
                       uuid(193), 100, 'a');
  insert_public_outbox(fixture.context->connection(), uuid(195), uuid(192),
                       uuid(196), 101, 'b');

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT counterpart_outbox_id,state FROM voice_narration_intent WHERE "
      "source_event_id=?");
  intent.bind(1, uuid(193));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == uuid(194));
  CHECK(intent.column_text(1) == "pending");
}

TEST_CASE("ambiguous public counterpart causation fails closed",
          "[persistence][vox][narration][outbox][privacy]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(200));
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_title_definition(definition_id,title,description,"
      "provenance,proposed_by_user_id,created_at_ms) VALUES("
      "'60000000-0000-4000-8000-000000000201','Keeper','Public title',"
      "'owner_curated','30',90);"
      "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
      "recipient_user_id,state,featured,revision,source_idempotency_key,"
      "proposed_at_ms,decided_at_ms,decided_by_user_id) VALUES("
      "'60000000-0000-4000-8000-000000000202',"
      "'60000000-0000-4000-8000-000000000201','30','active',0,1,"
      "'title:test:ambiguous',90,90,'30')");
  insert_event(fixture.context->connection(), uuid(203),
               "chronicle.title_awarded.v1", uuid(202), 100);
  insert_public_outbox(fixture.context->connection(), uuid(204), uuid(202),
                       uuid(203), 100, 'e');
  insert_public_outbox(fixture.context->connection(), uuid(205), uuid(202),
                       uuid(203), 101, 'f');

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,counterpart_outbox_id FROM "
      "voice_narration_intent WHERE source_event_id=?");
  intent.bind(1, uuid(203));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "counterpart_ambiguous");
  CHECK(intent.column_is_null(2));
}

TEST_CASE("a revoked public title is suppressed after generation",
          "[persistence][vox][narration][chronicle][privacy]") {
  NarrationFixture fixture;
  const auto session_id = uuid(310);
  const auto grant_id = uuid(312);
  const auto event_id = uuid(313);
  const auto outbox_id = uuid(314);
  fixture.insert_ready_voice_session(session_id);
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_title_definition(definition_id,title,description,"
      "provenance,proposed_by_user_id,created_at_ms) VALUES("
      "'60000000-0000-4000-8000-000000000311','Keeper','Public title',"
      "'owner_curated','30',90);"
      "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
      "recipient_user_id,state,featured,revision,source_idempotency_key,"
      "proposed_at_ms,decided_at_ms,decided_by_user_id) VALUES("
      "'60000000-0000-4000-8000-000000000312',"
      "'60000000-0000-4000-8000-000000000311','30','active',0,1,"
      "'title:test:revocation',90,90,'30')");
  insert_event(fixture.context->connection(), event_id,
               "chronicle.title_awarded.v1", grant_id, 100);
  insert_public_outbox(fixture.context->connection(), outbox_id, grant_id,
                       event_id, 100, 'd');
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='delivered',provider_message_id='50',"
      "delivered_at_ms=105,terminal_at_ms=105,updated_at_ms=105 WHERE "
      "outbox_id='60000000-0000-4000-8000-000000000314'");
  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  const auto candidate =
      fixture.repository->claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(315),
                                      .transition_id = uuid(316),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
  REQUIRE(candidate);
  fixture.context->connection().execute(
      "UPDATE chronicle_title_grant SET state='revoked',featured=0,"
      "revision=revision+1,revoked_at_ms=111,revoked_by_user_id='30' WHERE "
      "grant_id='60000000-0000-4000-8000-000000000312'");
  constexpr std::string_view line{"Keeper is entered among the worthy."};
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::generated,
       .content_hash = sanguinius::sha256_hex(
           std::as_bytes(std::span{line.data(), line.size()})),
       .speech_id = uuid(317),
       .transition_id = uuid(318),
       .now_ms = 112});
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE source_event_id=?");
  intent.bind(1, event_id);
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "source_ineligible");
  CHECK(intent.column_is_null(2));
}

TEST_CASE("Vox narration gates exhausted TTS budget before generation",
          "[persistence][vox][narration][budget]") {
  NarrationFixture fixture;
  fixture.repository =
      std::make_unique<sanguinius::persistence::SqliteVoxNarrationRepository>(
          fixture.context,
          sanguinius::TtsUsagePolicy{.rolling_day_micro_usd = 100'000,
                                     .calendar_month_micro_usd = 2'000'000,
                                     .rolling_day_attempts = 1});
  fixture.insert_ready_voice_session(uuid(70));
  insert_event(fixture.context->connection(), uuid(71),
               "chronicle.session_started.v1", uuid(72), 100);
  fixture.insert_pending_intent(uuid(73), uuid(71), uuid(70));
  CHECK_FALSE(fixture.repository->automatic_speech_suppressed(110));
  CHECK_FALSE(fixture.repository->automatic_speech_admission_suppressed(110));
  insert_tts_attempt(fixture.context->connection(), 74, 100);

  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 110,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(76),
                                              .transition_id = uuid(77),
                                              .lease_until_ms = 30'110,
                                              .test_mode = false}));
  CHECK_FALSE(fixture.repository->automatic_speech_suppressed(110));
  CHECK(fixture.repository->automatic_speech_admission_suppressed(110));
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason FROM voice_narration_intent WHERE "
      "intent_id=?");
  intent.bind(1, uuid(73));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "provider_budget");
}

TEST_CASE("Vox narration revalidates TTS budget after generation",
          "[persistence][vox][narration][budget]") {
  NarrationFixture fixture;
  fixture.repository =
      std::make_unique<sanguinius::persistence::SqliteVoxNarrationRepository>(
          fixture.context,
          sanguinius::TtsUsagePolicy{.rolling_day_micro_usd = 100'000,
                                     .calendar_month_micro_usd = 2'000'000,
                                     .rolling_day_attempts = 1});
  fixture.insert_ready_voice_session(uuid(80));
  insert_event(fixture.context->connection(), uuid(81),
               "chronicle.session_started.v1", uuid(82), 100);
  fixture.insert_pending_intent(uuid(83), uuid(81), uuid(80));
  const auto candidate =
      fixture.repository->claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(84),
                                      .transition_id = uuid(85),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
  REQUIRE(candidate);
  insert_tts_attempt(fixture.context->connection(), 86, 111);
  constexpr std::string_view line{"The Chronicle opens for this gathering."};
  const auto bytes = std::as_bytes(std::span{line.data(), line.size()});
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(bytes),
       .speech_id = uuid(88),
       .transition_id = uuid(89),
       .now_ms = 111});

  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE intent_id=?");
  intent.bind(1, uuid(83));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "provider_budget");
  CHECK(intent.column_is_null(2));
}

TEST_CASE("Vox narration gates a full noncritical speech queue before AI",
          "[persistence][vox][narration][backpressure]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(130));
  insert_event(fixture.context->connection(), uuid(131),
               "chronicle.session_started.v1", uuid(132), 100);
  fixture.insert_pending_intent(uuid(133), uuid(131), uuid(130));
  for (std::size_t index = 0; index < 16; ++index)
    insert_queued_speech(fixture.context->connection(), uuid(130), 140 + index,
                         100 + static_cast<std::int64_t>(index));

  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 120,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(160),
                                              .transition_id = uuid(161),
                                              .lease_until_ms = 30'120,
                                              .test_mode = false}));
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason FROM voice_narration_intent WHERE "
      "intent_id=?");
  intent.bind(1, uuid(133));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "queue_full");
}

TEST_CASE("expired generation leases recover once and never outlive source TTL",
          "[persistence][vox][narration][restart][expiry]") {
  NarrationFixture fixture;
  const auto session_id = uuid(50);
  const auto event_id = uuid(51);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), event_id,
               "chronicle.session_started.v1", uuid(52), 100);
  fixture.insert_pending_intent(uuid(53), event_id, session_id);
  REQUIRE(fixture.repository->claim_next({.now_ms = 200,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(54),
                                          .transition_id = uuid(55),
                                          .lease_until_ms = 30'200,
                                          .test_mode = false}));
  CHECK(fixture.repository->reconcile(30'200, fixture.ids()) == 1);
  auto recovered = fixture.context->connection().prepare(
      "SELECT state,state_version,model_status FROM voice_narration_intent "
      "WHERE intent_id=?");
  recovered.bind(1, uuid(53));
  REQUIRE(recovered.step());
  CHECK(recovered.column_text(0) == "pending");
  CHECK(recovered.column_int64(1) == 3);
  CHECK(recovered.column_text(2) == "not_requested");

  REQUIRE(fixture.repository->claim_next({.now_ms = 30'201,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(56),
                                          .transition_id = uuid(57),
                                          .lease_until_ms = 60'201,
                                          .test_mode = false}));
  CHECK(fixture.repository->reconcile(120'100, fixture.ids()) == 1);
  auto expired = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE intent_id=?");
  expired.bind(1, uuid(53));
  REQUIRE(expired.step());
  CHECK(expired.column_text(0) == "expired");
  CHECK(expired.column_text(1) == "stale");
  CHECK(expired.column_is_null(2));
}

TEST_CASE("narration generation starts its lease after shared-worker delay",
          "[persistence][vox][narration][lease][ai][concurrency]") {
  NarrationFixture fixture;
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{1}});
  const auto session_id = uuid(580);
  const auto event_id = uuid(581);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), event_id,
               "chronicle.title_awarded.v1", uuid(582), 100);
  fixture.insert_pending_intent(uuid(583), event_id, session_id, "chronicle",
                                "chronicle.title_awarded.v1", 100);

  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeAiClient ai;
  ai.set_response(R"({"line":"A worthy name enters the Chronicle."})");
  ai.block();
  sanguinius::AiWorkService work{4, 1};
  work.start();
  std::promise<void> blocker_entered;
  auto blocker_entered_future = blocker_entered.get_future();
  std::promise<void> blocker_release;
  auto blocker_release_future = blocker_release.get_future().share();
  REQUIRE(work.submit([&blocker_entered, blocker_release_future](
                          const std::stop_token stop_token) mutable {
    blocker_entered.set_value();
    while (!stop_token.stop_requested() &&
           blocker_release_future.wait_for(5ms) != std::future_status::ready) {
    }
  }) == sanguinius::SubmitResult::accepted);
  REQUIRE(blocker_entered_future.wait_for(2s) == std::future_status::ready);
  sanguinius::VoxNarrationService service{*fixture.repository,
                                          fixture.clock,
                                          ids,
                                          diagnostics,
                                          ai,
                                          work,
                                          uuid(99),
                                          true,
                                          false};

  service.run_one_cycle();
  CHECK_FALSE(ai.wait_until_entered(20ms));
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{70}});
  service.run_one_cycle();
  auto dispatched = fixture.context->connection().prepare(
      "SELECT state,state_version,lease_until_ms FROM "
      "voice_narration_intent WHERE intent_id=?");
  dispatched.bind(1, uuid(583));
  REQUIRE(dispatched.step());
  CHECK(dispatched.column_text(0) == "generating");
  CHECK(dispatched.column_int64(1) == 2);
  CHECK(dispatched.column_int64(2) < 70'000);

  blocker_release.set_value();
  REQUIRE(ai.wait_until_entered(2s));
  auto started = fixture.context->connection().prepare(
      "SELECT state,state_version,lease_until_ms FROM "
      "voice_narration_intent WHERE intent_id=?");
  started.bind(1, uuid(583));
  REQUIRE(started.step());
  CHECK(started.column_text(0) == "generating");
  CHECK(started.column_int64(1) == 3);
  CHECK(started.column_int64(2) == 135'000);
  auto transition = fixture.context->connection().prepare(
      "SELECT from_state,to_state,reason,occurred_at_ms FROM "
      "voice_narration_transition WHERE intent_id=? AND "
      "reason='generation_started'");
  transition.bind(1, uuid(583));
  REQUIRE(transition.step());
  CHECK(transition.column_text(0) == "generating");
  CHECK(transition.column_text(1) == "generating");
  CHECK(transition.column_int64(3) == 70'000);

  ai.release();
  bool queued{};
  for (std::size_t attempt = 0; attempt < 200 && !queued; ++attempt) {
    queued = fixture.repository->health().queued == 1;
    if (!queued)
      std::this_thread::sleep_for(5ms);
  }
  CHECK(queued);
  CHECK(ai.requests().size() == 1);
  service.stop();
  work.stop();
}

TEST_CASE("Chronicle narration waits for its committed text counterpart",
          "[persistence][vox][narration][privacy]") {
  NarrationFixture fixture;
  const auto session_id = uuid(20);
  const auto chronicle_session_id = uuid(21);
  fixture.insert_ready_voice_session(session_id);
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES(?,'10','20','30','open',90,1,'chronicle-session-test')");
  chronicle.bind(1, chronicle_session_id);
  chronicle.execute();
  insert_event(
      fixture.context->connection(), uuid(22), "chronicle.session_started.v1",
      chronicle_session_id, 100,
      R"({"summary":"SUMMARY_CANARY","transcript":"TRANSCRIPT_CANARY","private_memory":"MEMORY_CANARY","relationship_dimensions":{"trust":99}})");

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 110,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(30),
                                              .transition_id = uuid(31),
                                              .lease_until_ms = 30'110,
                                              .test_mode = false}));

  auto counterpart = fixture.context->connection().prepare(
      "SELECT outbox_id,payload_json,state FROM outbox_message WHERE "
      "aggregate_type='voice_narration'");
  REQUIRE(counterpart.step());
  const auto counterpart_id = counterpart.column_text(0);
  CHECK(counterpart.column_text(2) == "pending");
  CHECK(counterpart.column_text(1).find("SUMMARY_CANARY") == std::string::npos);
  auto delivered = fixture.context->connection().prepare(
      "UPDATE outbox_message SET state='delivered',provider_message_id='50',"
      "delivered_at_ms=120,terminal_at_ms=120,updated_at_ms=120 WHERE "
      "outbox_id=?");
  delivered.bind(1, counterpart_id);
  delivered.execute();

  const auto candidate =
      fixture.repository->claim_next({.now_ms = 120,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(32),
                                      .transition_id = uuid(33),
                                      .lease_until_ms = 30'120,
                                      .test_mode = false});
  REQUIRE(candidate);
  CHECK(candidate->rank == 60);
  CHECK(candidate->safe_input == "The shared Chronicle session is open.");
  constexpr std::string_view line{"The Chronicle opens for this gathering."};
  const auto line_bytes = std::as_bytes(std::span{line.data(), line.size()});
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(line_bytes),
       .speech_id = uuid(34),
       .transition_id = uuid(35),
       .now_ms = 120});

  auto speech = fixture.context->connection().prepare(
      "SELECT text,priority,narration_rank,state,source_event_id FROM "
      "speech_item WHERE speech_id=?");
  speech.bind(1, uuid(34));
  REQUIRE(speech.step());
  CHECK(speech.column_text(0) == line);
  CHECK(speech.column_int64(1) == 200);
  CHECK(speech.column_int64(2) == 60);
  CHECK(speech.column_text(3) == "pending");
  CHECK(speech.column_text(4) == uuid(22));
  CHECK(speech.column_text(0).find("CANARY") == std::string::npos);
  const auto flavor_context =
      fixture.repository->session_flavor_context(session_id, "10", "30");
  REQUIRE(flavor_context);
  CHECK(flavor_context->find("Summoner display name: Owner.") !=
        std::string::npos);
  CHECK(flavor_context->find("CANARY") == std::string::npos);
  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 121,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(36),
                                              .transition_id = uuid(37),
                                              .lease_until_ms = 30'121,
                                              .test_mode = false}));

  fixture.context->connection().execute(
      "UPDATE appearance_control_state SET quiet_until_ms=1000,"
      "quiet_set_by_user_id='30',quiet_reason='duration',updated_at_ms=130 "
      "WHERE singleton=1");
  CHECK(fixture.repository->automatic_speech_suppressed(130));
  CHECK(fixture.repository->reconcile(130, fixture.ids()) == 1);
  auto cancelled = fixture.context->connection().prepare(
      "SELECT speech.state,speech.last_error_code,intent.state,"
      "intent.terminal_reason FROM speech_item speech JOIN "
      "voice_narration_intent intent ON intent.speech_id=speech.speech_id "
      "WHERE speech.speech_id=?");
  cancelled.bind(1, uuid(34));
  REQUIRE(cancelled.step());
  CHECK(cancelled.column_text(0) == "cancelled");
  CHECK(cancelled.column_text(1) == "narration_quiet");
  CHECK(cancelled.column_text(2) == "cancelled");
  CHECK(cancelled.column_text(3) == "quiet");

  fixture.context->connection().execute(
      "UPDATE voice_session SET state='muted',state_version=3,"
      "muted_at_ms=130 WHERE "
      "session_id='60000000-0000-4000-8000-000000000020'");
  CHECK_FALSE(
      fixture.repository->session_flavor_context(session_id, "10", "30"));
}

TEST_CASE("Chronicle start is revalidated after its session is abandoned",
          "[persistence][vox][narration][chronicle][privacy][race]") {
  NarrationFixture fixture;
  const auto voice_session_id = uuid(250);
  const auto chronicle_session_id = uuid(251);
  const auto event_id = uuid(252);
  fixture.insert_ready_voice_session(voice_session_id);
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES(?,'10','20','30','open',90,1,'chronicle-abandon-race')");
  chronicle.bind(1, chronicle_session_id);
  chronicle.execute();
  insert_event(fixture.context->connection(), event_id,
               "chronicle.session_started.v1", chronicle_session_id, 100);

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='delivered',provider_message_id='50',"
      "delivered_at_ms=111,terminal_at_ms=111,updated_at_ms=111 WHERE "
      "aggregate_type='voice_narration'");
  const auto candidate =
      fixture.repository->claim_next({.now_ms = 112,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(253),
                                      .transition_id = uuid(254),
                                      .lease_until_ms = 30'112,
                                      .test_mode = false});
  REQUIRE(candidate);
  auto abandon = fixture.context->connection().prepare(
      "UPDATE chronicle_session SET state='abandoned',closed_at_ms=113,"
      "revision=revision+1 WHERE session_id=?");
  abandon.bind(1, chronicle_session_id);
  abandon.execute();
  constexpr std::string_view line{"The Chronicle opens for this gathering."};
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(
           std::as_bytes(std::span{line.data(), line.size()})),
       .speech_id = uuid(255),
       .transition_id = uuid(256),
       .now_ms = 114});

  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE source_event_id=?");
  intent.bind(1, event_id);
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "source_ineligible");
  CHECK(intent.column_is_null(2));
}

TEST_CASE("Mute and unmute fence generation before the model starts",
          "[persistence][vox][narration][mute][race]") {
  NarrationFixture fixture;
  const auto voice_session_id = uuid(360);
  const auto chronicle_session_id = uuid(361);
  const auto event_id = uuid(362);
  fixture.insert_ready_voice_session(voice_session_id);
  insert_event(fixture.context->connection(), event_id,
               "chronicle.session_started.v1", chronicle_session_id, 100);
  fixture.insert_pending_intent(uuid(363), event_id, voice_session_id);

  const auto candidate =
      fixture.repository->claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(364),
                                      .transition_id = uuid(365),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
  REQUIRE(candidate);
  CHECK(candidate->mute_epoch == 0);

  sanguinius::persistence::SqliteVoxRepository vox{fixture.context};
  const sanguinius::VoxCommandContext mute_context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .interaction_idempotency_key = "narration:mute:on",
      .correlation_id = "narration-mute-race",
      .now_ms = 120};
  const auto muted = vox.command_mute(mute_context, false, std::nullopt,
                                      uuid(366), std::nullopt);
  REQUIRE(muted.code == sanguinius::VoxResultCode::accepted);
  REQUIRE(muted.session);
  REQUIRE(muted.session->state == sanguinius::VoxState::muted);

  auto unmute_context = mute_context;
  unmute_context.interaction_idempotency_key = "narration:mute:off";
  unmute_context.now_ms = 121;
  const auto unmuted = vox.command_mute(unmute_context, true, std::nullopt,
                                        uuid(367), std::nullopt);
  REQUIRE(unmuted.code == sanguinius::VoxResultCode::accepted);
  REQUIRE(unmuted.session);
  REQUIRE(unmuted.session->state == sanguinius::VoxState::ready);

  CHECK_FALSE(fixture.repository->begin_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .expected_mute_epoch = candidate->mute_epoch,
       .instance_id = uuid(99),
       .expected_lease_token = uuid(364),
       .lease_token = uuid(368),
       .transition_id = uuid(369),
       .now_ms = 122,
       .lease_until_ms = 30'122,
       .test_mode = false}));

  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE intent_id=?");
  intent.bind(1, candidate->intent_id);
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "muted");
  CHECK(intent.column_is_null(2));

  const auto fresh_event_id = uuid(370);
  insert_event(fixture.context->connection(), fresh_event_id,
               "tarot.wager_resolved.v1", uuid(371), 123);
  fixture.insert_pending_intent(uuid(372), fresh_event_id, voice_session_id,
                                "tarot", "tarot.wager_resolved.v1", 90, 123,
                                120'123);
  const auto fresh = fixture.repository->claim_next({.now_ms = 124,
                                                     .instance_id = uuid(99),
                                                     .lease_token = uuid(373),
                                                     .transition_id = uuid(374),
                                                     .lease_until_ms = 30'124,
                                                     .test_mode = false});
  REQUIRE(fresh);
  CHECK(fresh->mute_epoch == muted.session->revision);
  constexpr std::string_view fresh_line{"The Tarot speaks anew."};
  fixture.repository->complete_generation(
      {.intent_id = fresh->intent_id,
       .expected_revision = fresh->revision,
       .expected_mute_epoch = fresh->mute_epoch,
       .line = std::string{fresh_line},
       .model_status = sanguinius::VoxNarrationModelStatus::generated,
       .content_hash = sanguinius::sha256_hex(
           std::as_bytes(std::span{fresh_line.data(), fresh_line.size()})),
       .speech_id = uuid(375),
       .transition_id = uuid(376),
       .now_ms = 125});
  auto fresh_speech = fixture.context->connection().prepare(
      "SELECT state FROM speech_item WHERE speech_id=?");
  fresh_speech.bind(1, uuid(375));
  REQUIRE(fresh_speech.step());
  CHECK(fresh_speech.column_text(0) == "pending");
}

TEST_CASE("reconnecting Vox sessions defer rather than suppress generation",
          "[persistence][vox][narration][reconnect]") {
  NarrationFixture fixture;
  const auto session_id = uuid(260);
  const auto event_id = uuid(261);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), event_id,
               "chronicle.session_started.v1", uuid(262), 100);
  fixture.insert_pending_intent(uuid(263), event_id, session_id);
  const auto candidate =
      fixture.repository->claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(264),
                                      .transition_id = uuid(265),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
  REQUIRE(candidate);
  auto reconnect = fixture.context->connection().prepare(
      "UPDATE voice_session SET state='reconnecting',"
      "state_version=state_version+1,connection_generation=2,"
      "reconnect_count=1,last_active_at_ms=111 WHERE session_id=?");
  reconnect.bind(1, session_id);
  reconnect.execute();
  constexpr std::string_view line{"The Chronicle opens for this gathering."};
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(
           std::as_bytes(std::span{line.data(), line.size()})),
       .speech_id = uuid(266),
       .transition_id = uuid(267),
       .now_ms = 112});

  auto queued = fixture.context->connection().prepare(
      "SELECT intent.state,intent.terminal_reason,speech.state FROM "
      "voice_narration_intent intent JOIN speech_item speech ON "
      "speech.speech_id=intent.speech_id WHERE intent.intent_id=?");
  queued.bind(1, uuid(263));
  REQUIRE(queued.step());
  CHECK(queued.column_text(0) == "queued");
  CHECK(queued.column_is_null(1));
  CHECK(queued.column_text(2) == "pending");
}

TEST_CASE("A failed text counterpart permanently suppresses narration",
          "[persistence][vox][narration][outbox]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(40));
  auto chronicle = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
      "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
      "VALUES(?,'10','20','30','open',90,1,'failed-counterpart-test')");
  chronicle.bind(1, uuid(41));
  chronicle.execute();
  insert_event(fixture.context->connection(), uuid(42),
               "chronicle.session_started.v1", uuid(41), 100);

  REQUIRE(fixture.repository->observe_batch({.now_ms = 110,
                                             .enabled = true,
                                             .test_mode = false,
                                             .limit = 32,
                                             .next_id = fixture.ids()}) == 1);
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='failed',terminal_at_ms=120,"
      "updated_at_ms=120,last_error_code='discord_rejected' WHERE "
      "aggregate_type='voice_narration'");

  CHECK(fixture.repository->reconcile(120, fixture.ids()) == 1);
  auto intent = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id FROM voice_narration_intent "
      "WHERE source_event_id=?");
  intent.bind(1, uuid(42));
  REQUIRE(intent.step());
  CHECK(intent.column_text(0) == "suppressed");
  CHECK(intent.column_text(1) == "counterpart_failed");
  CHECK(intent.column_is_null(2));
  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 121,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(43),
                                              .transition_id = uuid(44),
                                              .lease_until_ms = 30'121,
                                              .test_mode = false}));
}

TEST_CASE("Narration admits the highest two distinct features per session",
          "[persistence][vox][narration][budget]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(60));
  insert_event(fixture.context->connection(), uuid(61),
               "chronicle.title_awarded.v1", uuid(61), 100);
  insert_event(fixture.context->connection(), uuid(62),
               "tarot.wager_resolved.v1", uuid(62), 101);
  insert_event(fixture.context->connection(), uuid(63),
               "appearance.live_queued.v1", uuid(63), 102);
  fixture.insert_pending_intent(uuid(64), uuid(61), uuid(60), "chronicle",
                                "chronicle.title_awarded.v1", 100);
  fixture.insert_pending_intent(uuid(65), uuid(62), uuid(60), "tarot",
                                "tarot.wager_resolved.v1", 90, 101, 120'101);
  fixture.insert_pending_intent(uuid(66), uuid(63), uuid(60), "appearance",
                                "appearance.live_queued.v1", 40, 102, 60'102);

  for (std::size_t index = 0; index < 2; ++index) {
    const auto candidate = fixture.repository->claim_next(
        {.now_ms = 110 + static_cast<std::int64_t>(index),
         .instance_id = uuid(99),
         .lease_token = uuid(70 + index * 4),
         .transition_id = uuid(71 + index * 4),
         .lease_until_ms = 30'110 + static_cast<std::int64_t>(index),
         .test_mode = false});
    REQUIRE(candidate);
    const std::string line = index == 0 ? "A title is borne with honor."
                                        : "The public wager is settled.";
    const auto bytes = std::as_bytes(std::span{line.data(), line.size()});
    fixture.repository->complete_generation(
        {.intent_id = candidate->intent_id,
         .expected_revision = candidate->revision,
         .line = line,
         .model_status = sanguinius::VoxNarrationModelStatus::fallback,
         .content_hash = sanguinius::sha256_hex(bytes),
         .speech_id = uuid(72 + index * 4),
         .transition_id = uuid(73 + index * 4),
         .now_ms = 110 + static_cast<std::int64_t>(index)});
  }
  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 113,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(80),
                                              .transition_id = uuid(81),
                                              .lease_until_ms = 30'113,
                                              .test_mode = false}));
  auto counts = fixture.context->connection().prepare(
      "SELECT (SELECT count(*) FROM speech_item WHERE source_kind="
      "'vox_feature_narration'),(SELECT terminal_reason FROM "
      "voice_narration_intent WHERE intent_id=?)");
  counts.bind(1, uuid(66));
  REQUIRE(counts.step());
  CHECK(counts.column_int64(0) == 2);
  CHECK(counts.column_text(1) == "session_budget");
}

TEST_CASE("played narration still consumes its slot after playback failure",
          "[persistence][vox][narration][budget][playback]") {
  NarrationFixture fixture;
  const auto session_id = uuid(270);
  fixture.insert_ready_voice_session(session_id);
  insert_event(fixture.context->connection(), uuid(271),
               "chronicle.session_started.v1", uuid(272), 100);
  insert_event(fixture.context->connection(), uuid(273),
               "tarot.wager_resolved.v1", uuid(274), 101);
  fixture.insert_pending_intent(uuid(275), uuid(271), session_id);
  fixture.insert_pending_intent(uuid(276), uuid(273), session_id, "tarot",
                                "tarot.wager_resolved.v1", 90, 101, 120'101);

  for (std::size_t index = 0; index < 2; ++index) {
    const auto candidate = fixture.repository->claim_next(
        {.now_ms = 110 + static_cast<std::int64_t>(index),
         .instance_id = uuid(99),
         .lease_token = uuid(277 + index * 4),
         .transition_id = uuid(278 + index * 4),
         .lease_until_ms = 30'110 + static_cast<std::int64_t>(index),
         .test_mode = false});
    REQUIRE(candidate);
    const std::string line = index == 0
                                 ? "The Chronicle opens for this gathering."
                                 : "The public wager is settled.";
    fixture.repository->complete_generation(
        {.intent_id = candidate->intent_id,
         .expected_revision = candidate->revision,
         .line = line,
         .model_status = sanguinius::VoxNarrationModelStatus::fallback,
         .content_hash = sanguinius::sha256_hex(
             std::as_bytes(std::span{line.data(), line.size()})),
         .speech_id = uuid(279 + index * 4),
         .transition_id = uuid(280 + index * 4),
         .now_ms = 110 + static_cast<std::int64_t>(index)});
  }

  sanguinius::persistence::SqliteSpeechRepository speech{fixture.context};
  const auto claimed =
      speech.claim_next(session_id, 120, uuid(288), "speech:budget:claim");
  REQUIRE(claimed);
  REQUIRE(claimed->source_event_id);
  REQUIRE(*claimed->source_event_id == uuid(273));
  REQUIRE(speech.transition({.speech_id = claimed->speech_id,
                             .expected_revision = claimed->revision,
                             .target = sanguinius::SpeechState::ready,
                             .transition_id = uuid(289),
                             .reason = "synthesis_ready",
                             .idempotency_key = "speech:budget:ready",
                             .occurred_at_ms = 121,
                             .provider_request_id = "provider-request",
                             .cache_key = std::string(64, 'a'),
                             .cache_checksum = std::string(64, 'b'),
                             .marker = std::nullopt,
                             .duration_ms = 100,
                             .error_code = std::nullopt}) ==
          sanguinius::SpeechMutationStatus::applied);
  const auto ready = speech.find(claimed->speech_id);
  REQUIRE(ready);
  REQUIRE(speech.transition({.speech_id = ready->speech_id,
                             .expected_revision = ready->revision,
                             .target = sanguinius::SpeechState::playing,
                             .transition_id = uuid(290),
                             .reason = "playback_started",
                             .idempotency_key = "speech:budget:playing",
                             .occurred_at_ms = 122,
                             .provider_request_id = std::nullopt,
                             .cache_key = std::nullopt,
                             .cache_checksum = std::nullopt,
                             .marker = "budget-marker",
                             .duration_ms = std::nullopt,
                             .error_code = std::nullopt}) ==
          sanguinius::SpeechMutationStatus::applied);
  const auto playing = speech.find(claimed->speech_id);
  REQUIRE(playing);
  REQUIRE(speech.transition({.speech_id = playing->speech_id,
                             .expected_revision = playing->revision,
                             .target = sanguinius::SpeechState::failed,
                             .transition_id = uuid(291),
                             .reason = "gateway_failed",
                             .idempotency_key = "speech:budget:failed",
                             .occurred_at_ms = 123,
                             .provider_request_id = std::nullopt,
                             .cache_key = std::nullopt,
                             .cache_checksum = std::nullopt,
                             .marker = std::nullopt,
                             .duration_ms = std::nullopt,
                             .error_code = "gateway_failed"}) ==
          sanguinius::SpeechMutationStatus::applied);
  REQUIRE(fixture.repository->reconcile(124, fixture.ids()) == 1);

  insert_event(fixture.context->connection(), uuid(292),
               "appearance.live_queued.v1", uuid(293), 125);
  fixture.insert_pending_intent(uuid(294), uuid(292), session_id, "appearance",
                                "appearance.live_queued.v1", 40, 125, 60'125);
  CHECK_FALSE(fixture.repository->claim_next({.now_ms = 126,
                                              .instance_id = uuid(99),
                                              .lease_token = uuid(295),
                                              .transition_id = uuid(296),
                                              .lease_until_ms = 30'126,
                                              .test_mode = false}));
  auto budget = fixture.context->connection().prepare(
      "SELECT state,terminal_reason FROM voice_narration_intent WHERE "
      "intent_id=?");
  budget.bind(1, uuid(294));
  REQUIRE(budget.step());
  CHECK(budget.column_text(0) == "suppressed");
  CHECK(budget.column_text(1) == "session_budget");
  CHECK(fixture.repository->health().session_feature_count == 2);
}

TEST_CASE("A later higher-ranked feature supersedes only pending speech",
          "[persistence][vox][narration][budget]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(90));
  insert_event(fixture.context->connection(), uuid(91),
               "chronicle.session_started.v1", uuid(91), 100);
  fixture.insert_pending_intent(uuid(92), uuid(91), uuid(90));
  const auto lower = fixture.repository->claim_next({.now_ms = 110,
                                                     .instance_id = uuid(99),
                                                     .lease_token = uuid(93),
                                                     .transition_id = uuid(94),
                                                     .lease_until_ms = 30'110,
                                                     .test_mode = false});
  REQUIRE(lower);
  const std::string low_line{"The Chronicle opens for this gathering."};
  const auto low_bytes =
      std::as_bytes(std::span{low_line.data(), low_line.size()});
  fixture.repository->complete_generation(
      {.intent_id = lower->intent_id,
       .expected_revision = lower->revision,
       .line = low_line,
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(low_bytes),
       .speech_id = uuid(95),
       .transition_id = uuid(96),
       .now_ms = 110});

  insert_event(fixture.context->connection(), uuid(97),
               "chronicle.title_awarded.v1", uuid(97), 111);
  fixture.insert_pending_intent(uuid(98), uuid(97), uuid(90), "chronicle",
                                "chronicle.title_awarded.v1", 100, 111,
                                120'111);
  const auto higher =
      fixture.repository->claim_next({.now_ms = 112,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(100),
                                      .transition_id = uuid(101),
                                      .lease_until_ms = 30'112,
                                      .test_mode = false});
  REQUIRE(higher);
  CHECK(higher->intent_id == uuid(98));
  auto superseded = fixture.context->connection().prepare(
      "SELECT speech.state,speech.last_error_code,intent.state,"
      "intent.terminal_reason FROM speech_item speech JOIN "
      "voice_narration_intent intent ON intent.speech_id=speech.speech_id "
      "WHERE speech.speech_id=?");
  superseded.bind(1, uuid(95));
  REQUIRE(superseded.step());
  CHECK(superseded.column_text(0) == "cancelled");
  CHECK(superseded.column_text(1) == "narration_superseded");
  CHECK(superseded.column_text(2) == "cancelled");
  CHECK(superseded.column_text(3) == "superseded");
}

TEST_CASE("higher-ranked narration supersedes unfinished AI generation",
          "[persistence][vox][narration][budget][supersession]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(200));
  insert_event(fixture.context->connection(), uuid(201),
               "chronicle.session_started.v1", uuid(201), 100);
  fixture.insert_pending_intent(uuid(202), uuid(201), uuid(200));
  REQUIRE(fixture.repository->claim_next({.now_ms = 110,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(203),
                                          .transition_id = uuid(204),
                                          .lease_until_ms = 30'110,
                                          .test_mode = false}));

  insert_event(fixture.context->connection(), uuid(205),
               "chronicle.title_awarded.v1", uuid(205), 111);
  fixture.insert_pending_intent(uuid(206), uuid(205), uuid(200), "chronicle",
                                "chronicle.title_awarded.v1", 100, 111,
                                120'111);
  const auto higher =
      fixture.repository->claim_next({.now_ms = 112,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(207),
                                      .transition_id = uuid(208),
                                      .lease_until_ms = 30'112,
                                      .test_mode = false});
  REQUIRE(higher);
  CHECK(higher->intent_id == uuid(206));

  auto lower = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,lease_owner FROM voice_narration_intent "
      "WHERE intent_id=?");
  lower.bind(1, uuid(202));
  REQUIRE(lower.step());
  CHECK(lower.column_text(0) == "cancelled");
  CHECK(lower.column_text(1) == "superseded");
  CHECK(lower.column_is_null(2));
}

TEST_CASE("highest two features replace a lower global generation slot",
          "[persistence][vox][narration][budget][supersession]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(210));
  insert_event(fixture.context->connection(), uuid(211),
               "appearance.live_queued.v1", uuid(211), 100);
  fixture.insert_pending_intent(uuid(212), uuid(211), uuid(210), "appearance",
                                "appearance.live_queued.v1", 40, 100, 60'100);
  REQUIRE(fixture.repository->claim_next({.now_ms = 110,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(213),
                                          .transition_id = uuid(214),
                                          .lease_until_ms = 30'110,
                                          .test_mode = false}));
  insert_event(fixture.context->connection(), uuid(215),
               "chronicle.session_started.v1", uuid(215), 101);
  fixture.insert_pending_intent(uuid(216), uuid(215), uuid(210), "chronicle",
                                "chronicle.session_started.v1", 60, 101,
                                120'101);
  REQUIRE(fixture.repository->claim_next({.now_ms = 111,
                                          .instance_id = uuid(99),
                                          .lease_token = uuid(217),
                                          .transition_id = uuid(218),
                                          .lease_until_ms = 30'111,
                                          .test_mode = false}));

  insert_event(fixture.context->connection(), uuid(219),
               "tarot.wager_resolved.v1", uuid(219), 112);
  fixture.insert_pending_intent(uuid(220), uuid(219), uuid(210), "tarot",
                                "tarot.wager_resolved.v1", 90, 112, 120'112);
  const auto higher =
      fixture.repository->claim_next({.now_ms = 113,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(221),
                                      .transition_id = uuid(222),
                                      .lease_until_ms = 30'113,
                                      .test_mode = false});
  REQUIRE(higher);
  CHECK(higher->intent_id == uuid(220));

  auto states = fixture.context->connection().prepare(
      "SELECT intent_id,state,terminal_reason FROM voice_narration_intent "
      "WHERE intent_id IN (?,?,?) ORDER BY intent_id");
  states.bind(1, uuid(212));
  states.bind(2, uuid(216));
  states.bind(3, uuid(220));
  REQUIRE(states.step());
  CHECK(states.column_text(0) == uuid(212));
  CHECK(states.column_text(1) == "cancelled");
  CHECK(states.column_text(2) == "superseded");
  REQUIRE(states.step());
  CHECK(states.column_text(1) == "generating");
  REQUIRE(states.step());
  CHECK(states.column_text(1) == "generating");
}

TEST_CASE("Competing narration workers create one speech item",
          "[persistence][vox][narration][concurrency]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(110));
  insert_event(fixture.context->connection(), uuid(111),
               "chronicle.session_started.v1", uuid(111), 100);
  fixture.insert_pending_intent(uuid(112), uuid(111), uuid(110));
  std::barrier claim_gate{3};
  std::optional<sanguinius::VoxNarrationCandidate> first_candidate;
  std::optional<sanguinius::VoxNarrationCandidate> second_candidate;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  const auto claim = [&](const std::size_t base,
                         std::optional<sanguinius::VoxNarrationCandidate>
                             &result,
                         std::exception_ptr &error) {
    try {
      auto context =
          std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
              sanguinius::persistence::Database::open_runtime(
                  fixture.temporary.path(), 2s));
      sanguinius::persistence::SqliteVoxNarrationRepository repository{context};
      claim_gate.arrive_and_wait();
      result = repository.claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(base),
                                      .transition_id = uuid(base + 1),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
    } catch (...) {
      error = std::current_exception();
    }
  };
  std::thread first{claim, 113, std::ref(first_candidate),
                    std::ref(first_error)};
  std::thread second{claim, 115, std::ref(second_candidate),
                     std::ref(second_error)};
  claim_gate.arrive_and_wait();
  first.join();
  second.join();
  if (first_error)
    std::rethrow_exception(first_error);
  if (second_error)
    std::rethrow_exception(second_error);
  CHECK(first_candidate.has_value() != second_candidate.has_value());
  const auto candidate = first_candidate ? *first_candidate : *second_candidate;

  std::barrier completion_gate{3};
  const std::string line{"The Chronicle opens for this gathering."};
  const auto bytes = std::as_bytes(std::span{line.data(), line.size()});
  first_error = nullptr;
  second_error = nullptr;
  const auto complete = [&](const std::size_t base, std::exception_ptr &error) {
    try {
      auto context =
          std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
              sanguinius::persistence::Database::open_runtime(
                  fixture.temporary.path(), 2s));
      sanguinius::persistence::SqliteVoxNarrationRepository repository{context};
      completion_gate.arrive_and_wait();
      repository.complete_generation(
          {.intent_id = candidate.intent_id,
           .expected_revision = candidate.revision,
           .line = line,
           .model_status = sanguinius::VoxNarrationModelStatus::fallback,
           .content_hash = sanguinius::sha256_hex(bytes),
           .speech_id = uuid(base),
           .transition_id = uuid(base + 1),
           .now_ms = 111});
    } catch (...) {
      error = std::current_exception();
    }
  };
  std::thread first_completion{complete, 117, std::ref(first_error)};
  std::thread second_completion{complete, 119, std::ref(second_error)};
  completion_gate.arrive_and_wait();
  first_completion.join();
  second_completion.join();
  if (first_error)
    std::rethrow_exception(first_error);
  if (second_error)
    std::rethrow_exception(second_error);
  auto count = fixture.context->connection().prepare(
      "SELECT count(*) FROM speech_item WHERE source_event_id=?");
  count.bind(1, uuid(111));
  REQUIRE(count.step());
  CHECK(count.column_int64(0) == 1);
}

TEST_CASE("narration audit survives retained speech-row purging",
          "[persistence][vox][narration][speech][purge]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(230));
  insert_event(fixture.context->connection(), uuid(231),
               "chronicle.session_started.v1", uuid(231), 100);
  fixture.insert_pending_intent(uuid(232), uuid(231), uuid(230));
  const auto candidate =
      fixture.repository->claim_next({.now_ms = 110,
                                      .instance_id = uuid(99),
                                      .lease_token = uuid(233),
                                      .transition_id = uuid(234),
                                      .lease_until_ms = 30'110,
                                      .test_mode = false});
  REQUIRE(candidate);
  constexpr std::string_view line{"The Chronicle opens for this gathering."};
  const auto bytes = std::as_bytes(std::span{line.data(), line.size()});
  fixture.repository->complete_generation(
      {.intent_id = candidate->intent_id,
       .expected_revision = candidate->revision,
       .line = std::string{line},
       .model_status = sanguinius::VoxNarrationModelStatus::fallback,
       .content_hash = sanguinius::sha256_hex(bytes),
       .speech_id = uuid(235),
       .transition_id = uuid(236),
       .now_ms = 110});

  sanguinius::persistence::SqliteSpeechRepository speech{fixture.context};
  REQUIRE(speech.recover(600, "restart_abandoned") == 1);
  const auto after_retention = 601 + 31LL * 24 * 60 * 60 * 1'000;
  REQUIRE(speech.purge_retained(after_retention) == 0);

  auto retained = fixture.context->connection().prepare(
      "SELECT intent.state,(SELECT count(*) FROM speech_item WHERE "
      "speech_id=?) FROM voice_narration_intent intent WHERE intent_id=?");
  retained.bind(1, uuid(235));
  retained.bind(2, uuid(232));
  REQUIRE(retained.step());
  CHECK(retained.column_text(0) == "queued");
  CHECK(retained.column_int64(1) == 1);

  REQUIRE(fixture.repository->reconcile(after_retention, fixture.ids()) == 1);
  REQUIRE(speech.purge_retained(after_retention) == 1);

  auto audit = fixture.context->connection().prepare(
      "SELECT state,terminal_reason,speech_id,(SELECT count(*) FROM "
      "speech_item WHERE speech_id=?) FROM voice_narration_intent WHERE "
      "intent_id=?");
  audit.bind(1, uuid(235));
  audit.bind(2, uuid(232));
  REQUIRE(audit.step());
  CHECK(audit.column_text(0) == "cancelled");
  CHECK(audit.column_text(1) == "restart_abandoned");
  CHECK(audit.column_text(2) == uuid(235));
  CHECK(audit.column_int64(3) == 0);
}

TEST_CASE("narration insertion guards reject mismatched and undelivered links",
          "[persistence][vox][narration][schema][privacy]") {
  NarrationFixture fixture;
  fixture.insert_ready_voice_session(uuid(240));
  insert_event(fixture.context->connection(), uuid(241),
               "chronicle.session_started.v1", uuid(241), 100);
  CHECK_THROWS(fixture.insert_pending_intent(uuid(242), uuid(241), uuid(240),
                                             "chronicle",
                                             "chronicle.session_completed.v1"));

  insert_public_outbox(fixture.context->connection(), uuid(243), uuid(241),
                       uuid(241), 100, 'c');
  auto speech = fixture.context->connection().prepare(
      "INSERT INTO speech_item(speech_id,voice_session_id,source_event_id,"
      "source_kind,text,text_hash,scalar_count,provider,model,voice_id,"
      "priority,"
      "narration_rank,state,state_version,earliest_at_ms,expires_at_ms,"
      "interruptible,deduplication_key,created_at_ms) VALUES(?,?,?,"
      "'vox_feature_narration','safe',?,4,'openai','tts-1','onyx',200,60,"
      "'pending',1,100,120100,1,'speech:schema:queued',100)");
  speech.bind(1, uuid(244));
  speech.bind(2, uuid(240));
  speech.bind(3, uuid(241));
  speech.bind(4, std::string(64, 'd'));
  speech.execute();
  auto queued = fixture.context->connection().prepare(
      "INSERT INTO voice_narration_intent(intent_id,source_event_id,slot,"
      "feature,event_type,guild_id,channel_id,safe_input,fallback_line,"
      "narration_rank,created_at_ms,expires_at_ms,session_id,"
      "counterpart_outbox_id,counterpart_required,model_status,speech_id,"
      "is_test,state,state_version) VALUES(?,?,'feature','chronicle',"
      "'chronicle.session_started.v1','10','20','safe','safe',60,100,120100,"
      "?,?,1,'generated',?,0,'queued',1)");
  queued.bind(1, uuid(245));
  queued.bind(2, uuid(241));
  queued.bind(3, uuid(240));
  queued.bind(4, uuid(243));
  queued.bind(5, uuid(244));
  CHECK_THROWS(queued.execute());
}
