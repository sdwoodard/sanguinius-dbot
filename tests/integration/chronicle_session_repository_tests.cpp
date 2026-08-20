#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_session_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <memory>
#include <numeric>
#include <thread>

namespace {

using namespace std::chrono_literals;
using sanguinius::ChronicleSessionResultCode;
using sanguinius::ChronicleSessionState;
using sanguinius::ChronicleSummaryCandidate;
using sanguinius::ChronicleTitleState;
using sanguinius::InteractionKind;
using sanguinius::SummaryEditRequest;
using sanguinius::SummaryJobCompletionRequest;
using sanguinius::TitleAction;
using sanguinius::WorkMutationStatus;
using sanguinius::persistence::Database;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SqliteChronicleRepository;
using sanguinius::persistence::SqliteChronicleSessionRepository;
using sanguinius::persistence::SqliteCoreIdentityRepository;
using sanguinius::persistence::SqliteDurableWorkRepository;
using sanguinius::persistence::SqliteRelationshipRepository;
using sanguinius::persistence::SqliteRepositoryContext;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

class SessionFixture {
public:
  SessionFixture() {
    {
      auto database = Database::open_migration(temporary.path(), 25ms);
      const Migrator migrator{sanguinius::persistence::production_migrations(),
                              {"test", "revision"},
                              clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 7);
    }
    context = std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path(), 25ms));
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
    identities.ensure_user({31, "Opener", "opener", false, 100});
    identities.ensure_user({32, "Member", "member", false, 100});
    context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=1");
    sessions = std::make_unique<SqliteChronicleSessionRepository>(context);
    chronicle = std::make_unique<SqliteChronicleRepository>(context);
    durable = std::make_unique<SqliteDurableWorkRepository>(context);
  }

  void
  insert_shared_entry(const std::string &entry_id,
                      const std::string &message_id = "900",
                      const std::string_view body = "Approved shared canon.") {
    auto insert = context->connection().prepare(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
        "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_"
        "ms,"
        "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
        "source_message_id,source_author_user_id,source_text,"
        "source_text_truncated,source_attachment_count,revision,source_kind) "
        "VALUES (?,'deed','First deed',?,'shared','canon',"
        "150,150,'31',150,150,'30','10','20',?,'31','source',0,0,1,"
        "'discord_message')");
    insert.bind(1, entry_id);
    insert.bind(2, body);
    insert.bind(3, message_id);
    insert.execute();
    auto participant = context->connection().prepare(
        "INSERT INTO chronicle_participant(entry_id,user_id,role) "
        "VALUES (?,'32','subject')");
    participant.bind(1, entry_id);
    participant.execute();
  }

  [[nodiscard]] std::int64_t scalar(const std::string_view sql) {
    auto query = context->connection().prepare(sql);
    REQUIRE(query.step());
    return query.column_int64(0);
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteChronicleSessionRepository> sessions;
  std::unique_ptr<SqliteChronicleRepository> chronicle;
  std::unique_ptr<SqliteDurableWorkRepository> durable;
};

} // namespace

