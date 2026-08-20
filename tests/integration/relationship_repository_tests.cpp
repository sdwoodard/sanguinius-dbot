#include "sanguinius/chronicle.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_persistent_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

[[nodiscard]] std::int64_t
scalar(sanguinius::persistence::SqliteRepositoryContext &context,
       const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_int64(0);
}

class RelationshipFixture {
public:
  struct ChronicleContextIds {
    std::string session_id;
    std::string summary_entry_id;
    std::string definition_id;
    std::string grant_id;
  };

  RelationshipFixture() {
    {
      auto database = sanguinius::persistence::Database::open_migration(
          temporary.path(), 25ms);
      const sanguinius::persistence::Migrator migrator{
          sanguinius::persistence::production_migrations(),
          {"test", "revision"},
          clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 7);
    }
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            sanguinius::persistence::Database::open_runtime(temporary.path(),
                                                            25ms));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
    identities.ensure_user({31, "Member", "member", false, 100});
    identities.ensure_user({32, "Other", "other", false, 100});
    context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=1 WHERE user_id IN "
        "('31','32')");
    sanguinius::persistence::SqliteApplicationInstanceRepository instances{
        context};
    instances.record_start({.instance_id = uuid(1),
                            .application_version = "test",
                            .git_revision = "revision",
                            .hostname = "test-host",
                            .process_id = 1,
                            .started_at_ms = 100});
    chronicle =
        std::make_unique<sanguinius::persistence::SqliteChronicleRepository>(
            context);
    relationships =
        std::make_unique<sanguinius::persistence::SqliteRelationshipRepository>(
            context);
  }

  void confirm_memory(
      const std::size_t base, const std::string &text,
      std::vector<std::string> tags,
      const sanguinius::DiscordSnowflake user = 31,
      const std::optional<std::int64_t> expires_at_ms = std::nullopt) {
    const auto result = chronicle->confirm_memory(
        {.memory_id = uuid(base),
         .event_id = uuid(base + 1),
         .expiry_job_id = expires_at_ms
                              ? std::optional<std::string>{uuid(base + 2)}
                              : std::nullopt,
         .draft = {.text = text,
                   .tags = std::move(tags),
                   .visibility = sanguinius::MemoryVisibility::shared,
                   .sensitivity = sanguinius::MemorySensitivity::ordinary,
                   .expires_at_ms = expires_at_ms,
                   .guild_id = 10,
                   .channel_id = 20,
                   .user_id = user},
         .correlation_id = "memory-correlation",
         .interaction_idempotency_key = "memory:" + std::to_string(base),
         .now_ms = 1'000});
    REQUIRE(result.code == sanguinius::ChronicleResultCode::created);
  }

  [[nodiscard]] sanguinius::PreparePromptContextRequest
  prompt(const std::size_t base, std::string current,
         const std::int64_t now_ms = 2'000) const {
    return {.attempt_id = uuid(base),
            .application_instance_id = uuid(1),
            .requester_user_id = 31,
            .requester_username = "member",
            .requester_display_name = "Member",
            .guild_id = 10,
            .channel_id = 20,
            .source_message_id = sanguinius::DiscordSnowflake{base},
            .current_request = std::move(current),
            .replied_text = {},
            .correlation_id = "prompt-correlation",
            .now_ms = now_ms};
  }

  [[nodiscard]] ChronicleContextIds install_chronicle_context() {
    ChronicleContextIds ids{.session_id = uuid(900),
                            .summary_entry_id = uuid(901),
                            .definition_id = uuid(902),
                            .grant_id = uuid(903)};
    auto session = context->connection().prepare(
        "INSERT INTO chronicle_session(session_id,guild_id,channel_id,"
        "opened_by_user_id,state,opened_at_ms,revision,start_idempotency_key) "
        "VALUES (?,'10','20','31','open',100,1,'prompt:test:session')");
    session.bind(1, ids.session_id);
    session.execute();
    auto summary = context->connection().prepare(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
        "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_"
        "ms,"
        "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
        "source_message_id,source_author_user_id,source_text,"
        "source_text_truncated,source_attachment_count,revision,source_kind) "
        "VALUES (?,'session_summary','The sealed hour','A shared approved "
        "chapter.','shared','canon',110,110,'30',110,110,'30','10','20',NULL,"
        "'30','',0,0,1,'session_summary')");
    summary.bind(1, ids.summary_entry_id);
    summary.execute();
    auto participant = context->connection().prepare(
        "INSERT INTO chronicle_participant(entry_id,user_id,role) "
        "VALUES (?,'31','session_participant')");
    participant.bind(1, ids.summary_entry_id);
    participant.execute();
    auto definition = context->connection().prepare(
        "INSERT INTO "
        "chronicle_title_definition(definition_id,title,description,"
        "provenance,session_id,supporting_entry_id,proposed_by_user_id,created_"
        "at_ms) "
        "VALUES (?,'Keeper of the Hour','For steadfast attendance.',"
        "'owner_curated',NULL,NULL,'30',120)");
    definition.bind(1, ids.definition_id);
    definition.execute();
    auto grant = context->connection().prepare(
        "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
        "recipient_user_id,state,featured,revision,source_idempotency_key,"
        "proposed_at_ms,decided_at_ms,decided_by_user_id) "
        "VALUES (?,?,'31','active',1,2,'prompt:test:title',120,121,'30')");
    grant.bind(1, ids.grant_id);
    grant.bind(2, ids.definition_id);
    grant.execute();
    return ids;
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteChronicleRepository> chronicle;
  std::unique_ptr<sanguinius::persistence::SqliteRelationshipRepository>
      relationships;
};

} // namespace

TEST_CASE("prompt attempts reserve relevant requester memories and finalize "
          "atomically",
          "[relationship][sqlite][memory][idempotency]") {
  RelationshipFixture fixture;
  fixture.confirm_memory(100, "The crimson dragon guards the old tower.",
                         {"dragon", "tower"});
  fixture.confirm_memory(110, "A peaceful garden blooms in spring.",
                         {"garden"});
  fixture.confirm_memory(120, "OTHER_USER_PRIVATE_SENTINEL dragon", {"dragon"},
                         32);
  fixture.confirm_memory(140, "EXPIRED_SENTINEL dragon", {"dragon"}, 31, 1'500);
  fixture.confirm_memory(150, "RETRACTED_SENTINEL dragon", {"dragon"});
  fixture.context->connection().execute(
      "UPDATE memory SET status='retracted',retracted_at_ms=1000,"
      "revision=revision+1 WHERE "
      "memory_id='" +
      uuid(150) + "'");
  REQUIRE(
      fixture.chronicle
          ->confirm_memory(
              {.memory_id = uuid(130),
               .event_id = uuid(131),
               .draft = {.text = "SENSITIVE_PRIVATE_SENTINEL dragon",
                         .tags = {"dragon"},
                         .visibility = sanguinius::MemoryVisibility::self_only,
                         .sensitivity =
                             sanguinius::MemorySensitivity::sensitive,
                         .guild_id = 10,
                         .channel_id = 20,
                         .user_id = 31},
               .correlation_id = "memory-correlation",
               .interaction_idempotency_key = "memory:130",
               .now_ms = 1'000})
          .code == sanguinius::ChronicleResultCode::created);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM memory_subject WHERE "
                                   "memory_id='" +
                                       uuid(100) +
                                       "' AND subject_type='topic'") == 2);
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=1,updated_at_ms=150 "
      "WHERE user_id='31'");

  const auto first_request =
      fixture.prompt(200, "What do we know of the dragon?");
  const auto prepared =
      fixture.relationships->prepare_prompt_context(first_request);
  REQUIRE(prepared.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared.attempt_id == uuid(200));
  REQUIRE(prepared.memories.size() == 1);
  REQUIRE(prepared.memories[0].memory.memory_id == uuid(100));
  REQUIRE(prepared.memories[0].memory.text.find("crimson") !=
          std::string::npos);
  REQUIRE(prepared.memories[0].memory.text.find("PRIVATE_SENTINEL") ==
          std::string::npos);
  REQUIRE(prepared.memories[0].memory.text.find("EXPIRED_SENTINEL") ==
          std::string::npos);
  REQUIRE(prepared.memories[0].memory.text.find("RETRACTED_SENTINEL") ==
          std::string::npos);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE ai_prompt_attempt_memory SET rank_position=2"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM ai_prompt_attempt_memory"));

  const auto reserved = fixture.relationships->prepare_prompt_context(
      fixture.prompt(205, "dragon", 2'001));
  REQUIRE(reserved.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(reserved.memories.empty());
  REQUIRE(
      fixture.relationships->fail_prompt_attempt({.attempt_id = uuid(205),
                                                  .outcome = "cancelled",
                                                  .error_code = "test_cleanup",
                                                  .now_ms = 2'002}) ==
      sanguinius::PromptFinalizationStatus::applied);

  auto duplicate_request = first_request;
  duplicate_request.attempt_id = uuid(201);
  duplicate_request.now_ms = 2'001;
  const auto duplicate =
      fixture.relationships->prepare_prompt_context(duplicate_request);
  REQUIRE(duplicate.status == sanguinius::PromptPreparationStatus::duplicate);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt WHERE "
                 "source_message_id='200'") == 1);

  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(200),
               .source_event_id = uuid(202),
               .relationship_event_id = uuid(203),
               .now_ms = 2'100}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(100) +
                     "'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          1);

  constexpr std::int64_t one_hour_ms = 60LL * 60 * 1'000;
  const auto second = fixture.relationships->prepare_prompt_context(
      fixture.prompt(210, "dragon", 2'100 + one_hour_ms));
  REQUIRE(second.memories.empty());
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(210),
               .source_event_id = uuid(211),
               .relationship_event_id = uuid(212),
               .now_ms = 2'101 + one_hour_ms}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT interaction_count FROM relationship_state WHERE "
                 "subject_user_id='31'") == 2);

  constexpr std::int64_t one_day_ms = 24LL * 60 * 60 * 1'000;
  const auto boundary = fixture.relationships->prepare_prompt_context(
      fixture.prompt(220, "dragon", 2'100 + one_day_ms));
  REQUIRE(boundary.memories.empty());
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(220),
               .source_event_id = uuid(221),
               .relationship_event_id = uuid(222),
               .now_ms = 2'100 + one_day_ms}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 2);

  constexpr std::int64_t seven_days_ms = 7LL * 24 * 60 * 60 * 1'000;
  const auto memory_boundary = fixture.relationships->prepare_prompt_context(
      fixture.prompt(230, "dragon", 2'100 + seven_days_ms));
  REQUIRE(memory_boundary.memories.size() == 1);
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(230),
               .source_event_id = uuid(231),
               .relationship_event_id = uuid(232),
               .now_ms = 2'100 + seven_days_ms}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(100) +
                     "'") == 2);
}

TEST_CASE("failed and privacy-invalidated attempts consume no social state",
          "[relationship][sqlite][privacy][failure]") {
  RelationshipFixture fixture;
  fixture.confirm_memory(300, "The silver phoenix returns at dawn.",
                         {"phoenix"});
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=1,updated_at_ms=150 "
      "WHERE user_id='31'");

  const auto failed = fixture.relationships->prepare_prompt_context(
      fixture.prompt(310, "phoenix"));
  REQUIRE(failed.memories.size() == 1);
  REQUIRE(fixture.relationships->fail_prompt_attempt({.attempt_id = uuid(310),
                                                      .outcome = "model_failed",
                                                      .error_code = "injected",
                                                      .now_ms = 2'100}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(300) +
                     "'") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          0);

  const auto prepared = fixture.relationships->prepare_prompt_context(
      fixture.prompt(320, "phoenix", 2'200));
  REQUIRE(prepared.memories.size() == 1);
  fixture.context->connection().execute(
      "UPDATE memory SET revision=revision+1 WHERE memory_id='" + uuid(300) +
      "'");
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(320),
               .source_event_id = uuid(321),
               .relationship_event_id = uuid(322),
               .now_ms = 2'250}) ==
          sanguinius::PromptFinalizationStatus::invalidated);

  const auto opted_out = fixture.relationships->prepare_prompt_context(
      fixture.prompt(330, "phoenix", 2'260));
  REQUIRE(opted_out.memories.size() == 1);
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=0,updated_at_ms=2201 "
      "WHERE user_id='31'");
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(330),
               .source_event_id = uuid(331),
               .relationship_event_id = uuid(332),
               .now_ms = 2'300}) ==
          sanguinius::PromptFinalizationStatus::invalidated);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(300) +
                     "'") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt WHERE "
                 "state='privacy_invalidated'") == 2);
}

TEST_CASE("prompt completion revalidates Chronicle continuity snapshots",
          "[relationship][sqlite][chronicle][privacy]") {
  RelationshipFixture fixture;
  const auto ids = fixture.install_chronicle_context();
  const auto prepared = fixture.relationships->prepare_prompt_context(
      fixture.prompt(950, "Recall our current Chronicle", 2'000));
  REQUIRE(prepared.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared.featured_title == "Keeper of the Hour");
  REQUIRE(prepared.latest_session_summary ==
          "The sealed hour — A shared approved chapter.");
  REQUIRE(prepared.session_open);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt_chronicle_context "
                 "WHERE attempt_id='" +
                     uuid(950) + "'") == 1);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE ai_prompt_attempt_chronicle_context SET "
      "open_session_revision=2"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM ai_prompt_attempt_chronicle_context"));

  SECTION("Chronicle opt-out invalidates a reply even without memories") {
    fixture.context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=0,updated_at_ms=2001 "
        "WHERE user_id='31'");
  }
  SECTION("featured title revocation invalidates the prepared reply") {
    fixture.context->connection().execute(
        "UPDATE chronicle_title_grant SET state='revoked',featured=0,"
        "revoked_at_ms=2001,revoked_by_user_id='31',revision=revision+1 "
        "WHERE grant_id='" +
        ids.grant_id + "'");
  }
  SECTION("summary retraction invalidates the prepared reply") {
    fixture.context->connection().execute(
        "UPDATE chronicle_entry SET status='retracted',retracted_at_ms=2001,"
        "retracted_by_user_id='31',revision=revision+1 WHERE entry_id='" +
        ids.summary_entry_id + "'");
  }
  SECTION("session closure invalidates the prepared open-state reply") {
    fixture.context->connection().execute(
        "UPDATE chronicle_session SET state='closed',closed_at_ms=2001,"
        "revision=revision+1 WHERE session_id='" +
        ids.session_id + "'");
  }

  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(950),
               .source_event_id = uuid(951),
               .relationship_event_id = uuid(952),
               .now_ms = 2'100}) ==
          sanguinius::PromptFinalizationStatus::invalidated);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt WHERE "
                 "attempt_id='" +
                     uuid(950) + "' AND state='privacy_invalidated'") == 1);
}