TEST_CASE(
    "Chronicle sessions link system canon and bound authoritative context",
    "[chronicle][session][persistence][bounds]") {
  SessionFixture fixture;
  const auto session_id = uuid(800);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(801),
                       .correlation_id = "bounded-session",
                       .idempotency_key = "session:start:bounded",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);

  auto system_entry = fixture.context->connection().prepare(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,"
      "source_text_truncated,source_attachment_count,revision,source_kind) "
      "VALUES (?,'title_award','System title','Awarded during the session.',"
      "'shared','canon',110,110,'30',110,110,'30','10','20',NULL,'30','',"
      "0,0,1,'title_award')");
  system_entry.bind(1, uuid(802));
  system_entry.execute();
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_entry") == 1);

  for (std::size_t index = 0;
       index < sanguinius::maximum_session_linked_events + 5; ++index) {
    auto event = fixture.context->connection().prepare(
        "INSERT INTO "
        "event_journal(event_id,event_type,aggregate_type,aggregate_id,"
        "actor_user_id,guild_id,channel_id,source_message_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,causation_id,idempotency_key,payload_"
        "json) "
        "VALUES (?,'chronicle.test','chronicle_test',?,'31','10','20',NULL,?,?,"
        "'bounded-session',NULL,?,'{}')");
    event.bind(1, uuid(1'000 + index));
    event.bind(2, std::to_string(index));
    event.bind(3, static_cast<std::int64_t>(120 + index));
    event.bind(4, static_cast<std::int64_t>(120 + index));
    event.bind(5, "event:bounded:" + std::to_string(index));
    event.execute();
  }
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_event") ==
          static_cast<std::int64_t>(sanguinius::maximum_session_linked_events));

  const std::string maximum_body(1'000, 'x');
  for (std::size_t index = 0;
       index < sanguinius::maximum_session_linked_entries + 5; ++index) {
    fixture.insert_shared_entry(uuid(2'000 + index),
                                std::to_string(5'000 + index), maximum_body);
  }
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM chronicle_session_entry") ==
      static_cast<std::int64_t>(sanguinius::maximum_session_linked_entries));
  const auto summary_context = fixture.sessions->summary_context(session_id);
  REQUIRE(summary_context.shared_entry_ids.size() ==
          summary_context.shared_entry_context.size());
  REQUIRE(summary_context.shared_entry_context.size() <
          sanguinius::maximum_session_linked_entries);
  const auto summary_bytes =
      std::accumulate(summary_context.shared_entry_context.begin(),
                      summary_context.shared_entry_context.end(), std::size_t{},
                      [](const std::size_t total, const std::string &item) {
                        return total + item.size();
                      });
  REQUIRE(summary_bytes <= sanguinius::maximum_session_summary_input_bytes);
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .owner_user_id = 30,
                       .draft_id = uuid(2'100),
                       .summary_job_id = uuid(2'101),
                       .purge_job_id = uuid(2'102),
                       .event_id = uuid(2'103),
                       .correlation_id = "bounded-session",
                       .idempotency_key = "session:close:bounded",
                       .now_ms = 500})
              .session->state == ChronicleSessionState::closing);
}

TEST_CASE("session relinking imports participants from pre-session proposals",
          "[chronicle][session][participant][transition]") {
  SessionFixture fixture;
  const auto entry_id = uuid(2'200);
  auto entry = fixture.context->connection().prepare(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,"
      "source_text_truncated,source_attachment_count,revision,source_kind) "
      "VALUES (?,'deed','Earlier proposal','Approved during the session.',"
      "'shared','proposed',80,80,'31',80,NULL,NULL,'10','20','2200','31',"
      "'earlier source',0,0,1,'discord_message')");
  entry.bind(1, entry_id);
  entry.execute();
  auto participants = fixture.context->connection().prepare(
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES "
      "(?,'31','source_author'),(?,'32','subject')");
  participants.bind(1, entry_id);
  participants.bind(2, entry_id);
  participants.execute();

  const auto session_id = uuid(2'201);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(2'202),
                       .correlation_id = "relink",
                       .idempotency_key = "session:start:relink",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.context->connection().execute(
      "UPDATE chronicle_entry SET status='canon',approved_at_ms=110,"
      "approved_by_user_id='30',revision=revision+1 WHERE entry_id='" +
      entry_id + "'");

  REQUIRE(
      fixture.scalar(
          "SELECT count(*) FROM chronicle_session_entry WHERE session_id='" +
          session_id + "' AND entry_id='" + entry_id + "'") == 1);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM chronicle_session_participant WHERE "
                     "session_id='" +
                     session_id + "' AND user_id IN ('31','32')") == 2);
}

TEST_CASE("session context withdrawal purges and filters retained excerpts",
          "[chronicle][session][context][privacy]") {
  SessionFixture fixture;
  const auto session_id = uuid(2'300);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(2'301),
                       .correlation_id = "context-consent",
                       .idempotency_key = "session:start:context-consent",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  REQUIRE(fixture.sessions->observe_context(
      {.context_id = uuid(2'302),
       .guild_id = 10,
       .channel_id = 20,
       .message_id = 2'302,
       .author_user_id = 32,
       .excerpt = "This excerpt is consent-bound.",
       .correlation_id = "context-consent",
       .observed_at_ms = 110}));
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          1);

  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=0,updated_at_ms=120 "
      "WHERE user_id='32'");
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          0);
  REQUIRE_FALSE(fixture.sessions->observe_context(
      {.context_id = uuid(2'303),
       .guild_id = 10,
       .channel_id = 20,
       .message_id = 2'303,
       .author_user_id = 32,
       .excerpt = "This excerpt must not be retained.",
       .correlation_id = "context-consent",
       .observed_at_ms = 121}));

  auto retained = fixture.context->connection().prepare(
      "INSERT INTO chronicle_session_context(context_id,session_id,message_id,"
      "author_user_id,excerpt,observed_at_ms) VALUES (?,?,?,'32',?,?)");
  retained.bind(1, uuid(2'304));
  retained.bind(2, session_id);
  retained.bind(3, "2304");
  retained.bind(4, "Legacy retained excerpt.");
  retained.bind(5, 122);
  retained.execute();
  const auto context = fixture.sessions->summary_context(session_id);
  REQUIRE(context.transient_context.empty());
  REQUIRE(std::ranges::find(context.opted_in_participants, 32) ==
          context.opted_in_participants.end());
}

TEST_CASE(
    "Chronicle sessions recover through summary title search and anniversary",
    "[chronicle][session][persistence][search][anniversary]") {
  SessionFixture fixture;
  const auto session_id = uuid(100);
  const auto entry_id = uuid(101);
  const auto draft_id = uuid(102);
  const auto summary_job_id = uuid(103);
  const auto purge_job_id = uuid(104);
  const auto start =
      fixture.sessions->start({.session_id = session_id,
                               .guild_id = 10,
                               .channel_id = 20,
                               .actor_user_id = 31,
                               .event_id = uuid(105),
                               .correlation_id = "start",
                               .idempotency_key = "session:start:one",
                               .now_ms = 100});
  REQUIRE(start.code == ChronicleSessionResultCode::created);
  REQUIRE(start.session->state == ChronicleSessionState::open);
  REQUIRE(fixture.sessions
              ->start({.session_id = uuid(106),
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 32,
                       .event_id = uuid(107),
                       .correlation_id = "competing",
                       .idempotency_key = "session:start:two",
                       .now_ms = 110})
              .code == ChronicleSessionResultCode::existing);

  for (std::size_t index = 0; index < 25; ++index) {
    REQUIRE(fixture.sessions->observe_context(
        {.context_id = uuid(200 + index),
         .guild_id = 10,
         .channel_id = 20,
         .message_id = sanguinius::DiscordSnowflake{1'000 + index},
         .author_user_id = 32,
         .excerpt = std::string(500, 'z'),
         .correlation_id = "context",
         .observed_at_ms = 120 + static_cast<std::int64_t>(index)}));
  }
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          20);
  REQUIRE(fixture.scalar("SELECT sum(length(CAST(excerpt AS BLOB))) FROM "
                         "chronicle_session_context") <=
          static_cast<std::int64_t>(
              sanguinius::maximum_session_context_total_bytes));

  fixture.insert_shared_entry(entry_id);
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 32,
                       .owner_user_id = 30,
                       .draft_id = uuid(108),
                       .summary_job_id = uuid(109),
                       .purge_job_id = uuid(110),
                       .event_id = uuid(111),
                       .correlation_id = "unauthorized",
                       .idempotency_key = "session:close:unauthorized",
                       .now_ms = 200})
              .code == ChronicleSessionResultCode::unauthorized);
  const auto closed =
      fixture.sessions->close({.guild_id = 10,
                               .channel_id = 20,
                               .actor_user_id = 31,
                               .owner_user_id = 30,
                               .draft_id = draft_id,
                               .summary_job_id = summary_job_id,
                               .purge_job_id = purge_job_id,
                               .event_id = uuid(112),
                               .correlation_id = "close",
                               .idempotency_key = "session:close:one",
                               .now_ms = 210});
  REQUIRE(closed.code == ChronicleSessionResultCode::updated);
  REQUIRE(closed.wake_scheduler);
  REQUIRE(closed.session->state == ChronicleSessionState::closing);

  const auto summary_job =
      fixture.durable->claim_due_job(210, 1'000, "instance", uuid(113));
  REQUIRE(summary_job);
  REQUIRE(summary_job->job_type == sanguinius::session_summary_job_type);
  const ChronicleSummaryCandidate candidate{
      .chapter_title = "A model chapter",
      .summary = "A model summary of approved shared canon.",
      .highlighted_entry_ids = {entry_id},
      .proposed_titles = {{.recipient_user_id = 31,
                           .title = "Keeper of the Hour",
                           .description = "For sustaining the session.",
                           .supporting_entry_id = entry_id}},
  };
  const auto completion =
      fixture.sessions->complete_summary_job(SummaryJobCompletionRequest{
          .job = *summary_job,
          .generation_context = fixture.sessions->summary_context(session_id),
          .candidate = candidate,
          .failure_category = std::nullopt,
          .title_ids = {{.definition_id = uuid(114), .grant_id = uuid(115)}},
          .relationship_event_ids = {{.participant_user_id = 31,
                                      .relationship_event_id = uuid(140)},
                                     {.participant_user_id = 32,
                                      .relationship_event_id = uuid(157)}},
          .event_id = uuid(116),
          .notice_id = uuid(117),
          .notice_token_id = uuid(118),
          .edit_token_id = uuid(119),
          .approve_token_id = uuid(120),
          .reject_token_id = uuid(121),
          .notice_outbox_id = uuid(122),
          .owner_user_id = 30,
          .now_ms = 220});
  REQUIRE(completion == WorkMutationStatus::applied);
  REQUIRE(fixture.sessions->status(10)->state == ChronicleSessionState::closed);
  REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE outbox_id='" +
                         uuid(122) + "' AND state='pending'") == 1);

  const auto approve_control = fixture.sessions->resolve_summary_control(
      uuid(120), 30, 10, 20, InteractionKind::button, "summary:decision:one",
      221);
  REQUIRE(approve_control);
  REQUIRE(approve_control->draft_id == draft_id);
  REQUIRE(approve_control->action == "chronicle.summary.approve");
  REQUIRE(approve_control->expected_revision == 2);
  const SummaryEditRequest edit_request{
      .draft_id = draft_id,
      .expected_revision = 2,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .chapter_title = "A model chapter",
      .summary = "A model summary of approved shared canon.",
      .event_id = uuid(141),
      .notice_id = uuid(3'000),
      .notice_token_id = uuid(3'001),
      .edit_token_id = uuid(3'002),
      .approve_token_id = uuid(3'003),
      .reject_token_id = uuid(3'004),
      .notice_outbox_id = uuid(3'005),
      .idempotency_key = "summary:edit:one",
      .correlation_id = "edit",
      .control_token_id = uuid(119),
      .now_ms = 225};
  const auto edited = fixture.sessions->edit_summary(edit_request);
  REQUIRE(edited.code == ChronicleSessionResultCode::updated);
  REQUIRE(edited.wake_outbox);
  REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE outbox_id='" +
                         uuid(122) + "' AND state='cancelled'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE outbox_id='" +
                         uuid(3'005) + "' AND state='pending'") == 1);
  const auto claimed_review = fixture.durable->claim_due_outbox(
      226, 1'000, "instance", uuid(3'006), false);
  REQUIRE(claimed_review);
  REQUIRE(claimed_review->outbox_id == uuid(3'005));
  const auto replayed_edit_control = fixture.sessions->resolve_summary_control(
      uuid(119), 30, 10, 20, InteractionKind::modal_submit,
      "summary:edit:one", 226);
  REQUIRE(replayed_edit_control);
  REQUIRE(replayed_edit_control->draft_id == draft_id);
  REQUIRE(replayed_edit_control->action == "chronicle.summary.edit");
  REQUIRE(replayed_edit_control->expected_revision == 2);
  REQUIRE(fixture.sessions->edit_summary(edit_request).code ==
          ChronicleSessionResultCode::unchanged);
  REQUIRE_FALSE(fixture.sessions->resolve_summary_control(
      uuid(119), 30, 10, 20, InteractionKind::modal_submit,
      "summary:edit:stale", 226));
  REQUIRE_FALSE(fixture.sessions->resolve_summary_control(
      uuid(120), 30, 10, 20, InteractionKind::button,
      "summary:decision:stale", 226));
  const auto refreshed_control = fixture.sessions->resolve_summary_control(
      uuid(3'003), 30, 10, 20, InteractionKind::button,
      "summary:decision:one",
      226);
  REQUIRE(refreshed_control);
  REQUIRE(refreshed_control->expected_revision == 3);

  const auto summary_entry_id = uuid(123);
  const auto approved = fixture.sessions->decide_summary(
      {.draft_id = draft_id,
       .expected_revision = 3,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .approve = true,
       .entry_id = summary_entry_id,
       .event_id = uuid(124),
       .outbox_id = uuid(125),
       .idempotency_key = "summary:approve:one",
       .correlation_id = "approve",
       .control_token_id = uuid(3'003),
       .now_ms = 230});
  REQUIRE(approved.code == ChronicleSessionResultCode::updated);
  REQUIRE(approved.wake_outbox);
  REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE outbox_id='" +
                         uuid(3'005) + "' AND state='cancelled'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          0);
  REQUIRE_FALSE(fixture.sessions->resolve_summary_control(
      uuid(3'004), 30, 10, 20, InteractionKind::button,
      "summary:decision:other",
      231));
  REQUIRE_FALSE(fixture.sessions->resolve_summary_control(
      uuid(120), 30, 10, 20, InteractionKind::button,
      "summary:approve:different-interaction", 231));
  const auto replayed_control = fixture.sessions->resolve_summary_control(
      uuid(3'003), 30, 10, 20, InteractionKind::button,
      "summary:approve:one",
      231);
  REQUIRE(replayed_control);
  REQUIRE(fixture.sessions
              ->decide_summary({.draft_id = draft_id,
                                .expected_revision = 3,
                                .guild_id = 10,
                                .channel_id = 20,
                                .actor_user_id = 30,
                                .owner_user_id = 30,
                                .approve = true,
                                .entry_id = uuid(123),
                                .event_id = uuid(124),
                                .outbox_id = uuid(125),
                                .idempotency_key = "summary:approve:one",
                                .correlation_id = "approve",
                                .control_token_id = uuid(3'003),
                                .now_ms = 231})
              .code == ChronicleSessionResultCode::unchanged);

  const auto title =
      fixture.sessions->mutate_title({.grant_id = uuid(115),
                                      .expected_revision = 1,
                                      .guild_id = 10,
                                      .channel_id = 20,
                                      .actor_user_id = 30,
                                      .owner_user_id = 30,
                                      .action = TitleAction::approve,
                                      .award_entry_id = uuid(126),
                                      .event_id = uuid(127),
                                      .outbox_id = uuid(128),
                                      .relationship_event_id = uuid(129),
                                      .idempotency_key = "title:approve:one",
                                      .correlation_id = "title",
                                      .now_ms = 240});
  REQUIRE(title.code == ChronicleSessionResultCode::updated);
  REQUIRE(title.grant->state == ChronicleTitleState::active);
  REQUIRE(title.grant->featured);
  const auto title_page = fixture.sessions->list_titles(31, 31, false, 0);
  REQUIRE(title_page.total == 1);
  REQUIRE(title_page.grants.size() == 1);
  REQUIRE(title_page.grants.front().grant_id == uuid(115));
  REQUIRE(fixture.chronicle->manageable(31, 30, uuid(126), 240, 20).empty());
  const auto generic_title_retraction = fixture.chronicle->retract_entry(
      {.entity_id = uuid(126),
       .expected_revision = 1,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 31,
       .owner_user_id = 30,
       .event_id = uuid(158),
       .public_outbox_id = uuid(159),
       .correlation_id = "title",
       .interaction_idempotency_key = "title:generic-retract",
       .now_ms = 240});
  REQUIRE(generic_title_retraction.code ==
          sanguinius::ChronicleResultCode::invalid_state);
  REQUIRE(generic_title_retraction.entry->status ==
          sanguinius::ChronicleEntryStatus::canon);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM relationship_event WHERE "
              "subject_user_id='31' AND reason_code='session.completed'") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM relationship_event WHERE "
              "subject_user_id='31' AND reason_code='title.awarded'") == 1);
  REQUIRE(fixture.scalar("SELECT familiarity FROM relationship_state WHERE "
                         "subject_user_id='31'") == 1);
  REQUIRE(fixture.scalar("SELECT esteem FROM relationship_state WHERE "
                         "subject_user_id='31'") == 1);
  SqliteRelationshipRepository relationships{fixture.context};
  sanguinius::test::FakePersistentIdGenerator recovery_ids;
  REQUIRE(relationships.synchronize_chronicle_sources(recovery_ids, 241) == 0);
  const auto profile = relationships.profile(31, 31, false, 241);
  REQUIRE(profile.featured_title == "Keeper of the Hour");
  REQUIRE(profile.latest_session_summary.has_value());
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='31'");
  const auto opted_out_profile = relationships.profile(31, 31, false, 241);
  REQUIRE_FALSE(opted_out_profile.featured_title.has_value());
  REQUIRE_FALSE(opted_out_profile.latest_session_summary.has_value());
  REQUIRE_FALSE(opted_out_profile.session_open);
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1 WHERE user_id='31'");

  const auto second_proposal =
      fixture.sessions->propose_title({.definition_id = uuid(142),
                                       .grant_id = uuid(143),
                                       .guild_id = 10,
                                       .channel_id = 20,
                                       .actor_user_id = 30,
                                       .owner_user_id = 30,
                                       .recipient_user_id = 31,
                                       .title = "Witness of the Second Seal",
                                       .description = "A second curated title.",
                                       .event_id = uuid(144),
                                       .idempotency_key = "title:propose:two",
                                       .correlation_id = "title-two",
                                       .now_ms = 241});
  REQUIRE(second_proposal.code == ChronicleSessionResultCode::created);
  const auto second_approval =
      fixture.sessions->mutate_title({.grant_id = uuid(143),
                                      .expected_revision = 1,
                                      .guild_id = 10,
                                      .channel_id = 20,
                                      .actor_user_id = 30,
                                      .owner_user_id = 30,
                                      .action = TitleAction::approve,
                                      .award_entry_id = uuid(145),
                                      .event_id = uuid(146),
                                      .outbox_id = uuid(147),
                                      .relationship_event_id = uuid(148),
                                      .idempotency_key = "title:approve:two",
                                      .correlation_id = "title-two",
                                      .now_ms = 242});
  REQUIRE(second_approval.code == ChronicleSessionResultCode::updated);
  REQUIRE_FALSE(second_approval.grant->featured);
  const auto featured =
      fixture.sessions->mutate_title({.grant_id = uuid(143),
                                      .expected_revision = 2,
                                      .guild_id = 10,
                                      .channel_id = 20,
                                      .actor_user_id = 31,
                                      .owner_user_id = 30,
                                      .action = TitleAction::feature,
                                      .award_entry_id = uuid(149),
                                      .event_id = uuid(150),
                                      .outbox_id = uuid(151),
                                      .relationship_event_id = uuid(152),
                                      .idempotency_key = "title:feature:two",
                                      .correlation_id = "title-two",
                                      .now_ms = 243});
  REQUIRE(featured.code == ChronicleSessionResultCode::updated);
  REQUIRE(featured.grant->featured);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_title_grant WHERE featured=1 "
              "AND grant_id='00000000-0000-4000-8000-000000000143'") == 1);
  const auto revoked =
      fixture.sessions->mutate_title({.grant_id = uuid(143),
                                      .expected_revision = 3,
                                      .guild_id = 10,
                                      .channel_id = 20,
                                      .actor_user_id = 31,
                                      .owner_user_id = 30,
                                      .action = TitleAction::revoke,
                                      .award_entry_id = uuid(153),
                                      .event_id = uuid(154),
                                      .outbox_id = uuid(155),
                                      .relationship_event_id = uuid(156),
                                      .idempotency_key = "title:revoke:two",
                                      .correlation_id = "title-two",
                                      .now_ms = 244});
  REQUIRE(revoked.code == ChronicleSessionResultCode::updated);
  REQUIRE_FALSE(revoked.grant->featured);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_title_grant WHERE featured=1") ==
          0);

  const auto third_proposal = fixture.sessions->propose_title(
      {.definition_id = uuid(160),
       .grant_id = uuid(161),
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .recipient_user_id = 31,
       .title = "Witness of the Third Seal",
       .description = "A title approved after a featured revocation.",
       .event_id = uuid(162),
       .idempotency_key = "title:propose:three",
       .correlation_id = "title-three",
       .now_ms = 245});
  REQUIRE(third_proposal.code == ChronicleSessionResultCode::created);
  const auto third_approval =
      fixture.sessions->mutate_title({.grant_id = uuid(161),
                                      .expected_revision = 1,
                                      .guild_id = 10,
                                      .channel_id = 20,
                                      .actor_user_id = 30,
                                      .owner_user_id = 30,
                                      .action = TitleAction::approve,
                                      .award_entry_id = uuid(163),
                                      .event_id = uuid(164),
                                      .outbox_id = uuid(165),
                                      .relationship_event_id = uuid(166),
                                      .idempotency_key = "title:approve:three",
                                      .correlation_id = "title-three",
                                      .now_ms = 246});
  REQUIRE(third_approval.code == ChronicleSessionResultCode::updated);
  REQUIRE_FALSE(third_approval.grant->featured);
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(fixture.sessions
                ->propose_title(
                    {.definition_id = uuid(170 + index * 3),
                     .grant_id = uuid(171 + index * 3),
                     .guild_id = 10,
                     .channel_id = 20,
                     .actor_user_id = 30,
                     .owner_user_id = 30,
                     .recipient_user_id = 31,
                     .title = "Retained title " + std::to_string(index),
                     .description = "Retained provenance for pagination.",
                     .event_id = uuid(172 + index * 3),
                     .idempotency_key =
                         "title:propose:retained:" + std::to_string(index),
                     .correlation_id = "title-page",
                     .now_ms = 247})
                .code == ChronicleSessionResultCode::created);
  }
  const auto first_title_page = fixture.sessions->list_titles(31, 31, false, 0);
  const auto second_title_page =
      fixture.sessions->list_titles(31, 31, false, 1);
  REQUIRE(first_title_page.total == 6);
  REQUIRE(first_title_page.grants.size() ==
          sanguinius::chronicle_title_page_size);
  REQUIRE(second_title_page.total == 6);
  REQUIRE(second_title_page.grants.size() == 1);

  auto memory = fixture.context->connection().prepare(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,expires_at_ms,"
      "retracted_at_ms,expired_at_ms,last_used_at_ms,use_count,revision,"
      "creation_idempotency_key) VALUES (?,'explicit','needle remembered "
      "lantern','shared','ordinary','confirmed','user_confirmed',?,'31','31',"
      "248,248,NULL,NULL,NULL,NULL,0,1,'memory:search:one')");
  memory.bind(1, uuid(190));
  memory.bind(2, uuid(105));
  memory.execute();
  auto memory_subject = fixture.context->connection().prepare(
      "INSERT INTO memory_subject(memory_id,subject_type,subject_id) "
      "VALUES (?,'user','31')");
  memory_subject.bind(1, uuid(190));
  memory_subject.execute();

  const auto phrase_search =
      fixture.sessions->begin_search(31,
                                     {.query = "model summary",
                                      .participant = std::nullopt,
                                      .entry_type = std::nullopt,
                                      .from_ms = std::nullopt,
                                      .to_ms = std::nullopt,
                                      .presentation = "recall"},
                                     uuid(139), 249);
  REQUIRE(phrase_search.total == 1);
  REQUIRE(phrase_search.items.front().item_id == summary_entry_id);

  for (std::size_t index = 0; index < 4; ++index)
    fixture.insert_shared_entry(uuid(600 + index), std::to_string(901 + index));
  const auto search =
      fixture.sessions->begin_search(31,
                                     {.query = "",
                                      .participant = std::nullopt,
                                      .entry_type = std::nullopt,
                                      .from_ms = std::nullopt,
                                      .to_ms = std::nullopt,
                                      .presentation = "recall"},
                                     uuid(130), 250);
  REQUIRE(search.total >= 1);
  REQUIRE(search.items.size() == sanguinius::chronicle_search_page_size);
  REQUIRE(search.items.front().item_id == uuid(190));
  REQUIRE(search.items.front().title == "Explicit memory");
  REQUIRE(search.navigation_token_id == uuid(130));
  REQUIRE(fixture.sessions->search_page(32, uuid(130), 0, 251).items.empty());
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='31'");
  REQUIRE(fixture.sessions->search_page(31, uuid(130), 0, 251).items.empty());
  const auto opted_out_search =
      fixture.sessions->begin_search(31,
                                     {.query = "needle",
                                      .participant = std::nullopt,
                                      .entry_type = std::nullopt,
                                      .from_ms = std::nullopt,
                                      .to_ms = std::nullopt,
                                      .presentation = "recall"},
                                     uuid(192), 251);
  REQUIRE(opted_out_search.cursor_id.empty());
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_search_cursor WHERE cursor_id='" +
              uuid(192) + "'") == 0);
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1 WHERE user_id='31'");
  const auto second_page =
      fixture.sessions->advance_search(31, 10, 20, uuid(130), uuid(137), 252);
  REQUIRE(second_page.page == 1);
  REQUIRE_FALSE(second_page.items.empty());
  REQUIRE(
      fixture.sessions->advance_search(31, 10, 20, uuid(130), uuid(138), 253)
          .cursor_id.empty());

  for (std::size_t index = 0;
       index < sanguinius::chronicle_search_maximum_items; ++index) {
    fixture.insert_shared_entry(uuid(700 + index),
                                std::to_string(10'000 + index),
                                "A needle appears in shared canon.");
  }
  const auto saturated_search =
      fixture.sessions->begin_search(31,
                                     {.query = "needle",
                                      .participant = std::nullopt,
                                      .entry_type = std::nullopt,
                                      .from_ms = std::nullopt,
                                      .to_ms = std::nullopt,
                                      .presentation = "recall"},
                                     uuid(191), 254);
  REQUIRE(saturated_search.total == sanguinius::chronicle_search_maximum_items);
  const auto final_search_page = fixture.sessions->search_page(
      31, uuid(191),
      sanguinius::chronicle_search_maximum_items /
              sanguinius::chronicle_search_page_size -
          1,
      254);
  REQUIRE(std::ranges::any_of(final_search_page.items, [](const auto &item) {
    return item.title == "Explicit memory";
  }));

  fixture.context->connection().execute(
      "INSERT INTO chronicle_tag(entry_id,tag) VALUES ('" + summary_entry_id +
      "','owner-test')");
  constexpr std::int64_t anniversary_now = 1'800'000'000'000;
  const sanguinius::ScheduledJobEnqueue anniversary_job{
      .job_id = uuid(131),
      .job_type = std::string{sanguinius::anniversary_scan_job_type},
      .aggregate_type = "chronicle_anniversary",
      .aggregate_id = "test",
      .due_at_ms = anniversary_now,
      .max_attempts = 5,
      .idempotency_key = "job:anniversary:test:one",
      .created_at_ms = 260};
  REQUIRE(fixture.sessions->queue_anniversary_scan(
      anniversary_job,
      sanguinius::AnniversaryScanJobPayload{.local_date = "2027-01-15",
                                            .test_run = true},
      "anniversary"));
  auto duplicate_job = anniversary_job;
  duplicate_job.job_id = uuid(132);
  REQUIRE_FALSE(fixture.sessions->queue_anniversary_scan(
      duplicate_job,
      sanguinius::AnniversaryScanJobPayload{.local_date = "2027-01-15",
                                            .test_run = true},
      "anniversary"));
  const auto claimed_anniversary = fixture.durable->claim_due_job(
      anniversary_now, anniversary_now + 1'000, "instance", uuid(133));
  REQUIRE(claimed_anniversary);
  REQUIRE(claimed_anniversary->job_type ==
          sanguinius::anniversary_scan_job_type);
  sanguinius::test::FakePersistentIdGenerator ids{
      {uuid(134), uuid(135), uuid(136)}};
  const auto anniversary = fixture.sessions->run_anniversary_scan(
      *claimed_anniversary, "America/New_York", true, anniversary_now, ids);
  REQUIRE(anniversary.status == WorkMutationStatus::applied);
  REQUIRE(anniversary.wake_outbox);
  REQUIRE(fixture.sessions
              ->run_anniversary_scan(*claimed_anniversary, "America/New_York",
                                     true, anniversary_now, ids)
              .status == WorkMutationStatus::unchanged);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_anniversary_delivery WHERE "
              "is_test=1") == 1);
  const auto retract = fixture.chronicle->retract_entry(
      {.entity_id = summary_entry_id,
       .expected_revision = 1,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .event_id = uuid(193),
       .public_outbox_id = uuid(194),
       .correlation_id = "summary-retract",
       .interaction_idempotency_key = "summary:retract:one",
       .now_ms = 270});
  REQUIRE(retract.code == sanguinius::ChronicleResultCode::updated);
  REQUIRE(fixture.scalar("SELECT count(*) FROM event_journal WHERE event_id='" +
                         uuid(193) + "' AND source_message_id IS NULL") == 1);
  REQUIRE(fixture.sessions->search_page(31, uuid(139), 0, 271).items.empty());
}

TEST_CASE("open Chronicle context expires through durable scheduled work",
          "[chronicle][session][context][expiry][restart]") {
  SessionFixture fixture;
  const auto session_id = uuid(300);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(301),
                       .correlation_id = "context-expiry",
                       .idempotency_key = "session:start:context-expiry",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  REQUIRE(fixture.sessions->observe_context({.context_id = uuid(302),
                                             .guild_id = 10,
                                             .channel_id = 20,
                                             .message_id = 930,
                                             .author_user_id = 31,
                                             .excerpt = "first excerpt",
                                             .correlation_id = "context-expiry",
                                             .observed_at_ms = 100}));
  REQUIRE(fixture.sessions->observe_context({.context_id = uuid(303),
                                             .guild_id = 10,
                                             .channel_id = 20,
                                             .message_id = 931,
                                             .author_user_id = 31,
                                             .excerpt = "second excerpt",
                                             .correlation_id = "context-expiry",
                                             .observed_at_ms = 200}));

  const auto first_due = 100 + sanguinius::session_context_retention_ms;
  const auto first_claim = fixture.durable->claim_due_job(
      first_due, first_due + 1'000, "instance", uuid(304));
  REQUIRE(first_claim);
  REQUIRE(first_claim->job_type == sanguinius::session_context_purge_job_type);
  REQUIRE(fixture.sessions->purge_context_job(*first_claim, first_due) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          1);

  const auto second_due = 200 + sanguinius::session_context_retention_ms;
  const auto second_claim = fixture.durable->claim_due_job(
      second_due, second_due + 1'000, "instance", uuid(305));
  REQUIRE(second_claim);
  REQUIRE(second_claim->attempt_count == 1);
  REQUIRE(fixture.sessions->purge_context_job(*second_claim, second_due) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          0);
  REQUIRE(fixture.sessions->status(10)->state == ChronicleSessionState::open);
}

TEST_CASE("session association bounds preserve shared canon evidence",
          "[chronicle][session][bounds][privacy]") {
  SessionFixture fixture;
  REQUIRE(fixture.sessions
              ->start({.session_id = uuid(350),
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(351),
                       .correlation_id = "mixed-bounds",
                       .idempotency_key = "session:start:mixed-bounds",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(uuid(352), "935");
  for (std::size_t index = 0;
       index < sanguinius::maximum_session_linked_entries; ++index) {
    auto insert = fixture.context->connection().prepare(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
        "status,occurred_at_ms,created_at_ms,created_by_user_id,source_guild_"
        "id,"
        "source_channel_id,source_message_id,source_author_user_id,source_text,"
        "source_text_truncated,source_attachment_count,revision,source_kind) "
        "VALUES (?,'incident','Private','Participant-only record',"
        "'participant_only','proposed',?,?, '31','10','20',?,'31','source',"
        "0,0,1,'discord_message')");
    insert.bind(1, uuid(400 + index));
    insert.bind(2, static_cast<std::int64_t>(200 + index));
    insert.bind(3, static_cast<std::int64_t>(200 + index));
    insert.bind(4, std::to_string(10'500 + index));
    insert.execute();
  }
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM chronicle_session_entry") ==
      static_cast<std::int64_t>(sanguinius::maximum_session_linked_entries));
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_session_entry WHERE entry_id='" +
              uuid(352) + "'") == 1);
  const auto closed =
      fixture.sessions->close({.guild_id = 10,
                               .channel_id = 20,
                               .actor_user_id = 31,
                               .owner_user_id = 30,
                               .draft_id = uuid(460),
                               .summary_job_id = uuid(461),
                               .purge_job_id = uuid(462),
                               .event_id = uuid(463),
                               .correlation_id = "mixed-bounds",
                               .idempotency_key = "session:close:mixed-bounds",
                               .now_ms = 300});
  REQUIRE(closed.session->state == ChronicleSessionState::closing);
  REQUIRE(closed.session->linked_shared_canon_entries == 1);
}

TEST_CASE("claimed open-context cleanup honors the later close deadline",
          "[chronicle][session][context][close][lease]") {
  SessionFixture fixture;
  const auto session_id = uuid(470);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(471),
                       .correlation_id = "context-close-race",
                       .idempotency_key = "session:start:context-close-race",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  REQUIRE(
      fixture.sessions->observe_context({.context_id = uuid(472),
                                         .guild_id = 10,
                                         .channel_id = 20,
                                         .message_id = 940,
                                         .author_user_id = 31,
                                         .excerpt = "expiring excerpt",
                                         .correlation_id = "context-close-race",
                                         .observed_at_ms = 100}));
  REQUIRE(
      fixture.sessions->observe_context({.context_id = uuid(473),
                                         .guild_id = 10,
                                         .channel_id = 20,
                                         .message_id = 941,
                                         .author_user_id = 31,
                                         .excerpt = "newer excerpt",
                                         .correlation_id = "context-close-race",
                                         .observed_at_ms = 200}));
  fixture.insert_shared_entry(uuid(474), "942");

  const auto open_due = 100 + sanguinius::session_context_retention_ms;
  const auto claimed_purge = fixture.durable->claim_due_job(
      open_due, open_due + 1'000, "instance", uuid(475));
  REQUIRE(claimed_purge);
  const auto summary_job_id = uuid(477);
  const auto closed = fixture.sessions->close(
      {.guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 31,
       .owner_user_id = 30,
       .draft_id = uuid(476),
       .summary_job_id = summary_job_id,
       .purge_job_id = uuid(478),
       .event_id = uuid(479),
       .correlation_id = "context-close-race",
       .idempotency_key = "session:close:context-close-race",
       .now_ms = open_due});
  REQUIRE(closed.session->state == ChronicleSessionState::closing);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          1);
  REQUIRE(fixture.durable->cancel_job(summary_job_id, open_due) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.sessions->purge_context_job(*claimed_purge, open_due) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          1);

  const auto close_due = open_due + sanguinius::session_context_retention_ms;
  const auto reclaimed_purge = fixture.durable->claim_due_job(
      close_due, close_due + 1'000, "instance", uuid(480));
  REQUIRE(reclaimed_purge);
  REQUIRE(reclaimed_purge->job_type ==
          sanguinius::session_context_purge_job_type);
  REQUIRE(fixture.sessions->purge_context_job(*reclaimed_purge, close_due) ==
          WorkMutationStatus::applied);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_session_context") ==
          0);
}

TEST_CASE("session summary review tolerates backward wall clock movement",
          "[chronicle][session][summary][clock]") {
  SessionFixture fixture;
  const auto session_id = uuid(4'000);
  const auto draft_id = uuid(4'001);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(4'002),
                       .correlation_id = "clock-rollback",
                       .idempotency_key = "start:clock-rollback",
                       .now_ms = 200})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(uuid(4'003), "4000");
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .owner_user_id = 30,
                       .draft_id = draft_id,
                       .summary_job_id = uuid(4'004),
                       .purge_job_id = uuid(4'005),
                       .event_id = uuid(4'006),
                       .correlation_id = "clock-rollback",
                       .idempotency_key = "close:clock-rollback",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::updated);
  REQUIRE(fixture.scalar("SELECT closing_at_ms FROM chronicle_session WHERE "
                         "session_id='" +
                         session_id + "'") == 200);
  REQUIRE(fixture.scalar("SELECT created_at_ms FROM chronicle_summary_draft "
                         "WHERE draft_id='" +
                         draft_id + "'") == 200);

  const auto job =
      fixture.durable->claim_due_job(200, 1'000, "instance", uuid(4'007));
  REQUIRE(job);
  const auto context = fixture.sessions->summary_context(session_id);
  REQUIRE(fixture.sessions->complete_summary_job(
              {.job = *job,
               .generation_context = context,
               .candidate = std::nullopt,
               .failure_category = "model_failed",
               .title_ids = {},
               .relationship_event_ids = {{.participant_user_id = 31,
                                           .relationship_event_id = uuid(4'008)},
                                          {.participant_user_id = 32,
                                           .relationship_event_id = uuid(4'009)}},
               .event_id = uuid(4'010),
               .notice_id = uuid(4'011),
               .notice_token_id = uuid(4'012),
               .edit_token_id = uuid(4'013),
               .approve_token_id = uuid(4'014),
               .reject_token_id = uuid(4'015),
               .notice_outbox_id = uuid(4'016),
               .owner_user_id = 30,
               .now_ms = 150}) == WorkMutationStatus::applied);
  REQUIRE(fixture.scalar("SELECT closed_at_ms FROM chronicle_session WHERE "
                         "session_id='" +
                         session_id + "'") == 200);
  REQUIRE(fixture.scalar("SELECT updated_at_ms FROM chronicle_summary_draft "
                         "WHERE draft_id='" +
                         draft_id + "'") == 200);

  REQUIRE(fixture.sessions
              ->edit_summary({.draft_id = draft_id,
                              .expected_revision = 1,
                              .actor_user_id = 30,
                              .owner_user_id = 30,
                              .chapter_title = "Rollback-safe chapter",
                              .summary = "The reviewed prose remains valid.",
                              .event_id = uuid(4'017),
                              .notice_id = uuid(4'018),
                              .notice_token_id = uuid(4'019),
                              .edit_token_id = uuid(4'020),
                              .approve_token_id = uuid(4'021),
                              .reject_token_id = uuid(4'022),
                              .notice_outbox_id = uuid(4'023),
                              .idempotency_key = "edit:clock-rollback",
                              .correlation_id = "clock-rollback",
                              .control_token_id = std::nullopt,
                              .now_ms = 120})
              .code == ChronicleSessionResultCode::updated);
  REQUIRE(fixture.scalar("SELECT updated_at_ms FROM chronicle_summary_draft "
                         "WHERE draft_id='" +
                         draft_id + "'") == 200);

  REQUIRE(fixture.sessions
              ->decide_summary({.draft_id = draft_id,
                                .expected_revision = 2,
                                .guild_id = 10,
                                .channel_id = 20,
                                .actor_user_id = 30,
                                .owner_user_id = 30,
                                .approve = true,
                                .entry_id = uuid(4'024),
                                .event_id = uuid(4'025),
                                .outbox_id = uuid(4'026),
                                .idempotency_key = "approve:clock-rollback",
                                .correlation_id = "clock-rollback",
                                .control_token_id = std::nullopt,
                                .now_ms = 110})
              .code == ChronicleSessionResultCode::updated);
  REQUIRE(fixture.scalar("SELECT decided_at_ms FROM chronicle_summary_draft "
                         "WHERE draft_id='" +
                         draft_id + "'") == 200);
  REQUIRE(fixture.scalar("SELECT created_at_ms FROM chronicle_entry WHERE "
                         "entry_id='" +
                         uuid(4'024) + "'") == 200);
}

TEST_CASE("empty Chronicle sessions abandon without durable summary work",
          "[chronicle][session][transition]") {
  SessionFixture fixture;
  REQUIRE(fixture.sessions
              ->start({.session_id = uuid(500),
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(501),
                       .correlation_id = "start",
                       .idempotency_key = "start:empty",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  const auto closed = fixture.sessions->close({.guild_id = 10,
                                               .channel_id = 20,
                                               .actor_user_id = 30,
                                               .owner_user_id = 30,
                                               .draft_id = uuid(502),
                                               .summary_job_id = uuid(503),
                                               .purge_job_id = uuid(504),
                                               .event_id = uuid(505),
                                               .correlation_id = "close",
                                               .idempotency_key = "close:empty",
                                               .now_ms = 110});
  REQUIRE(closed.code == ChronicleSessionResultCode::updated);
  REQUIRE_FALSE(closed.wake_scheduler);
  REQUIRE(closed.session->state == ChronicleSessionState::abandoned);
  REQUIRE(fixture.scalar("SELECT count(*) FROM scheduled_job") == 0);
}

TEST_CASE("failed model summaries retain an editable deterministic fallback",
          "[chronicle][session][summary][fallback]") {
  SessionFixture fixture;
  const auto session_id = uuid(700);
  const auto draft_id = uuid(702);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(701),
                       .correlation_id = "fallback",
                       .idempotency_key = "start:fallback",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(uuid(703), "998");
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .owner_user_id = 30,
                       .draft_id = draft_id,
                       .summary_job_id = uuid(704),
                       .purge_job_id = uuid(705),
                       .event_id = uuid(706),
                       .correlation_id = "fallback",
                       .idempotency_key = "close:fallback",
                       .now_ms = 200})
              .code == ChronicleSessionResultCode::updated);
  REQUIRE(
      fixture.sessions
          ->edit_summary({.draft_id = draft_id,
                          .expected_revision = 1,
                          .actor_user_id = 30,
                          .owner_user_id = 30,
                          .chapter_title = "Too early",
                          .summary = "The closing draft cannot be edited yet.",
                          .event_id = uuid(721),
                          .notice_id = uuid(725),
                          .notice_token_id = uuid(726),
                          .edit_token_id = uuid(727),
                          .approve_token_id = uuid(728),
                          .reject_token_id = uuid(729),
                          .notice_outbox_id = uuid(730),
                          .idempotency_key = "edit:fallback:early",
                          .correlation_id = "fallback",
                          .control_token_id = std::nullopt,
                          .now_ms = 201})
          .code == ChronicleSessionResultCode::invalid_state);
  REQUIRE(fixture.sessions
              ->decide_summary({.draft_id = draft_id,
                                .expected_revision = 1,
                                .guild_id = 10,
                                .channel_id = 20,
                                .actor_user_id = 30,
                                .owner_user_id = 30,
                                .approve = true,
                                .entry_id = uuid(722),
                                .event_id = uuid(723),
                                .outbox_id = uuid(724),
                                .idempotency_key = "approve:fallback:early",
                                .correlation_id = "fallback",
                                .control_token_id = std::nullopt,
                                .now_ms = 202})
              .code == ChronicleSessionResultCode::invalid_state);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_entry WHERE "
                         "entry_type='session_summary'") == 0);
  REQUIRE(fixture.scalar(
              "SELECT revision FROM chronicle_summary_draft WHERE draft_id='" +
              draft_id + "'") == 1);
  const auto job =
      fixture.durable->claim_due_job(200, 1'000, "instance", uuid(707));
  REQUIRE(job);
  REQUIRE(fixture.sessions->complete_summary_job(
              {.job = *job,
               .generation_context =
                   fixture.sessions->summary_context(session_id),
               .candidate = std::nullopt,
               .failure_category = "refusal",
               .title_ids = {},
               .relationship_event_ids = {{.participant_user_id = 31,
                                           .relationship_event_id = uuid(708)},
                                          {.participant_user_id = 32,
                                           .relationship_event_id = uuid(709)}},
               .event_id = uuid(710),
               .notice_id = uuid(711),
               .notice_token_id = uuid(712),
               .edit_token_id = uuid(713),
               .approve_token_id = uuid(714),
               .reject_token_id = uuid(715),
               .notice_outbox_id = uuid(716),
               .owner_user_id = 30,
               .now_ms = 220}) == WorkMutationStatus::applied);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_summary_draft WHERE "
              "source='fallback' AND model_failure_category='refusal' AND "
              "state='pending'") == 1);
  REQUIRE(
      fixture.sessions
          ->edit_summary(
              {.draft_id = draft_id,
               .expected_revision = 1,
               .actor_user_id = 30,
               .owner_user_id = 30,
               .chapter_title = "The manually sealed chapter",
               .summary = "The owner supplied this bounded fallback summary.",
               .event_id = uuid(717),
               .notice_id = uuid(745),
               .notice_token_id = uuid(746),
               .edit_token_id = uuid(747),
               .approve_token_id = uuid(748),
               .reject_token_id = uuid(749),
               .notice_outbox_id = uuid(750),
               .idempotency_key = "edit:fallback",
               .correlation_id = "fallback",
               .control_token_id = std::nullopt,
               .now_ms = 230})
          .code == ChronicleSessionResultCode::updated);
  REQUIRE(fixture.sessions
              ->decide_summary({.draft_id = draft_id,
                                .expected_revision = 2,
                                .guild_id = 10,
                                .channel_id = 20,
                                .actor_user_id = 30,
                                .owner_user_id = 30,
                                .approve = true,
                                .entry_id = uuid(718),
                                .event_id = uuid(719),
                                .outbox_id = uuid(720),
                                .idempotency_key = "approve:fallback",
                                .correlation_id = "fallback",
                                .control_token_id = std::nullopt,
                                .now_ms = 240})
              .code == ChronicleSessionResultCode::updated);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_entry WHERE "
              "entry_type='session_summary' AND status='canon' AND "
              "body='The owner supplied this bounded fallback summary.'") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM outbox_message WHERE kind='notice.pending.v1' "
              "AND state='cancelled'") == 2);
  REQUIRE(fixture.scalar("SELECT count(*) FROM pending_notice") == 0);
}

TEST_CASE("summary completion rejects context changed during generation",
          "[chronicle][session][summary][privacy][revision]") {
  SessionFixture fixture;
  const auto session_id = uuid(760);
  const auto draft_id = uuid(761);
  const auto entry_id = uuid(762);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(763),
                       .correlation_id = "context-fence",
                       .idempotency_key = "start:context-fence",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(entry_id, "760");
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .owner_user_id = 30,
                       .draft_id = draft_id,
                       .summary_job_id = uuid(764),
                       .purge_job_id = uuid(765),
                       .event_id = uuid(766),
                       .correlation_id = "context-fence",
                       .idempotency_key = "close:context-fence",
                       .now_ms = 200})
              .code == ChronicleSessionResultCode::updated);
  const auto generated_from = fixture.sessions->summary_context(session_id);
  REQUIRE(generated_from.shared_entry_ids == std::vector<std::string>{entry_id});
  const auto job =
      fixture.durable->claim_due_job(200, 1'000, "instance", uuid(767));
  REQUIRE(job);

  fixture.context->connection().execute(
      "UPDATE chronicle_entry SET status='retracted',retracted_at_ms=221,"
      "retracted_by_user_id='31',revision=revision+1 WHERE entry_id='" +
      entry_id + "'");
  const ChronicleSummaryCandidate stale_candidate{
      .chapter_title = "A stale chapter",
      .summary = "Prose derived from canon that has since been retracted.",
      .highlighted_entry_ids = {},
      .proposed_titles = {},
  };
  REQUIRE(fixture.sessions->complete_summary_job(
              {.job = *job,
               .generation_context = generated_from,
               .candidate = stale_candidate,
               .failure_category = std::nullopt,
               .title_ids = {},
               .relationship_event_ids = {{.participant_user_id = 31,
                                           .relationship_event_id = uuid(768)},
                                          {.participant_user_id = 32,
                                           .relationship_event_id = uuid(769)}},
               .event_id = uuid(770),
               .notice_id = uuid(771),
               .notice_token_id = uuid(772),
               .edit_token_id = uuid(773),
               .approve_token_id = uuid(774),
               .reject_token_id = uuid(775),
               .notice_outbox_id = uuid(776),
               .owner_user_id = 30,
               .now_ms = 222}) == WorkMutationStatus::invalid_state);
  REQUIRE(fixture.sessions->status(10)->state == ChronicleSessionState::closing);
  REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM scheduled_job WHERE job_id='" +
                         uuid(764) + "' AND state='claimed'") == 1);

  REQUIRE(fixture.durable->release_job(*job, 222) ==
          WorkMutationStatus::applied);
  const auto retried =
      fixture.durable->claim_due_job(222, 1'000, "instance", uuid(777));
  REQUIRE(retried);
  const auto current_context = fixture.sessions->summary_context(session_id);
  REQUIRE(current_context.shared_entry_ids.empty());
  REQUIRE(fixture.sessions->complete_summary_job(
              {.job = *retried,
               .generation_context = current_context,
               .candidate = std::nullopt,
               .failure_category = "context_changed",
               .title_ids = {},
               .relationship_event_ids = {{.participant_user_id = 31,
                                           .relationship_event_id = uuid(778)},
                                          {.participant_user_id = 32,
                                           .relationship_event_id = uuid(779)}},
               .event_id = uuid(780),
               .notice_id = uuid(781),
               .notice_token_id = uuid(782),
               .edit_token_id = uuid(783),
               .approve_token_id = uuid(784),
               .reject_token_id = uuid(785),
               .notice_outbox_id = uuid(786),
               .owner_user_id = 30,
               .now_ms = 223}) == WorkMutationStatus::applied);
  REQUIRE(fixture.sessions->status(10)->state == ChronicleSessionState::closed);
}

TEST_CASE("summary completion fences the pending draft revision",
          "[chronicle][session][summary][revision]") {
  SessionFixture fixture;
  const auto session_id = uuid(730);
  const auto draft_id = uuid(731);
  REQUIRE(fixture.sessions
              ->start({.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(732),
                       .correlation_id = "draft-fence",
                       .idempotency_key = "start:draft-fence",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(uuid(733), "733");
  REQUIRE(fixture.sessions
              ->close({.guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .owner_user_id = 30,
                       .draft_id = draft_id,
                       .summary_job_id = uuid(734),
                       .purge_job_id = uuid(735),
                       .event_id = uuid(736),
                       .correlation_id = "draft-fence",
                       .idempotency_key = "close:draft-fence",
                       .now_ms = 200})
              .code == ChronicleSessionResultCode::updated);
  const auto job =
      fixture.durable->claim_due_job(200, 1'000, "instance", uuid(737));
  REQUIRE(job);
  fixture.context->connection().execute(
      "UPDATE chronicle_summary_draft SET revision=revision+1,source='manual' "
      "WHERE draft_id='" +
      draft_id + "'");
  REQUIRE(fixture.sessions->complete_summary_job(
              {.job = *job,
               .generation_context = {},
               .candidate = std::nullopt,
               .failure_category = "model_failed",
               .title_ids = {},
               .relationship_event_ids = {},
               .event_id = uuid(738),
               .notice_id = uuid(739),
               .notice_token_id = uuid(740),
               .edit_token_id = uuid(741),
               .approve_token_id = uuid(742),
               .reject_token_id = uuid(743),
               .notice_outbox_id = uuid(744),
               .owner_user_id = 30,
               .now_ms = 220}) == WorkMutationStatus::unchanged);
  REQUIRE(fixture.sessions->status(10)->state ==
          ChronicleSessionState::closing);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM outbox_message WHERE aggregate_id='" +
              session_id + "'") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM scheduled_job WHERE job_id='" +
                         uuid(734) + "' AND state='completed'") == 1);
}

TEST_CASE("concurrent session close and title approval keep one winner",
          "[chronicle][session][title][concurrency]") {
  SessionFixture fixture;
  REQUIRE(fixture.sessions
              ->start({.session_id = uuid(800),
                       .guild_id = 10,
                       .channel_id = 20,
                       .actor_user_id = 31,
                       .event_id = uuid(801),
                       .correlation_id = "concurrent",
                       .idempotency_key = "start:concurrent",
                       .now_ms = 100})
              .code == ChronicleSessionResultCode::created);
  fixture.insert_shared_entry(uuid(802), "997");

  std::array<sanguinius::SessionMutationResult, 2> closes;
  std::barrier close_gate{3};
  std::jthread close_one{[&] {
    close_gate.arrive_and_wait();
    closes[0] =
        fixture.sessions->close({.guild_id = 10,
                                 .channel_id = 20,
                                 .actor_user_id = 31,
                                 .owner_user_id = 30,
                                 .draft_id = uuid(803),
                                 .summary_job_id = uuid(804),
                                 .purge_job_id = uuid(805),
                                 .event_id = uuid(806),
                                 .correlation_id = "concurrent",
                                 .idempotency_key = "close:concurrent:one",
                                 .now_ms = 200});
  }};
  std::jthread close_two{[&] {
    close_gate.arrive_and_wait();
    closes[1] =
        fixture.sessions->close({.guild_id = 10,
                                 .channel_id = 20,
                                 .actor_user_id = 30,
                                 .owner_user_id = 30,
                                 .draft_id = uuid(807),
                                 .summary_job_id = uuid(808),
                                 .purge_job_id = uuid(809),
                                 .event_id = uuid(810),
                                 .correlation_id = "concurrent",
                                 .idempotency_key = "close:concurrent:two",
                                 .now_ms = 201});
  }};
  close_gate.arrive_and_wait();
  close_one.join();
  close_two.join();
  REQUIRE(std::ranges::count(closes, ChronicleSessionResultCode::updated,
                             &sanguinius::SessionMutationResult::code) == 1);
  REQUIRE(std::ranges::count(closes, ChronicleSessionResultCode::existing,
                             &sanguinius::SessionMutationResult::code) == 1);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM scheduled_job WHERE aggregate_id="
                     "'00000000-0000-4000-8000-000000000800'") == 2);

  REQUIRE(fixture.sessions
              ->propose_title(
                  {.definition_id = uuid(811),
                   .grant_id = uuid(812),
                   .guild_id = 10,
                   .channel_id = 20,
                   .actor_user_id = 30,
                   .owner_user_id = 30,
                   .recipient_user_id = 31,
                   .title = "The Singular Victor",
                   .description = "Only one approval transaction may prevail.",
                   .event_id = uuid(813),
                   .idempotency_key = "title:concurrent:proposal",
                   .correlation_id = "concurrent",
                   .now_ms = 210})
              .code == ChronicleSessionResultCode::created);
  std::array<sanguinius::TitleMutationResult, 2> approvals;
  std::barrier approval_gate{3};
  const auto approve = [&](const std::size_t index, const std::size_t base) {
    approval_gate.arrive_and_wait();
    approvals[index] = fixture.sessions->mutate_title(
        {.grant_id = uuid(812),
         .expected_revision = 1,
         .guild_id = 10,
         .channel_id = 20,
         .actor_user_id = 30,
         .owner_user_id = 30,
         .action = TitleAction::approve,
         .award_entry_id = uuid(base),
         .event_id = uuid(base + 1),
         .outbox_id = uuid(base + 2),
         .relationship_event_id = uuid(base + 3),
         .idempotency_key = "title:concurrent:" + std::to_string(index),
         .correlation_id = "concurrent",
         .now_ms = 220});
  };
  std::jthread approve_one{approve, 0, 814};
  std::jthread approve_two{approve, 1, 818};
  approval_gate.arrive_and_wait();
  approve_one.join();
  approve_two.join();
  REQUIRE(std::ranges::count(approvals, ChronicleSessionResultCode::updated,
                             &sanguinius::TitleMutationResult::code) == 1);
  REQUIRE(std::ranges::count(approvals,
                             ChronicleSessionResultCode::stale_revision,
                             &sanguinius::TitleMutationResult::code) == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_entry WHERE "
                         "entry_type='title_award'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM relationship_event WHERE "
                         "reason_code='title.awarded'") == 1);
}