TEST_CASE("prompt terminal transitions tolerate wall-clock rollback",
          "[relationship][sqlite][clock][restart]") {
  RelationshipFixture fixture;
  const auto failed = fixture.relationships->prepare_prompt_context(
      fixture.prompt(340, "A pending request", 2'000));
  REQUIRE(failed.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(
      fixture.relationships->fail_prompt_attempt({.attempt_id = uuid(340),
                                                  .outcome = "model_failed",
                                                  .error_code = "clock_test",
                                                  .now_ms = 1'999}) ==
      sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT completed_at_ms FROM ai_prompt_attempt WHERE "
                 "attempt_id='" +
                     uuid(340) + "'") == 2'000);

  const auto completed = fixture.relationships->prepare_prompt_context(
      fixture.prompt(350, "A successful request", 3'000));
  REQUIRE(completed.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(350),
               .source_event_id = uuid(351),
               .relationship_event_id = uuid(352),
               .now_ms = 2'999}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT completed_at_ms FROM ai_prompt_attempt WHERE "
                 "attempt_id='" +
                     uuid(350) + "'") == 3'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT occurred_at_ms FROM relationship_event WHERE "
                 "relationship_event_id='" +
                     uuid(352) + "'") == 3'000);

  const auto abandoned = fixture.relationships->prepare_prompt_context(
      fixture.prompt(360, "An interrupted request", 4'000));
  REQUIRE(abandoned.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(fixture.relationships->recover_prompt_attempts(uuid(2), 3'999) == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT completed_at_ms FROM ai_prompt_attempt WHERE "
                 "attempt_id='" +
                     uuid(360) + "'") == 4'000);
}

TEST_CASE("successful memory use tolerates rollback before confirmation time",
          "[relationship][sqlite][memory][clock]") {
  RelationshipFixture fixture;
  fixture.confirm_memory(370, "The golden dragon remembers the gathering.",
                         {"dragon"});
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=1,updated_at_ms=150 "
      "WHERE user_id='31'");

  const auto prepared = fixture.relationships->prepare_prompt_context(
      fixture.prompt(380, "Tell me about the dragon", 500));
  REQUIRE(prepared.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared.memories.size() == 1);
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(380),
               .source_event_id = uuid(381),
               .relationship_event_id = uuid(382),
               .now_ms = 600}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT last_used_at_ms FROM memory WHERE memory_id='" +
                     uuid(370) + "'") == 1'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(370) +
                     "'") == 1);
  REQUIRE(
      scalar(*fixture.context, "SELECT count(*) FROM ai_prompt_attempt WHERE "
                               "attempt_id='" +
                                   uuid(380) + "' AND state='succeeded'") == 1);
}

TEST_CASE(
    "opted-out in-scope mentions remain source-idempotent without social state",
    "[relationship][sqlite][privacy][idempotency]") {
  RelationshipFixture fixture;
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=0,"
      "memory_callback_opt_in=0,updated_at_ms=200 WHERE user_id='32'");
  auto request = fixture.prompt(350, "A plain opted-out request");
  request.requester_user_id = 32;
  request.requester_username = "other";
  request.requester_display_name = "Other";

  const auto prepared = fixture.relationships->prepare_prompt_context(request);
  REQUIRE(prepared.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared.attempt_id == uuid(350));
  REQUIRE(prepared.relationship_style.empty());
  REQUIRE(prepared.memories.empty());

  request.attempt_id = uuid(351);
  REQUIRE(fixture.relationships->prepare_prompt_context(request).status ==
          sanguinius::PromptPreparationStatus::duplicate);
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(350),
               .source_event_id = uuid(352),
               .relationship_event_id = uuid(353),
               .now_ms = 2'100}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt WHERE "
                 "source_message_id='350' AND state='succeeded'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          0);
}

TEST_CASE("relationship projections detect drift rebuild exactly and events "
          "stay immutable",
          "[relationship][sqlite][projection][rollback]") {
  RelationshipFixture fixture;
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=0,updated_at_ms=150 "
      "WHERE user_id='31'");
  const auto prepared = fixture.relationships->prepare_prompt_context(
      fixture.prompt(400, "A direct request"));
  REQUIRE(prepared.status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(fixture.relationships->complete_prompt_attempt(
              {.attempt_id = uuid(400),
               .source_event_id = uuid(401),
               .relationship_event_id = uuid(402),
               .now_ms = 2'100}) ==
          sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(fixture.relationships->check_projection().valid);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE relationship_event SET reason_code='fabricated'"));
  REQUIRE_THROWS(
      fixture.context->connection().execute("DELETE FROM relationship_event"));

  fixture.context->connection().execute(
      "UPDATE relationship_state SET familiarity=99 WHERE "
      "subject_user_id='31'");
  const auto drift = fixture.relationships->check_projection();
  REQUIRE_FALSE(drift.valid);
  REQUIRE(drift.mismatch_count == 1);
  fixture.context->connection().execute(
      "CREATE TRIGGER reject_relationship_projection_rebuild BEFORE INSERT ON "
      "relationship_state BEGIN SELECT RAISE(ABORT,'injected rebuild "
      "failure'); "
      "END");
  REQUIRE_THROWS(fixture.relationships->rebuild_projection());
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 99);
  fixture.context->connection().execute(
      "DROP TRIGGER reject_relationship_projection_rebuild");
  const auto rebuilt = fixture.relationships->rebuild_projection();
  REQUIRE(rebuilt.valid);
  REQUIRE(fixture.relationships->check_projection().valid);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 1);
}

TEST_CASE("canon catch-up includes retracted source authors but excludes "
          "proposer-only users",
          "[relationship][sqlite][chronicle][backfill]") {
  RelationshipFixture fixture;
  const sanguinius::CreateProposalRequest proposal{
      .entry_id = uuid(500),
      .event_id = uuid(501),
      .actions = {uuid(502), uuid(503), uuid(504)},
      .source = {.reference = {.message_id = 500,
                               .guild_id = 10,
                               .channel_id = 20},
                 .author = {.user_id = 32,
                            .username = "other",
                            .display_name = "Other"},
                 .content = "A source moment that later leaves the narrative.",
                 .occurred_at_ms = 900},
      .proposer_user_id = 31,
      .owner_user_id = 30,
      .title = "A retracted source",
      .body = "The relationship audit survives narrative retraction.",
      .correlation_id = "canon-correlation",
      .idempotency_key = "proposal:500",
      .now_ms = 1'000,
      .action_expires_at_ms = 10'000,
      .notice_expires_at_ms = 20'000,
  };
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          sanguinius::ChronicleResultCode::created);
  const sanguinius::SubmitProposalRequest submission{
      .token_id = proposal.actions.submit_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .proposer_approval_id = uuid(510),
      .submit_event_id = uuid(511),
      .immediate_canon_event_id = uuid(512),
      .reviewer_dispatches = {{.approval_id = uuid(513),
                               .notice_id = uuid(514),
                               .notice_open_token_id = uuid(515),
                               .approve_token_id = uuid(516),
                               .decline_token_id = uuid(517),
                               .notice_event_id = uuid(518),
                               .notice_outbox_id = uuid(519)}},
      .correlation_id = "canon-correlation",
      .interaction_idempotency_key = "submit:500",
      .now_ms = 1'100,
      .notice_expires_at_ms = 20'000,
  };
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          sanguinius::ChronicleResultCode::updated);
  const auto approved = fixture.chronicle->apply_approval({
      .token_id = uuid(516),
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(520),
      .canon_event_id = uuid(521),
      .public_outbox_id = uuid(522),
      .correlation_id = "canon-correlation",
      .interaction_idempotency_key = "approve:500",
      .now_ms = 1'200,
  });
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry.has_value());
  REQUIRE(fixture.chronicle
              ->retract_entry({.entity_id = uuid(500),
                               .expected_revision = approved.entry->revision,
                               .guild_id = 10,
                               .channel_id = 20,
                               .actor_user_id = 31,
                               .owner_user_id = 30,
                               .event_id = uuid(523),
                               .public_outbox_id = uuid(524),
                               .correlation_id = "canon-correlation",
                               .interaction_idempotency_key = "retract:500",
                               .now_ms = 1'300})
              .code == sanguinius::ChronicleResultCode::updated);

  sanguinius::test::ExhaustingFakePersistentIdGenerator ids{
      {uuid(530), uuid(531), uuid(532)}};
  REQUIRE(fixture.relationships->synchronize_chronicle_sources(ids, 1'400) ==
          1);
  REQUIRE(fixture.relationships->synchronize_chronicle_sources(ids, 1'500) ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM relationship_event WHERE "
                 "subject_user_id='32' AND reason_code='chronicle.canon'") ==
          1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM relationship_event WHERE "
                 "subject_user_id='31'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='32'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT esteem FROM relationship_state WHERE "
                 "subject_user_id='32'") == 1);
}

TEST_CASE("restart recovery abandons only prior-instance prompt reservations",
          "[relationship][sqlite][restart]") {
  RelationshipFixture fixture;
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=0,updated_at_ms=150 "
      "WHERE user_id='31'");
  REQUIRE(fixture.relationships
              ->prepare_prompt_context(fixture.prompt(600, "A pending request"))
              .status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(fixture.relationships->recover_prompt_attempts(uuid(2), 3'000) == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM ai_prompt_attempt WHERE "
                 "state='abandoned' AND failure_code='process_restart'") == 1);
  REQUIRE(fixture.relationships->recover_prompt_attempts(uuid(2), 3'001) == 0);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE ai_prompt_attempt SET state='cancelled',completed_at_ms=3002,"
      "failure_code='late' WHERE state='abandoned'"));
  REQUIRE_THROWS(
      fixture.context->connection().execute("DELETE FROM ai_prompt_attempt"));
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM relationship_event") ==
          0);
}

TEST_CASE(
    "memory callback preferences are consent-gated audited and replay-safe",
    "[relationship][sqlite][privacy][idempotency]") {
  RelationshipFixture fixture;
  const sanguinius::SetMemoryCallbacksRequest enable{
      .guild_id = 10,
      .channel_id = 20,
      .user_id = 31,
      .enabled = true,
      .event_id = uuid(700),
      .correlation_id = "preference-correlation",
      .idempotency_key = "callbacks:700",
      .now_ms = 2'000,
  };
  REQUIRE(fixture.relationships->set_memory_callbacks(enable) ==
          sanguinius::PreferenceChangeStatus::updated);
  REQUIRE(fixture.relationships->set_memory_callbacks(enable) ==
          sanguinius::PreferenceChangeStatus::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT memory_callback_opt_in FROM user_preference WHERE "
                 "user_id='31'") == 1);
  auto payload = fixture.context->connection().prepare(
      "SELECT payload_json FROM event_journal WHERE event_id=?");
  payload.bind(1, uuid(700));
  REQUIRE(payload.step());
  REQUIRE(payload.column_text(0) == "{\"enabled\":true}");

  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='32'");
  REQUIRE(fixture.relationships->set_memory_callbacks(
              {.guild_id = 10,
               .channel_id = 20,
               .user_id = 32,
               .enabled = true,
               .event_id = uuid(701),
               .correlation_id = "preference-correlation",
               .idempotency_key = "callbacks:701",
               .now_ms = 2'001}) ==
          sanguinius::PreferenceChangeStatus::chronicle_opted_out);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_id='" +
                     uuid(701) + "'") == 0);

  REQUIRE(fixture.relationships->set_memory_callbacks(
              {.guild_id = 10,
               .channel_id = 20,
               .user_id = 31,
               .enabled = false,
               .event_id = uuid(702),
               .correlation_id = "preference-correlation",
               .idempotency_key = "callbacks:702",
               .now_ms = 2'002}) ==
          sanguinius::PreferenceChangeStatus::updated);
  REQUIRE(scalar(*fixture.context,
                 "SELECT memory_callback_opt_in FROM user_preference WHERE "
                 "user_id='31'") == 0);
}

TEST_CASE(
    "competing prompt reservations and projection writes serialize safely",
    "[relationship][sqlite][concurrency]") {
  RelationshipFixture fixture;
  fixture.confirm_memory(800, "The obsidian dragon crossed the moon.",
                         {"dragon"});
  fixture.context->connection().execute(
      "UPDATE user_preference SET memory_callback_opt_in=1,updated_at_ms=150 "
      "WHERE user_id='31'");
  std::array<sanguinius::PreparedPromptContext, 2> prepared;
  std::barrier prepare_gate{3};
  std::thread first_prepare{[&] {
    prepare_gate.arrive_and_wait();
    prepared[0] = fixture.relationships->prepare_prompt_context(
        fixture.prompt(810, "dragon", 2'000));
  }};
  std::thread second_prepare{[&] {
    prepare_gate.arrive_and_wait();
    prepared[1] = fixture.relationships->prepare_prompt_context(
        fixture.prompt(820, "dragon", 2'000));
  }};
  prepare_gate.arrive_and_wait();
  first_prepare.join();
  second_prepare.join();
  REQUIRE(prepared[0].status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared[1].status == sanguinius::PromptPreparationStatus::prepared);
  REQUIRE(prepared[0].memories.size() + prepared[1].memories.size() == 1);

  std::array<sanguinius::PromptFinalizationStatus, 2> completed;
  std::barrier complete_gate{3};
  std::thread first_complete{[&] {
    complete_gate.arrive_and_wait();
    completed[0] = fixture.relationships->complete_prompt_attempt(
        {.attempt_id = uuid(810),
         .source_event_id = uuid(811),
         .relationship_event_id = uuid(812),
         .now_ms = 2'100});
  }};
  std::thread second_complete{[&] {
    complete_gate.arrive_and_wait();
    completed[1] = fixture.relationships->complete_prompt_attempt(
        {.attempt_id = uuid(820),
         .source_event_id = uuid(821),
         .relationship_event_id = uuid(822),
         .now_ms = 2'100});
  }};
  complete_gate.arrive_and_wait();
  first_complete.join();
  second_complete.join();
  REQUIRE(completed[0] == sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(completed[1] == sanguinius::PromptFinalizationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT use_count FROM memory WHERE memory_id='" + uuid(800) +
                     "'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT interaction_count FROM relationship_state WHERE "
                 "subject_user_id='31'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT familiarity FROM relationship_state WHERE "
                 "subject_user_id='31'") == 1);
  REQUIRE(fixture.relationships->check_projection().valid);
}
