#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/appearances.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using sanguinius::persistence::Database;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SqliteAppearanceRepository;
using sanguinius::persistence::SqliteCoreIdentityRepository;
using sanguinius::persistence::SqliteDurableWorkRepository;
using sanguinius::persistence::SqliteRepositoryContext;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

[[nodiscard]] sanguinius::AppearancePolicy load_policy() {
  const auto path = std::filesystem::path{__FILE__}
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "config/appearance-policy-v1.json";
  std::ifstream stream{path};
  REQUIRE(stream.good());
  return sanguinius::parse_appearance_policy(
      std::string{std::istreambuf_iterator<char>{stream},
                  std::istreambuf_iterator<char>{}});
}

[[nodiscard]] std::int64_t scalar(SqliteRepositoryContext &context,
                                  const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_int64(0);
}

class AppearanceFixture {
public:
  AppearanceFixture() {
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
    identities.ensure_user({31, "First", "first", false, 100});
    identities.ensure_user({32, "Second", "second", false, 100});
    identities.ensure_user({42, "Sanguinius", "sanguinius", true, 100});
    context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=1,"
        "memory_callback_opt_in=1,appearance_callback_opt_in=1");
    repository = std::make_unique<SqliteAppearanceRepository>(context);
    repository->register_policy(policy, 100);
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  sanguinius::AppearancePolicy policy{load_policy()};
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteAppearanceRepository> repository;
};

} // namespace

TEST_CASE("schema v7 policy snapshots are immutable and version collision safe",
          "[appearance][repository][migration]") {
  AppearanceFixture fixture;
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_policy_snapshot") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM scheduled_job WHERE "
                                   "job_type LIKE 'appearance.%'") == 2);
  SqliteDurableWorkRepository durable{fixture.context};
  const auto scan = durable.claim_due_job(100, 1'100, "worker", uuid(990));
  REQUIRE(scan.has_value());
  REQUIRE(scan->job_type == sanguinius::appearance_scan_job_type);
  REQUIRE(std::get_if<sanguinius::AppearanceScanJobPayload>(&scan->payload) !=
          nullptr);
  REQUIRE(durable.reschedule_job(*scan, 100, 160) ==
          sanguinius::WorkMutationStatus::applied);
  const auto purge = durable.claim_due_job(100, 1'100, "worker", uuid(991));
  REQUIRE(purge.has_value());
  REQUIRE(purge->job_type == sanguinius::appearance_purge_job_type);
  REQUIRE(std::get_if<sanguinius::AppearancePurgeJobPayload>(&purge->payload) !=
          nullptr);
  REQUIRE(durable.reschedule_job(*purge, 100, 1'000) ==
          sanguinius::WorkMutationStatus::applied);
  fixture.repository->register_policy(fixture.policy, 200);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_policy_snapshot") == 1);
  auto collision = fixture.policy;
  collision.canonical_json += " ";
  REQUIRE_THROWS(fixture.repository->register_policy(collision, 300));
  auto next_json = nlohmann::json::parse(fixture.policy.canonical_json);
  next_json["policy_version"] = "m9-test-2";
  next_json["scoring"]["threshold"] = 61;
  const auto next = sanguinius::parse_appearance_policy(next_json.dump());
  fixture.repository->register_policy(next, 400);
  REQUIRE(fixture.repository->load_policy("m9-initial-1").score_threshold ==
          60);
  REQUIRE(fixture.repository->load_policy("m9-test-2").score_threshold == 61);
  auto scheduled = fixture.context->connection().prepare(
      "SELECT payload_json FROM scheduled_job WHERE job_type=?");
  scheduled.bind(1, sanguinius::appearance_scan_job_type);
  REQUIRE(scheduled.step());
  REQUIRE(
      nlohmann::json::parse(scheduled.column_text(0)).at("policy_version") ==
      "m9-test-2");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE appearance_policy_snapshot SET schema_version=1"));
}

TEST_CASE("schema v7 candidate references reject dangling identifiers",
          "[appearance][repository][migration][foreign-key]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute(
      "INSERT INTO appearance_message_activity(message_id,guild_id,channel_id,"
      "policy_version,author_user_id,author_is_bot,excerpt,observed_at_ms,"
      "expires_at_ms,correlation_id) VALUES('5000','10','20',"
      "'m9-initial-1','31',0,'ordinary activity',100,1800100,'fk-activity')");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE appearance_message_activity SET consumed_candidate_id='"
      "00000000-0000-4000-8000-000000009999' WHERE message_id='5000'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE appearance_message_activity SET serious_category='unknown' "
      "WHERE message_id='5000'"));

  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(
      sanguinius::EventJournalEntry{.event_id = uuid(5'001),
                                    .event_type = "tarot.settled.v1",
                                    .aggregate_type = "tarot_wager",
                                    .aggregate_id = uuid(5'002),
                                    .actor_user_id = 31,
                                    .guild_id = 10,
                                    .channel_id = 20,
                                    .source_message_id = std::nullopt,
                                    .occurred_at_ms = 100,
                                    .recorded_at_ms = 100,
                                    .correlation_id = "fk-observation",
                                    .causation_id = std::nullopt,
                                    .idempotency_key = "fk-observation",
                                    .payload_json = "{}"}));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE appearance_event_observation SET candidate_id='"
      "00000000-0000-4000-8000-000000009999' WHERE source_event_id='" +
      uuid(5'001) + "'"));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

TEST_CASE(
    "bounded primary activity extracts one idempotent conversation candidate",
    "[appearance][repository][activity]") {
  AppearanceFixture fixture;
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        sanguinius::AppearanceMessageObservation{
            .message_id = sanguinius::DiscordSnowflake{100 + index},
            .guild_id = 10,
            .channel_id = 20,
            .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                             : sanguinius::DiscordSnowflake{32},
            .author_is_bot = false,
            .excerpt =
                "ordinary lively game-night banter " + std::to_string(index),
            .observed_at_ms = 1'000 + static_cast<std::int64_t>(index),
            .correlation_id = "activity-" + std::to_string(index)},
        uuid(100 + index), uuid(200 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->actors.size() == 2);
  REQUIRE(candidate->excerpts.size() == 8);
  REQUIRE_FALSE(candidate->deterministic_serious_category);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate_source") == 8);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity WHERE "
                 "consumed_candidate_id IS NOT NULL") == 8);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);

  const auto duplicate =
      fixture.repository->observe_message(fixture.policy,
                                          {.message_id = 107,
                                           .guild_id = 10,
                                           .channel_id = 20,
                                           .author_user_id = 32,
                                           .author_is_bot = false,
                                           .excerpt = "duplicate",
                                           .observed_at_ms = 2'000,
                                           .correlation_id = "duplicate"},
                                          uuid(400), uuid(401));
  REQUIRE_FALSE(duplicate);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT human_message_count FROM appearance_channel_state") ==
          8);
}

TEST_CASE("conversation extraction uses message IDs to order equal timestamps",
          "[appearance][repository][activity][ordering]") {
  AppearanceFixture fixture;
  std::optional<sanguinius::AppearanceCandidate> candidate;
  std::vector<std::string> expected;
  for (std::size_t index = 0; index < 8; ++index) {
    expected.push_back("equal-time turn " + std::to_string(index));
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{600 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = expected.back(),
         .observed_at_ms = 1'500,
         .correlation_id = "equal-time-ordering"},
        uuid(600 + index), uuid(700 + index));
  }

  REQUIRE(candidate.has_value());
  REQUIRE(candidate->excerpts == expected);
  REQUIRE(candidate->alternating_turns);
}

TEST_CASE("activity from an older policy cannot seed a current candidate",
          "[appearance][repository][activity][policy]") {
  AppearanceFixture fixture;
  auto old_json = nlohmann::json::parse(fixture.policy.canonical_json);
  old_json["policy_version"] = "m9-old-activity";
  const auto old_policy = sanguinius::parse_appearance_policy(old_json.dump());
  fixture.repository->register_policy(old_policy, 500);

  for (std::size_t index = 0; index < 7; ++index) {
    REQUIRE_FALSE(fixture.repository->observe_message(
        old_policy,
        {.message_id = sanguinius::DiscordSnowflake{800 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = index == 0 ? "We prayed at church together."
                               : "older policy activity",
         .observed_at_ms = 1'000 + static_cast<std::int64_t>(index),
         .correlation_id = "old-policy-activity"},
        uuid(800 + index), uuid(900 + index)));
  }

  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{900 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "current policy activity " + std::to_string(index),
         .observed_at_ms = 1'100 + static_cast<std::int64_t>(index),
         .correlation_id = "current-policy-activity"},
        uuid(1'000 + index), uuid(1'100 + index));
    if (index == 0)
      REQUIRE_FALSE(candidate);
  }

  REQUIRE(candidate);
  REQUIRE(candidate->policy_version == fixture.policy.policy_version);
  REQUIRE(candidate->actors.size() == 2);
  REQUIRE(candidate->excerpts.size() == 8);
  REQUIRE_FALSE(candidate->deterministic_serious_category);
  auto sources = fixture.context->connection().prepare(
      "SELECT count(*) FROM appearance_candidate_source s JOIN "
      "appearance_message_activity a ON a.message_id=s.source_id WHERE "
      "s.candidate_id=? AND a.policy_version=?");
  sources.bind(1, candidate->candidate_id);
  sources.bind(2, fixture.policy.policy_version);
  REQUIRE(sources.step());
  REQUIRE(sources.column_int64(0) == 8);
}

TEST_CASE("activity sensitivity survives bounded excerpt truncation",
          "[appearance][repository][activity][safety]") {
  AppearanceFixture fixture;
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{1'100 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = index == 7 ? std::string(500, 'x') + " emergency"
                               : "ordinary game-night activity " +
                                     std::to_string(index),
         .observed_at_ms = 5'000 + static_cast<std::int64_t>(index),
         .correlation_id = "full-message-safety"},
        uuid(1'200 + index), uuid(1'300 + index));
  }

  REQUIRE(candidate);
  REQUIRE(candidate->deterministic_serious_category ==
          "crisis_self_harm_emergency");
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          *candidate, candidate->created_at_ms)
              .reason == "serious_context");
  auto stored = fixture.context->connection().prepare(
      "SELECT length(CAST(excerpt AS BLOB)),serious_category,"
      "instr(excerpt,'emergency') FROM appearance_message_activity WHERE "
      "message_id='1107'");
  REQUIRE(stored.step());
  REQUIRE(stored.column_int64(0) == 500);
  REQUIRE(stored.column_text(1) == "crisis_self_harm_emergency");
  REQUIRE(stored.column_int64(2) == 0);
}

TEST_CASE("message delivery deduplication survives activity retention",
          "[appearance][repository][activity][retention][idempotency]") {
  AppearanceFixture fixture;
  const auto observation = sanguinius::AppearanceMessageObservation{
      .message_id = 1'400,
      .guild_id = 10,
      .channel_id = 20,
      .author_user_id = 31,
      .author_is_bot = false,
      .excerpt = "ordinary retained activity",
      .observed_at_ms = 1'000,
      .correlation_id = "durable-message-fence"};
  REQUIRE_FALSE(fixture.repository->observe_message(fixture.policy, observation,
                                                    uuid(1'401), uuid(1'402)));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_seen") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT human_message_count FROM appearance_channel_state") ==
          1);

  const auto after_retention =
      observation.observed_at_ms + fixture.policy.activity_retention_ms;
  fixture.repository->purge(fixture.policy, after_retention);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 0);

  auto duplicate = observation;
  duplicate.observed_at_ms = after_retention + 1;
  duplicate.correlation_id = "durable-message-fence-replay";
  REQUIRE_FALSE(fixture.repository->observe_message(fixture.policy, duplicate,
                                                    uuid(1'403), uuid(1'404)));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_seen") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT human_message_count FROM appearance_channel_state") ==
          1);
}

TEST_CASE("conversation safety scans retained activity beyond consumed source "
          "messages",
          "[appearance][repository][activity][safety]") {
  AppearanceFixture fixture;
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 16; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{2'100 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = index == 0
                        ? "There is a medical emergency."
                        : "ordinary game-night banter " + std::to_string(index),
         .observed_at_ms = 10'000 + static_cast<std::int64_t>(index),
         .correlation_id = "full-window-safety"},
        uuid(2'200 + index), uuid(2'300 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->excerpts.size() == 8);
  REQUIRE(std::ranges::none_of(candidate->excerpts, [](const auto &excerpt) {
    return excerpt.find("emergency") != std::string::npos;
  }));
  REQUIRE(candidate->deterministic_serious_category ==
          "crisis_self_harm_emergency");
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          *candidate, 10'016)
              .reason == "serious_context");
}

TEST_CASE("conversation extraction excludes future rows after wall clock "
          "rollback",
          "[appearance][repository][activity][clock]") {
  AppearanceFixture fixture;
  for (std::size_t index = 0; index < 7; ++index) {
    REQUIRE_FALSE(fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{3'100 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "future-timestamp activity " + std::to_string(index),
         .observed_at_ms = 100'000 + static_cast<std::int64_t>(index),
         .correlation_id = "conversation-clock-future"},
        uuid(3'200 + index), uuid(3'300 + index)));
  }

  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 3'107,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 32,
       .author_is_bot = false,
       .excerpt = "current activity after rollback",
       .observed_at_ms = 90'000,
       .correlation_id = "conversation-clock-current"},
      uuid(3'207), uuid(3'307)));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 8);
}

TEST_CASE("Sanguinius speech timing survives bounded activity retention",
          "[appearance][repository][activity][retention]") {
  AppearanceFixture fixture;
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 2'400,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 42,
       .author_is_bot = true,
       .excerpt = "A prior Sanguinius response.",
       .observed_at_ms = 1'000,
       .correlation_id = "retained-speech"},
      uuid(2'401), uuid(2'402)));
  std::optional<sanguinius::AppearanceCandidate> candidate;
  constexpr std::int64_t after_retention = 1'801'001;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{2'410 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "ordinary retained timing " + std::to_string(index),
         .observed_at_ms = after_retention + static_cast<std::int64_t>(index),
         .correlation_id = "retained-speech"},
        uuid(2'420 + index), uuid(2'430 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity WHERE "
                 "author_is_bot=1") == 0);
  REQUIRE(candidate->bot_speech_age_ms == after_retention + 7 - 1'000);
  REQUIRE(candidate->human_messages_since_bot == 8);
  REQUIRE(candidate->human_message_count == 8);
}

TEST_CASE("owner simulations preserve hard gates and final hypotheticals are "
          "separate",
          "[appearance][repository][simulation]") {
  AppearanceFixture fixture;
  const auto request = sanguinius::AppearanceSimulationRequest{
      .fixture = "lively_game_night_banter",
      .idempotency_key = "simulation-one",
      .correlation_id = "simulation",
      .owner_user_id = 30,
      .now_ms = 10'000,
      .candidate_id = uuid(501),
      .event_id = uuid(502)};
  const auto candidate = fixture.repository->simulate(fixture.policy, request);
  const auto replay = fixture.repository->simulate(
      fixture.policy, sanguinius::AppearanceSimulationRequest{
                          .fixture = request.fixture,
                          .idempotency_key = request.idempotency_key,
                          .correlation_id = request.correlation_id,
                          .owner_user_id = 30,
                          .now_ms = 10'001,
                          .candidate_id = uuid(503),
                          .event_id = uuid(504)});
  REQUIRE(replay.candidate_id == candidate.candidate_id);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 10'001);
  REQUIRE(evaluation.eligible_for_model);
  REQUIRE(evaluation.score >= fixture.policy.score_threshold);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A fine evening for shared victories.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 0.95};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(505), uuid(506), "instance-a", "model_accepted", model,
      10'002));
  const auto stored = fixture.repository->decision(candidate.candidate_id);
  REQUIRE(stored.has_value());
  REQUIRE(stored->action == "hypothetical");
  REQUIRE(stored->preview == model.text);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM sqlite_schema WHERE "
                 "name='appearance_budget_reservation'") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);

  const auto stale = fixture.repository->simulate(
      fixture.policy, {.fixture = "stale_candidate",
                       .idempotency_key = "stale-one",
                       .correlation_id = "stale",
                       .owner_user_id = 30,
                       .now_ms = 11'000,
                       .candidate_id = uuid(507),
                       .event_id = uuid(508)});
  REQUIRE(stale.expires_at_ms > stale.created_at_ms);
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          stale, 11'000)
              .reason == "candidate_fresh");
}

TEST_CASE("post-hypothetical human activity count survives excerpt purge",
          "[appearance][repository][budget][retention]") {
  AppearanceFixture fixture;
  auto policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  policy_json["policy_version"] = "m9-retained-budget-test";
  policy_json["hypothetical_budget"]["maximum"] = 2;
  const auto policy = sanguinius::parse_appearance_policy(policy_json.dump());
  fixture.repository->register_policy(policy, 900);

  const auto first = fixture.repository->simulate(
      policy, {.fixture = "lively_game_night_banter",
               .idempotency_key = "retained-budget-first",
               .correlation_id = "retained-budget",
               .owner_user_id = 30,
               .now_ms = 1'000,
               .candidate_id = uuid(2'500),
               .event_id = uuid(2'501)});
  const auto first_evaluation = sanguinius::evaluate_appearance(
      policy, sanguinius::AppearanceMode::dry_run, first, 1'001);
  REQUIRE(fixture.repository->record_final(
      policy, sanguinius::AppearanceMode::dry_run, first, first_evaluation,
      uuid(2'502), uuid(2'503), "retained-budget-instance", "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "A first hypothetical.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      1'002));

  for (std::size_t index = 0; index < 8; ++index) {
    static_cast<void>(fixture.repository->observe_message(
        policy,
        {.message_id = sanguinius::DiscordSnowflake{2'510 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt =
             "ordinary post-hypothetical activity " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "retained-budget"},
        uuid(2'520 + index), uuid(2'530 + index)));
  }
  constexpr std::int64_t after_gap = 5'500'000;
  fixture.repository->purge(policy, after_gap);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 0);

  const auto second = fixture.repository->simulate(
      policy, {.fixture = "lively_game_night_banter",
               .idempotency_key = "retained-budget-second",
               .correlation_id = "retained-budget",
               .owner_user_id = 30,
               .now_ms = after_gap,
               .candidate_id = uuid(2'540),
               .event_id = uuid(2'541)});
  REQUIRE(second.human_message_count == 8);
  REQUIRE(second.messages_after_previous);
  REQUIRE(second.gap_available);
  REQUIRE(second.budget_available);
  const auto second_evaluation = sanguinius::evaluate_appearance(
      policy, sanguinius::AppearanceMode::dry_run, second, after_gap + 1);
  const auto messages_gate = std::ranges::find(
      second_evaluation.gates, std::string{"human_messages_after_previous"},
      &sanguinius::AppearanceGate::name);
  REQUIRE(messages_gate != second_evaluation.gates.end());
  REQUIRE(messages_gate->passed);
}

TEST_CASE("restart abandons prepared model decisions without retry",
          "[appearance][repository][restart]") {
  AppearanceFixture fixture;
  auto request =
      sanguinius::AppearanceSimulationRequest{.fixture = "owner_dry_run",
                                              .idempotency_key = "restart-one",
                                              .correlation_id = "restart",
                                              .owner_user_id = 30,
                                              .now_ms = 20'000,
                                              .candidate_id = uuid(601),
                                              .event_id = uuid(602)};
  const auto candidate = fixture.repository->simulate(fixture.policy, request);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 20'001);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(603), uuid(604), "old-instance", 20'001));
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(605)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", 20'002, restart_ids) == 1);
  const auto decision = fixture.repository->decision(uuid(603));
  REQUIRE(decision.has_value());
  REQUIRE(decision->state == "final");
  REQUIRE(decision->reason == "model_abandoned_restart");
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM appearance_preview") ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'appearance.decision_recorded.v1'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("restart reclaims an unprepared candidate exactly once",
          "[appearance][repository][restart]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "restart-unprepared",
                       .correlation_id = "restart",
                       .owner_user_id = 30,
                       .now_ms = 25'000,
                       .candidate_id = uuid(650),
                       .event_id = uuid(651)});
  REQUIRE(scalar(*fixture.context,
                 "SELECT evaluation_started_at_ms FROM appearance_candidate") ==
          25'000);
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(652)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", 25'001, restart_ids) == 0);
  const auto recovered =
      fixture.repository->scan_events(fixture.policy, 25'002, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().candidate_id == candidate.candidate_id);
  REQUIRE(
      fixture.repository->scan_events(fixture.policy, 25'003, "new-instance")
          .empty());
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("restart audits an unprepared candidate after it expires",
          "[appearance][repository][restart][expiry]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "restart-expired-unprepared",
                       .correlation_id = "restart-expiry",
                       .owner_user_id = 30,
                       .now_ms = 26'000,
                       .candidate_id = uuid(670),
                       .event_id = uuid(671)});
  const auto after_expiry = candidate.expires_at_ms + 1;
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(672)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", after_expiry, restart_ids) == 0);

  const auto recovered = fixture.repository->scan_events(
      fixture.policy, after_expiry, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().candidate_id == candidate.candidate_id);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      after_expiry);
  REQUIRE(evaluation.reason == "candidate_fresh");
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      evaluation, uuid(673), uuid(674), "new-instance", "not_requested",
      std::nullopt, after_expiry));
  REQUIRE(fixture.repository
              ->scan_events(fixture.policy, after_expiry + 1, "new-instance")
              .empty());
  const auto stored = fixture.repository->decision(uuid(673));
  REQUIRE(stored.has_value());
  REQUIRE(stored->reason == "candidate_fresh");
  REQUIRE(stored->action == "reject");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'appearance.decision_recorded.v1'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE(
    "restart recovery tolerates wall clock rollback and rejects freshness",
    "[appearance][repository][restart][clock]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "restart-clock-rollback",
                       .correlation_id = "restart-clock",
                       .owner_user_id = 30,
                       .now_ms = 25'000,
                       .candidate_id = uuid(660),
                       .event_id = uuid(661)});
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(662)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", 24'000, restart_ids) == 0);
  const auto recovered =
      fixture.repository->scan_events(fixture.policy, 24'000, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().candidate_id == candidate.candidate_id);
  REQUIRE(scalar(*fixture.context,
                 "SELECT evaluation_started_at_ms FROM appearance_candidate") ==
          25'000);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      24'000);
  REQUIRE(evaluation.reason == "candidate_fresh");
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      evaluation, uuid(663), uuid(664), "new-instance", "not_requested",
      std::nullopt, 24'000));
  const auto stored = fixture.repository->decision(uuid(663));
  REQUIRE(stored.has_value());
  REQUIRE(stored->reason == "candidate_fresh");
  REQUIRE(stored->action == "reject");
}

TEST_CASE("event triggers observe enabled Chronicle sources and audit Tarot as "
          "disabled",
          "[appearance][repository][events]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind)"
      " VALUES('00000000-0000-4000-8000-000000000700','incident','Shared "
      "victory',"
      "'A fine ordinary gathering.','shared','canon',29000,29000,'31',29000,"
      "29000,'30','10','20','700','31','A fine ordinary gathering.',1,"
      "'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000000700','31','source_author'),"
      "('00000000-0000-4000-8000-000000000700','32','subject');"
      "INSERT INTO appearance_message_activity(message_id,guild_id,channel_id,"
      "policy_version,author_user_id,author_is_bot,excerpt,observed_at_ms,"
      "expires_at_ms,correlation_id) "
      "VALUES"
      "('730','10','20','m9-initial-1','31',0,'ordinary game "
      "night',29990,1829990,"
      "'event-active-1'),"
      "('731','10','20','m9-initial-1','32',0,'ordinary shared "
      "victory',29991,1829991,'event-active-2')");
  const auto event = [&](const std::string &id, const std::string &type) {
    return sanguinius::EventJournalEntry{.event_id = id,
                                         .event_type = type,
                                         .aggregate_type = "chronicle_entry",
                                         .aggregate_id = uuid(700),
                                         .actor_user_id =
                                             sanguinius::DiscordSnowflake{31},
                                         .guild_id = 10,
                                         .channel_id = 20,
                                         .source_message_id = std::nullopt,
                                         .occurred_at_ms = 30'000,
                                         .recorded_at_ms = 30'000,
                                         .correlation_id = "event",
                                         .causation_id = std::nullopt,
                                         .idempotency_key = "event:" + id,
                                         .payload_json = "{}"};
  };
  REQUIRE(
      durable.append_event(event(uuid(701), "chronicle.entry_canonized.v1")));
  REQUIRE(durable.append_event(event(uuid(702), "tarot.settled.v1")));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_event_observation") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_event_observation WHERE "
                 "extraction_result='source_not_enabled'") == 1);
  const auto candidates =
      fixture.repository->scan_events(fixture.policy, 30'001, "instance");
  REQUIRE(candidates.size() == 1);
  REQUIRE(candidates.front().type ==
          sanguinius::AppearanceCandidateType::chronicle_entry);
  REQUIRE(candidates.front().actors.size() == 2);
  REQUIRE(candidates.front().visible);
  REQUIRE(scalar(*fixture.context,
                 "SELECT occurred_at_ms FROM event_journal WHERE event_type="
                 "'appearance.candidate_created.v1' ORDER BY rowid LIMIT 1") ==
          30'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT recorded_at_ms FROM event_journal WHERE event_type="
                 "'appearance.candidate_created.v1' ORDER BY rowid LIMIT 1") ==
          30'001);
  fixture.context->connection().execute(
      "UPDATE chronicle_entry SET visibility='participant_only' WHERE "
      "entry_id='00000000-0000-4000-8000-000000000700'");
  REQUIRE(
      durable.append_event(event(uuid(710), "chronicle.entry_canonized.v1")));
  const auto private_candidates =
      fixture.repository->scan_events(fixture.policy, 30'002, "instance");
  REQUIRE(private_candidates.size() == 1);
  REQUIRE_FALSE(private_candidates.front().visible);
  const auto private_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run,
      private_candidates.front(), 30'002);
  REQUIRE(private_evaluation.reason == "visibility");

  fixture.context->connection().execute(
      "UPDATE chronicle_entry SET visibility='shared',"
      "body='We prayed at church together.' WHERE "
      "entry_id='00000000-0000-4000-8000-000000000700'");
  REQUIRE(
      durable.append_event(event(uuid(720), "chronicle.entry_canonized.v1")));
  const auto serious_candidates =
      fixture.repository->scan_events(fixture.policy, 30'003, "instance");
  REQUIRE(serious_candidates.size() == 1);
  REQUIRE(serious_candidates.front().deterministic_serious_category ==
          "christianity");
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          serious_candidates.front(), 30'003)
              .reason == "serious_context");
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("off mode observations cannot backlog into a later dry run",
          "[appearance][repository][events][mode][restart]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind) VALUES('00000000-0000-4000-8000-000000000670','incident',"
      "'Mode fence','An ordinary shared event.','shared','canon',29000,29000,"
      "'31',29000,29000,'30','10','20','670','31',"
      "'An ordinary shared event.',1,'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000000670','31','source_author')");
  const auto event = [&](const std::size_t value,
                         const std::int64_t recorded_at_ms) {
    return sanguinius::EventJournalEntry{
        .event_id = uuid(value),
        .event_type = "chronicle.entry_canonized.v1",
        .aggregate_type = "chronicle_entry",
        .aggregate_id = uuid(670),
        .actor_user_id = 31,
        .guild_id = 10,
        .channel_id = 20,
        .source_message_id = std::nullopt,
        .occurred_at_ms = recorded_at_ms,
        .recorded_at_ms = recorded_at_ms,
        .correlation_id = "mode-fence",
        .causation_id = std::nullopt,
        .idempotency_key = "mode-fence:" + std::to_string(value),
        .payload_json = "{}"};
  };

  REQUIRE(durable.append_event(event(671, 30'000)));
  fixture.repository->activate_mode(sanguinius::AppearanceMode::off, 30'001);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_event_observation WHERE "
                 "extraction_result='mode_off' AND processed_at_ms=30001") ==
          1);

  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run,
                                    30'002);
  REQUIRE(fixture.repository->scan_events(fixture.policy, 30'002, "instance")
              .empty());

  REQUIRE(durable.append_event(event(672, 30'003)));
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run,
                                    30'004);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_event_observation WHERE "
                 "processed_at_ms IS NULL") == 1);
  const auto candidates =
      fixture.repository->scan_events(fixture.policy, 30'004, "new-instance");
  REQUIRE(candidates.size() == 1);
  REQUIRE(candidates.front().type ==
          sanguinius::AppearanceCandidateType::chronicle_entry);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT mode='dry_run' FROM appearance_mode_state WHERE "
                 "singleton=1") == 1);
}

TEST_CASE("event candidates use the full bounded activity window for actors "
          "and safety",
          "[appearance][repository][events][safety]") {
  AppearanceFixture fixture;
  for (std::size_t index = 0; index < 9; ++index) {
    static_cast<void>(fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{2'600 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index == 0 ? sanguinius::DiscordSnowflake{32}
                                      : sanguinius::DiscordSnowflake{31},
         .author_is_bot = false,
         .excerpt = index == 0
                        ? "A serious medical emergency."
                        : "ordinary event activity " + std::to_string(index),
         .observed_at_ms = 70'000 + static_cast<std::int64_t>(index),
         .correlation_id = "event-full-window"},
        uuid(2'610 + index), uuid(2'620 + index)));
  }
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(2'640),
      .event_type = "chronicle.entry_canonized.v1",
      .aggregate_type = "chronicle_entry",
      .aggregate_id = uuid(2'641),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 70'009,
      .recorded_at_ms = 70'009,
      .correlation_id = "event-full-window",
      .causation_id = std::nullopt,
      .idempotency_key = "event-full-window",
      .payload_json = "{}"}));
  const auto candidates =
      fixture.repository->scan_events(fixture.policy, 70'010, "instance");
  REQUIRE(candidates.size() == 1);
  REQUIRE(candidates.front().actors ==
          std::vector<sanguinius::DiscordSnowflake>{31, 32});
  REQUIRE(candidates.front().excerpts.size() == 8);
  REQUIRE(std::ranges::none_of(
      candidates.front().excerpts, [](const auto &excerpt) {
        return excerpt.find("emergency") != std::string::npos;
      }));
  REQUIRE(candidates.front().deterministic_serious_category ==
          "crisis_self_harm_emergency");
}

TEST_CASE("recovered event candidates rehydrate authoritative source context "
          "after excerpt purge",
          "[appearance][repository][events][restart][retention]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind)"
      " VALUES('00000000-0000-4000-8000-000000002800','incident','Recovered "
      "victory','An ordinary gathering worth recalling.','shared','canon',"
      "9000,9000,'31',9000,9000,'30','10','20','2800','31','An ordinary "
      "gathering worth recalling.',1,'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000002800','31','source_author'),"
      "('00000000-0000-4000-8000-000000002800','32','subject')");
  for (std::size_t index = 0; index < 2; ++index) {
    REQUIRE_FALSE(fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{2'810 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index == 0 ? sanguinius::DiscordSnowflake{31}
                                      : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "ordinary recovery activity",
         .observed_at_ms = 9'990 + static_cast<std::int64_t>(index),
         .correlation_id = "event-recovery-activity"},
        uuid(2'820 + index), uuid(2'830 + index)));
  }
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(2'801),
      .event_type = "chronicle.entry_canonized.v1",
      .aggregate_type = "chronicle_entry",
      .aggregate_id = uuid(2'800),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 10'000,
      .recorded_at_ms = 10'000,
      .correlation_id = "event-recovery",
      .causation_id = std::nullopt,
      .idempotency_key = "event-recovery",
      .payload_json = "{}"}));
  const auto initial =
      fixture.repository->scan_events(fixture.policy, 10'001, "instance");
  REQUIRE(initial.size() == 1);
  REQUIRE(initial.front().source_context.size() == 1);

  auto replacement_json = nlohmann::json::parse(fixture.policy.canonical_json);
  replacement_json["policy_version"] = "m9-recovery-replacement";
  replacement_json["activity"]["maximum_utf8_bytes_per_row"] = 32;
  replacement_json["serious_context"]["abuse_conflict"].push_back("ordinary");
  const auto replacement_policy =
      sanguinius::parse_appearance_policy(replacement_json.dump());
  fixture.repository->register_policy(replacement_policy, 10'002);

  const auto after_retention = 10'000 + fixture.policy.activity_retention_ms;
  fixture.repository->purge(fixture.policy, after_retention);
  REQUIRE(scalar(*fixture.context,
                 "SELECT json_array_length(context_json,'$.excerpts') FROM "
                 "appearance_candidate") == 0);
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(2'840)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", after_retention + 1, restart_ids) == 0);
  const auto recovered = fixture.repository->scan_events(
      replacement_policy, after_retention + 1, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().policy_version == fixture.policy.policy_version);
  REQUIRE(recovered.front().excerpts.empty());
  REQUIRE(recovered.front().source_context.size() == 1);
  REQUIRE(recovered.front().source_context.front().size() > 32);
  REQUIRE(recovered.front().source_context.front().find("Recovered victory") !=
          std::string::npos);
  REQUIRE_FALSE(recovered.front().deterministic_serious_category.has_value());
}

TEST_CASE("recovered Chronicle context uses its candidate policy bound",
          "[appearance][repository][restart][policy][chronicle]") {
  AppearanceFixture fixture;
  auto old_json = nlohmann::json::parse(fixture.policy.canonical_json);
  old_json["policy_version"] = "m9-chronicle-recovery-old";
  old_json["activity"]["maximum_utf8_bytes_per_row"] = 64;
  const auto old_policy = sanguinius::parse_appearance_policy(old_json.dump());
  fixture.repository->register_policy(old_policy, 90'000);
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind) VALUES('00000000-0000-4000-8000-000000002850','incident',"
      "'Longcampaign chronicle','Longcampaign victories remembered across a "
      "deliberately lengthy shared Chronicle source.','shared','canon',90000,"
      "90000,'31',90000,90000,'30','10','20','2850','31','Longcampaign "
      "victories remembered.',1,'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000002850','31','source_author'),"
      "('00000000-0000-4000-8000-000000002850','32','subject')");

  std::optional<sanguinius::AppearanceCandidate> created;
  for (std::size_t index = 0; index < 8; ++index) {
    created = fixture.repository->observe_message(
        old_policy,
        {.message_id = sanguinius::DiscordSnowflake{2'860 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "Longcampaign shared activity " + std::to_string(index),
         .observed_at_ms = 100'000 + static_cast<std::int64_t>(index),
         .correlation_id = "chronicle-policy-recovery"},
        uuid(2'880 + index), uuid(2'900 + index));
  }
  REQUIRE(created.has_value());
  REQUIRE(created->source_context.size() == 1);
  REQUIRE(created->source_context.front().size() == 64);

  auto replacement_json = nlohmann::json::parse(fixture.policy.canonical_json);
  replacement_json["policy_version"] = "m9-chronicle-recovery-current";
  replacement_json["activity"]["maximum_utf8_bytes_per_row"] = 32;
  const auto replacement_policy =
      sanguinius::parse_appearance_policy(replacement_json.dump());
  fixture.repository->register_policy(replacement_policy, 100'010);
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(2'920)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", 100'011, restart_ids) == 0);
  const auto recovered = fixture.repository->scan_events(
      replacement_policy, 100'012, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().policy_version == old_policy.policy_version);
  REQUIRE(recovered.front().source_context.size() == 1);
  REQUIRE(recovered.front().source_context.front().size() == 64);
}

TEST_CASE("event activity excludes future rows after wall clock rollback",
          "[appearance][repository][events][safety][clock]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind)"
      " VALUES('00000000-0000-4000-8000-000000002700','incident','Ordinary "
      "gathering','A quiet shared evening.','shared','canon',90000,90000,'31',"
      "90000,90000,'30','10','20','2700','31','A quiet shared evening.',1,"
      "'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000002700','31','source_author'),"
      "('00000000-0000-4000-8000-000000002700','32','subject');"
      "INSERT INTO appearance_message_activity(message_id,guild_id,channel_id,"
      "policy_version,author_user_id,author_is_bot,excerpt,observed_at_ms,"
      "expires_at_ms,correlation_id) VALUES"
      "('2701','10','20','m9-initial-1','31',0,'ordinary current "
      "activity',89990,1889990,"
      "'rollback-1'),"
      "('2702','10','20','m9-initial-1','32',0,'ordinary current "
      "reply',89991,1889991,"
      "'rollback-2'),"
      "('2703','10','20','m9-initial-1','31',0,'A serious medical "
      "emergency.',100000,1900000,"
      "'rollback-future')");
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(2'704),
      .event_type = "chronicle.entry_canonized.v1",
      .aggregate_type = "chronicle_entry",
      .aggregate_id = uuid(2'700),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 90'000,
      .recorded_at_ms = 90'000,
      .correlation_id = "rollback-event",
      .causation_id = std::nullopt,
      .idempotency_key = "rollback-event",
      .payload_json = "{}"}));

  const auto candidates =
      fixture.repository->scan_events(fixture.policy, 90'000, "instance");
  REQUIRE(candidates.size() == 1);
  REQUIRE(candidates.front().actors ==
          std::vector<sanguinius::DiscordSnowflake>{31, 32});
  REQUIRE(std::ranges::none_of(
      candidates.front().excerpts, [](const auto &excerpt) {
        return excerpt.find("emergency") != std::string::npos;
      }));
  REQUIRE_FALSE(candidates.front().deterministic_serious_category.has_value());
}

TEST_CASE("expired event candidates receive a durable rejection audit",
          "[appearance][repository][events][expiry]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind)"
      " VALUES('00000000-0000-4000-8000-000000002710','incident','Old "
      "gathering','An ordinary historical event.','shared','canon',1000,1000,"
      "'31',1000,1000,'30','10','20','2710','31','An ordinary historical "
      "event.',1,'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000002710','31','source_author')");
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(2'711),
      .event_type = "chronicle.entry_canonized.v1",
      .aggregate_type = "chronicle_entry",
      .aggregate_id = uuid(2'710),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "expired-event",
      .causation_id = std::nullopt,
      .idempotency_key = "expired-event",
      .payload_json = "{}"}));
  const auto now_ms =
      1'000 + fixture.policy.candidate_expiry_ms.at("chronicle_entry");
  const auto candidates =
      fixture.repository->scan_events(fixture.policy, now_ms, "instance");
  REQUIRE(candidates.size() == 1);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidates.front(),
      now_ms);
  REQUIRE(evaluation.reason == "candidate_fresh");
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidates.front(),
      evaluation, uuid(2'715), uuid(2'716), "instance", "not_requested",
      std::nullopt, now_ms));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_event_observation WHERE "
                 "extraction_result='expired' AND candidate_id IS NOT NULL") ==
          1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'appearance.candidate_created.v1'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE reason="
                 "'candidate_fresh' AND action='reject'") == 1);
}

TEST_CASE("appearance callback consent is self-service audited and replay safe",
          "[appearance][repository][consent]") {
  AppearanceFixture fixture;
  REQUIRE_FALSE(fixture.repository->set_callback_consent(
      31, true, 999, uuid(749), "appearance-consent-noop", "consent"));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'appearance.callback_consent_unchanged.v1'") == 1);
  REQUIRE(fixture.repository->set_callback_consent(
      31, false, 1'000, uuid(750), "appearance-consent-1", "consent"));
  REQUIRE(scalar(*fixture.context, "SELECT appearance_callback_opt_in FROM "
                                   "user_preference WHERE user_id='31'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'appearance.callback_consent_changed.v1'") == 1);
  REQUIRE_FALSE(fixture.repository->set_callback_consent(
      31, true, 1'000, uuid(754), "appearance-consent-noop", "consent"));
  REQUIRE(scalar(*fixture.context, "SELECT appearance_callback_opt_in FROM "
                                   "user_preference WHERE user_id='31'") == 0);
  REQUIRE_FALSE(fixture.repository->set_callback_consent(
      31, false, 1'001, uuid(751), "appearance-consent-1", "consent"));
  REQUIRE_THROWS(fixture.repository->set_callback_consent(
      31, true, 1'002, uuid(752), "appearance-consent-1", "consent"));
  REQUIRE(fixture.repository->set_callback_consent(
      31, true, 1'003, uuid(753), "appearance-consent-2", "consent"));
  REQUIRE(scalar(*fixture.context, "SELECT appearance_callback_opt_in FROM "
                                   "user_preference WHERE user_id='31'") == 1);
}

TEST_CASE(
    "Chronicle recurrence stores and revalidates its authoritative source",
    "[appearance][repository][chronicle][privacy]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind) VALUES('00000000-0000-4000-8000-000000001800','incident',"
      "'Calth victory','A shared triumph at Calth.','shared','canon',1000,"
      "1000,'31',1000,1000,'30','10','20','1800','31',"
      "'A shared triumph at Calth.',1,'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000001800','31','source_author'),"
      "('00000000-0000-4000-8000-000000001800','32','subject')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{1'810 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "Calth victory tale " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "chronicle-recurrence"},
        uuid(1'820 + index), uuid(1'840 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->type == sanguinius::AppearanceCandidateType::recurrence);
  REQUIRE(candidate->source_context.size() == 1);
  REQUIRE(candidate->safe_summary.find("shared-Chronicle") !=
          std::string::npos);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate_source WHERE "
                 "source_kind='chronicle_entry'") == 1);
  const auto ai_request =
      sanguinius::appearance_ai_request(fixture.policy, *candidate);
  REQUIRE(ai_request.conversation.front().content.find("shared triumph") !=
          std::string::npos);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate, 2'010);
  REQUIRE(evaluation.eligible_for_model);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(1'860), uuid(1'861), "instance", 2'010));
  fixture.context->connection().execute(
      "UPDATE chronicle_entry SET visibility='participant_only' WHERE "
      "entry_id='00000000-0000-4000-8000-000000001800'");
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(1'860), uuid(1'862), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "A victory remembered.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      2'011));
  const auto stored = fixture.repository->decision(uuid(1'860));
  REQUIRE(stored.has_value());
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "visibility");
}

TEST_CASE("post-model channel activity is revalidated before finalization",
          "[appearance][repository][activity][recheck]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "lively_game_night_banter",
                       .idempotency_key = "activity-recheck",
                       .correlation_id = "activity-recheck",
                       .owner_user_id = 30,
                       .now_ms = 60'000,
                       .candidate_id = uuid(1'870),
                       .event_id = uuid(1'871)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 60'001);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(1'872), uuid(1'873), "instance", 60'001));
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 1'874,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 42,
       .author_is_bot = true,
       .excerpt = "Sanguinius has just replied.",
       .observed_at_ms = 60'002,
       .correlation_id = "activity-recheck-bot"},
      uuid(1'875), uuid(1'876)));
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(1'872), uuid(1'877), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "Another observation.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      60'003));
  const auto stored = fixture.repository->decision(uuid(1'872));
  REQUIRE(stored.has_value());
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "bot_last_meaningful_speaker");
}

TEST_CASE("post-model serious human activity irrevocably suppresses a preview",
          "[appearance][repository][activity][safety]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "lively_game_night_banter",
                       .idempotency_key = "serious-recheck",
                       .correlation_id = "serious-recheck",
                       .owner_user_id = 30,
                       .now_ms = 61'000,
                       .candidate_id = uuid(1'880),
                       .event_id = uuid(1'881)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 61'001);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(1'882), uuid(1'883), "instance", 61'001));
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 1'884,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 31,
       .author_is_bot = false,
       .excerpt = "There is a serious medical emergency.",
       .observed_at_ms = 61'002,
       .correlation_id = "serious-recheck-message"},
      uuid(1'885), uuid(1'886)));
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(1'882), uuid(1'887), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "Another observation.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      61'003));
  const auto stored = fixture.repository->decision(uuid(1'882));
  REQUIRE(stored.has_value());
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "serious_context");
  REQUIRE(stored->serious_categories ==
          std::vector<std::string>{"crisis_self_harm_emergency"});
}

TEST_CASE("serious approved memory content suppresses before model preparation",
          "[appearance][repository][memory][safety]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(760),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(761),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "serious-memory",
      .causation_id = std::nullopt,
      .idempotency_key = "serious-memory-event",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,revision,"
      "creation_idempotency_key) VALUES("
      "'00000000-0000-4000-8000-000000000761','explicit',"
      "'Our victory prayer at church','shared','ordinary','confirmed',"
      "'user_confirmed','00000000-0000-4000-8000-000000000760','31','31',"
      "1000,1000,1,'serious-memory-create');"
      "INSERT INTO memory_subject VALUES("
      "'00000000-0000-4000-8000-000000000761','user','31')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{1'100 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "victory tale " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "serious-memory-activity"},
        uuid(770 + index), uuid(780 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->memory_context.size() == 1);
  REQUIRE(candidate->deterministic_serious_category == "christianity");
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          *candidate, 2'010)
              .reason == "serious_context");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("approved memory topics ground recurrence expiry and deduplication",
          "[appearance][repository][memory][recurrence]") {
  AppearanceFixture fixture;
  auto configured = nlohmann::json::parse(fixture.policy.canonical_json);
  configured["policy_version"] = "m9-topic-recurrence";
  configured["candidate_expiry_seconds"]["conversation"] = 600;
  configured["candidate_expiry_seconds"]["recurrence"] = 1'200;
  const auto recurrence_policy =
      sanguinius::parse_appearance_policy(configured.dump());
  fixture.repository->register_policy(recurrence_policy, 1'000);

  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(2'800),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(2'801),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "topic-memory",
      .causation_id = std::nullopt,
      .idempotency_key = "topic-memory",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,revision,"
      "creation_idempotency_key) VALUES('00000000-0000-4000-8000-"
      "000000002801','explicit','An ordinary remembered campaign.','shared',"
      "'ordinary','confirmed','user_confirmed','00000000-0000-4000-8000-"
      "000000002800','31','31',1000,1000,1,'topic-memory-create');"
      "INSERT INTO memory_subject VALUES"
      "('00000000-0000-4000-8000-000000002801','user','31'),"
      "('00000000-0000-4000-8000-000000002801','topic','victory')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        recurrence_policy,
        {.message_id = sanguinius::DiscordSnowflake{2'810 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "victory report " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "topic-recurrence"},
        uuid(2'820 + index), uuid(2'830 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->type == sanguinius::AppearanceCandidateType::recurrence);
  REQUIRE(candidate->memory_context.size() == 1);
  REQUIRE(candidate->recurrence_matches == 8);
  REQUIRE(candidate->expires_at_ms - candidate->created_at_ms == 1'200'000);
  auto stored = fixture.context->connection().prepare(
      "SELECT candidate_type,deduplication_key FROM appearance_candidate WHERE "
      "candidate_id=?");
  stored.bind(1, candidate->candidate_id);
  REQUIRE(stored.step());
  REQUIRE(stored.column_text(0) == "recurrence");
  REQUIRE(stored.column_text(1).starts_with("recurrence:"));
}

TEST_CASE("approved Chronicle tags ground recurrence candidates",
          "[appearance][repository][chronicle][recurrence]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind)"
      " VALUES('00000000-0000-4000-8000-000000002900','incident','An ordinary "
      "chapter','A quiet shared "
      "campaign.','shared','canon',1000,1000,'31',1000,"
      "1000,'30','10','20','2900','31','A quiet shared campaign.',1,"
      "'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES"
      "('00000000-0000-4000-8000-000000002900','31','source_author'),"
      "('00000000-0000-4000-8000-000000002900','32','subject');"
      "INSERT INTO chronicle_tag(entry_id,tag) VALUES"
      "('00000000-0000-4000-8000-000000002900','victory')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{2'910 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "victory exchange " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "chronicle-tag-recurrence"},
        uuid(2'920 + index), uuid(2'930 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->type == sanguinius::AppearanceCandidateType::recurrence);
  REQUIRE(candidate->memory_context.empty());
  REQUIRE(candidate->source_context.size() == 1);
  REQUIRE(candidate->source_context.front().find("victory") !=
          std::string::npos);
  REQUIRE(candidate->theme_key == "chronicle:" + uuid(2'900));
  REQUIRE(candidate->recurrence_matches == 8);
}

TEST_CASE("schema v7 dry-run trigger makes appearance public outbox impossible",
          "[appearance][repository][outbox]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "outbox-guard-candidate",
                       .correlation_id = "outbox-guard",
                       .owner_user_id = 30,
                       .now_ms = 1'000,
                       .candidate_id = uuid(802),
                       .event_id = uuid(803)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 1'001);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(806), uuid(807), "instance", "model_unavailable",
      std::nullopt, 1'001));

  const auto rejected_insert = [&](const std::string_view outbox_id,
                                   const std::string_view kind,
                                   const std::string_view aggregate_type,
                                   const std::string_view aggregate_id,
                                   const std::string_view idempotency_key,
                                   const std::string_view nonce) {
    auto insert = fixture.context->connection().prepare(
        "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
        "target_guild_id,target_channel_id,payload_json,state,attempt_count,"
        "max_attempts,idempotency_key,provider_nonce,created_at_ms,available_"
        "at_"
        "ms,updated_at_ms) VALUES(?,?,?,?,?,?,?, 'pending',0,5,?,?,?,?,?)");
    insert.bind(1, outbox_id);
    insert.bind(2, kind);
    insert.bind(3, aggregate_type);
    insert.bind(4, aggregate_id);
    insert.bind(5, "10");
    insert.bind(6, "20");
    insert.bind(7, "{}");
    insert.bind(8, idempotency_key);
    insert.bind(9, nonce);
    insert.bind(10, 1);
    insert.bind(11, 1);
    insert.bind(12, 1);
    REQUIRE_THROWS(insert.execute());
  };
  rejected_insert(uuid(804), "appearance.public.v1", "chronicle_entry",
                  uuid(9'991), "appearance-kind-forbidden",
                  "0000000000000000000000000");
  rejected_insert(uuid(805), sanguinius::public_discord_outbox_kind,
                  "appearance_candidate", uuid(9'992),
                  "appearance-type-forbidden", "0000000000000000000000001");
  rejected_insert(uuid(808), sanguinius::public_discord_outbox_kind,
                  "chronicle_entry", candidate.candidate_id,
                  "appearance-candidate-id-forbidden",
                  "0000000000000000000000002");
  rejected_insert(uuid(809), sanguinius::public_discord_outbox_kind,
                  "chronicle_entry", uuid(806),
                  "appearance-decision-id-forbidden",
                  "0000000000000000000000003");
  REQUIRE(fixture.repository->public_outbox_violation_count() == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("appearance outbox inspection recognizes aggregate ID violations",
          "[appearance][repository][outbox][inspection]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "outbox-inspection-candidate",
                       .correlation_id = "outbox-inspection",
                       .owner_user_id = 30,
                       .now_ms = 1'000,
                       .candidate_id = uuid(810),
                       .event_id = uuid(811)});
  fixture.context->connection().execute(
      "DROP TRIGGER appearance_dry_run_outbox_guard");
  auto insert = fixture.context->connection().prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,payload_json,state,attempt_count,max_"
      "attempts,idempotency_key,provider_nonce,created_at_ms,available_at_ms,"
      "updated_at_ms) VALUES(?,?,?,?,?,?,?,'pending',0,5,?,?,?,?,?)");
  insert.bind(1, uuid(812));
  insert.bind(2, sanguinius::public_discord_outbox_kind);
  insert.bind(3, "chronicle_entry");
  insert.bind(4, candidate.candidate_id);
  insert.bind(5, "10");
  insert.bind(6, "20");
  insert.bind(7, "{}");
  insert.bind(8, "appearance-id-violation");
  insert.bind(9, "0000000000000000000000004");
  insert.bind(10, 1);
  insert.bind(11, 1);
  insert.bind(12, 1);
  insert.execute();

  REQUIRE(fixture.repository->public_outbox_violation_count() == 1);
}

TEST_CASE(
    "literal approved memory context is consented audited and revalidated",
    "[appearance][repository][memory][privacy]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(811),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(812),
      .actor_user_id = sanguinius::DiscordSnowflake{31},
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "memory",
      .causation_id = std::nullopt,
      .idempotency_key = "memory-source",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO "
      "memory(memory_id,memory_type,text,visibility,sensitivity,status,"
      "confidence_basis,source_event_id,created_by_user_id,confirmed_by_user_"
      "id,"
      "created_at_ms,confirmed_at_ms,revision,creation_idempotency_key) VALUES("
      "'00000000-0000-4000-8000-000000000812','explicit','Our victory at "
      "Calth',"
      "'shared','ordinary','confirmed','user_confirmed',"
      "'00000000-0000-4000-8000-000000000811','31','31',1000,1000,1,'memory-"
      "create');"
      "INSERT INTO memory_subject VALUES("
      "'00000000-0000-4000-8000-000000000812','user','31')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{900 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "victory story " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "memory-activity"},
        uuid(820 + index), uuid(840 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->memory_context.size() == 1);
  REQUIRE(candidate->supplied_memory_ids ==
          std::vector<std::string>{uuid(812)});
  REQUIRE(candidate->chronicle_specificity == 25);
  REQUIRE(scalar(*fixture.context, "SELECT instr(context_json,'Our victory at "
                                   "Calth') FROM appearance_candidate") == 0);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate, 2'010);
  REQUIRE(evaluation.eligible_for_model);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(860), uuid(861), "instance", 2'010));
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=0 WHERE "
      "user_id='31'");
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(860), uuid(862), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "A remembered victory.",
                                        .tone = "warm",
                                        .memory_ids_used = {uuid(812)},
                                        .confidence = .95},
      2'011));
  const auto stored = fixture.repository->decision(uuid(860));
  REQUIRE(stored.has_value());
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "callback_consent");
  REQUIRE(stored->memory_ids == std::vector<std::string>{uuid(812)});
  REQUIRE(scalar(*fixture.context,
                 "SELECT used_by_model FROM appearance_decision_memory") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("same-memory cooldown ignores selected but unused model context",
          "[appearance][repository][memory][cooldown]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(3'100),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(3'101),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "unused-memory-source",
      .causation_id = std::nullopt,
      .idempotency_key = "unused-memory-source",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,confirmed_by_"
      "user_id,created_at_ms,confirmed_at_ms,revision,creation_idempotency_key)"
      " VALUES('00000000-0000-4000-8000-000000003101','explicit','Our Calth "
      "victory','shared','ordinary','confirmed','user_confirmed',"
      "'00000000-0000-4000-8000-000000003100','31','31',1000,1000,1,"
      "'unused-memory-create');"
      "INSERT INTO memory_subject VALUES("
      "'00000000-0000-4000-8000-000000003101','user','31')");

  std::optional<sanguinius::AppearanceCandidate> first;
  for (std::size_t index = 0; index < 8; ++index) {
    first = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{3'200 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "Calth victory first " + std::to_string(index),
         .observed_at_ms = 2'000 + static_cast<std::int64_t>(index),
         .correlation_id = "unused-memory-first"},
        uuid(3'300 + index), uuid(3'400 + index));
  }
  REQUIRE(first.has_value());
  REQUIRE(first->supplied_memory_ids == std::vector<std::string>{uuid(3'101)});
  const auto first_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *first, 2'010);
  REQUIRE(first_evaluation.eligible_for_model);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *first,
      first_evaluation, uuid(3'500), uuid(3'501), "instance", "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "A fine shared victory.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      2'010));
  REQUIRE(scalar(*fixture.context,
                 "SELECT used_by_model FROM appearance_decision_memory") == 0);

  std::optional<sanguinius::AppearanceCandidate> second;
  for (std::size_t index = 0; index < 8; ++index) {
    second = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{3'600 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "Calth victory second " + std::to_string(index),
         .observed_at_ms = 3'000 + static_cast<std::int64_t>(index),
         .correlation_id = "unused-memory-second"},
        uuid(3'700 + index), uuid(3'800 + index));
  }
  REQUIRE(second.has_value());
  REQUIRE(second->supplied_memory_ids == std::vector<std::string>{uuid(3'101)});
  REQUIRE(second->memory_available);
  REQUIRE_FALSE(second->theme_available);
  REQUIRE_FALSE(second->budget_available);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("recovered memory references fail closed at their stored revision",
          "[appearance][repository][memory][restart]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(1'900),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(1'901),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "restart-memory",
      .causation_id = std::nullopt,
      .idempotency_key = "restart-memory-event",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,revision,"
      "creation_idempotency_key) VALUES("
      "'00000000-0000-4000-8000-000000001901','explicit','Our Calth "
      "victory','shared','ordinary','confirmed','user_confirmed',"
      "'00000000-0000-4000-8000-000000001900','31','31',1000,1000,1,"
      "'restart-memory-create');"
      "INSERT INTO memory_subject VALUES("
      "'00000000-0000-4000-8000-000000001901','user','31')");
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{1'910 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt = "Calth victory " + std::to_string(index),
         .observed_at_ms = 3'000 + static_cast<std::int64_t>(index),
         .correlation_id = "restart-memory-activity"},
        uuid(1'920 + index), uuid(1'940 + index));
  }
  REQUIRE(candidate.has_value());
  REQUIRE(candidate->memory_context.front().revision == 1);
  sanguinius::test::FakePersistentIdGenerator restart_ids{{uuid(1'960)}};
  REQUIRE(fixture.repository->abandon_prior_instance_attempts(
              "new-instance", 3'009, restart_ids) == 0);
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=0 WHERE "
      "user_id='31'");
  const auto recovered =
      fixture.repository->scan_events(fixture.policy, 3'010, "new-instance");
  REQUIRE(recovered.size() == 1);
  REQUIRE(recovered.front().supplied_memory_ids ==
          std::vector<std::string>{uuid(1'901)});
  REQUIRE(recovered.front().memory_context.size() == 1);
  REQUIRE(recovered.front().memory_context.front().revision == 1);
  REQUIRE(recovered.front().memory_context.front().text.empty());
  REQUIRE_FALSE(recovered.front().memory_available);
  REQUIRE_FALSE(recovered.front().consented);
  REQUIRE_FALSE(recovered.front().visible);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      3'010);
  REQUIRE_FALSE(evaluation.eligible_for_model);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, recovered.front(),
      evaluation, uuid(1'961), uuid(1'962), "new-instance", "not_requested",
      std::nullopt, 3'010));
  REQUIRE(scalar(*fixture.context,
                 "SELECT memory_revision FROM appearance_decision_memory") ==
          1);
  const auto stored = fixture.repository->decision(uuid(1'961));
  REQUIRE(stored.has_value());
  const auto memory_gate =
      std::ranges::find(stored->gates, std::string{"memory_cooldown"},
                        &sanguinius::AppearanceGate::name);
  const auto consent_gate =
      std::ranges::find(stored->gates, std::string{"callback_consent"},
                        &sanguinius::AppearanceGate::name);
  const auto visibility_gate =
      std::ranges::find(stored->gates, std::string{"visibility"},
                        &sanguinius::AppearanceGate::name);
  REQUIRE(memory_gate != stored->gates.end());
  REQUIRE(consent_gate != stored->gates.end());
  REQUIRE(visibility_gate != stored->gates.end());
  REQUIRE_FALSE(memory_gate->passed);
  REQUIRE_FALSE(consent_gate->passed);
  REQUIRE_FALSE(visibility_gate->passed);
}

TEST_CASE("recurrence extraction persists novelty and repetition history",
          "[appearance][repository][scoring][history]") {
  AppearanceFixture fixture;
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(sanguinius::EventJournalEntry{
      .event_id = uuid(1'200),
      .event_type = "chronicle.memory_confirmed.v1",
      .aggregate_type = "memory",
      .aggregate_id = uuid(1'201),
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'000,
      .recorded_at_ms = 1'000,
      .correlation_id = "history-memory",
      .causation_id = std::nullopt,
      .idempotency_key = "history-memory-event",
      .payload_json = "{}"}));
  fixture.context->connection().execute_script(
      "INSERT INTO memory(memory_id,memory_type,text,visibility,sensitivity,"
      "status,confidence_basis,source_event_id,created_by_user_id,"
      "confirmed_by_user_id,created_at_ms,confirmed_at_ms,revision,"
      "creation_idempotency_key) VALUES("
      "'00000000-0000-4000-8000-000000001201','explicit',"
      "'Our Calth victory','shared','ordinary','confirmed','user_confirmed',"
      "'00000000-0000-4000-8000-000000001200','31','31',1000,1000,1,"
      "'history-memory-create');"
      "INSERT INTO memory_subject VALUES("
      "'00000000-0000-4000-8000-000000001201','user','31')");
  const auto extract = [&](const std::int64_t start,
                           const std::size_t id_base) {
    std::optional<sanguinius::AppearanceCandidate> candidate;
    for (std::size_t index = 0; index < 8; ++index) {
      candidate = fixture.repository->observe_message(
          fixture.policy,
          {.message_id = sanguinius::DiscordSnowflake{id_base + index},
           .guild_id = 10,
           .channel_id = 20,
           .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                            : sanguinius::DiscordSnowflake{32},
           .author_is_bot = false,
           .excerpt = "Calth victory " + std::to_string(index),
           .observed_at_ms = start + static_cast<std::int64_t>(index),
           .correlation_id = "history-activity"},
          uuid(id_base + 100 + index), uuid(id_base + 200 + index));
    }
    REQUIRE(candidate.has_value());
    return *candidate;
  };
  const auto first = extract(100'000, 1'300);
  const auto first_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, first, 100'010);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, first,
      first_evaluation, uuid(1'500), uuid(1'501), "history-instance",
      "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "A remembered victory.",
                                        .tone = "warm",
                                        .memory_ids_used = {uuid(1'201)},
                                        .confidence = .95},
      100'010));
  constexpr std::int64_t eight_days = 8LL * 24 * 60 * 60 * 1'000;
  const auto second = extract(100'000 + eight_days, 1'600);
  REQUIRE(second.novelty_age_ms.has_value());
  REQUIRE(second.repetition_age_ms.has_value());
  REQUIRE(*second.novelty_age_ms >= eight_days - 20);
  REQUIRE(*second.repetition_age_ms >= eight_days - 20);
  const auto second_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, second,
      100'010 + eight_days);
  REQUIRE(std::ranges::any_of(
      second_evaluation.score_components, [](const auto &component) {
        return component.name == "repetition" && component.points == -25;
      }));
}

TEST_CASE("preview retention purges prose but preserves decision audit",
          "[appearance][repository][retention]") {
  AppearanceFixture fixture;
  auto request = sanguinius::AppearanceSimulationRequest{
      .fixture = "lively_game_night_banter",
      .idempotency_key = "retention-one",
      .correlation_id = "retention",
      .owner_user_id = 30,
      .now_ms = 40'000,
      .candidate_id = uuid(901),
      .event_id = uuid(902)};
  const auto candidate = fixture.repository->simulate(fixture.policy, request);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 40'001);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(903), uuid(904), "instance", "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "Retained briefly.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .9},
      40'001));
  fixture.repository->purge(fixture.policy,
                            40'000 + fixture.policy.activity_retention_ms);
  REQUIRE(scalar(*fixture.context,
                 "SELECT json_array_length(context_json,'$.excerpts') FROM "
                 "appearance_candidate") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM appearance_preview") ==
          1);
  fixture.repository->purge(
      fixture.policy,
      40'001 + fixture.policy.generated_preview_retention_ms + 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM appearance_preview") ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate") == 1);
  const auto stored = fixture.repository->decision(uuid(903));
  REQUIRE(stored.has_value());
  REQUIRE(stored->policy_version == fixture.policy.policy_version);
  REQUIRE(stored->safe_summary ==
          "Owner simulation fixture: lively_game_night_banter.");
  REQUIRE_FALSE(stored->preview.has_value());
}

TEST_CASE("retention deadlines cannot be extended by a later policy version",
          "[appearance][repository][retention][policy]") {
  AppearanceFixture fixture;
  constexpr std::int64_t observed_at = 50'000;
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 3'600,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 31,
       .author_is_bot = false,
       .excerpt = "ordinary retained activity",
       .observed_at_ms = observed_at,
       .correlation_id = "retention-policy-activity"},
      uuid(3'601), uuid(3'602)));
  static_cast<void>(fixture.repository->simulate(
      fixture.policy, {.fixture = "lively_game_night_banter",
                       .idempotency_key = "retention-policy-candidate",
                       .correlation_id = "retention-policy",
                       .owner_user_id = 30,
                       .now_ms = observed_at,
                       .candidate_id = uuid(3'603),
                       .event_id = uuid(3'604)}));

  auto longer_json = nlohmann::json::parse(fixture.policy.canonical_json);
  longer_json["policy_version"] = "m9-longer-retention";
  longer_json["activity"]["retention_seconds"] = 7'200;
  const auto longer = sanguinius::parse_appearance_policy(longer_json.dump());
  fixture.repository->register_policy(longer, observed_at + 1);
  const auto original_deadline =
      observed_at + fixture.policy.activity_retention_ms;
  fixture.repository->purge(longer, original_deadline - 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT json_array_length(context_json,'$.excerpts') FROM "
                 "appearance_candidate") == 1);

  fixture.repository->purge(longer, original_deadline);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_message_activity") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT json_array_length(context_json,'$.excerpts') FROM "
                 "appearance_candidate") == 0);
}

TEST_CASE("two concurrent candidates can produce only one hypothetical",
          "[appearance][repository][concurrency]") {
  AppearanceFixture fixture;
  const auto make_candidate = [&](const std::size_t suffix) {
    return fixture.repository->simulate(
        fixture.policy,
        {.fixture = "lively_game_night_banter",
         .idempotency_key = "concurrent-" + std::to_string(suffix),
         .correlation_id = "concurrent",
         .owner_user_id = 30,
         .now_ms = 50'000,
         .candidate_id = uuid(1'000 + suffix),
         .event_id = uuid(1'010 + suffix)});
  };
  const auto first = make_candidate(1);
  const auto second = make_candidate(2);
  const auto first_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, first, 50'001);
  const auto second_evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, second, 50'001);
  const sanguinius::AppearanceModelResult model{.serious_context = false,
                                                .serious_categories = {},
                                                .should_speak = true,
                                                .text = "A hypothetical only.",
                                                .tone = "warm",
                                                .memory_ids_used = {},
                                                .confidence = .95};
  std::thread left{[&] {
    static_cast<void>(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::dry_run, first,
        first_evaluation, uuid(1'021), uuid(1'031), "instance",
        "model_accepted", model, 50'002));
  }};
  std::thread right{[&] {
    static_cast<void>(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::dry_run, second,
        second_evaluation, uuid(1'022), uuid(1'032), "instance",
        "model_accepted", model, 50'002));
  }};
  left.join();
  right.join();
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM appearance_decision "
                                   "WHERE action='hypothetical'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE "
                 "reason='hypothetical_daily_budget'") == 1);
  const auto decisions = fixture.repository->recent(10);
  const auto rejected =
      std::ranges::find(decisions, std::string{"hypothetical_daily_budget"},
                        &sanguinius::AppearanceDecisionRecord::reason);
  REQUIRE(rejected != decisions.end());
  const auto budget_gate = std::ranges::find(
      rejected->gates, std::string{"hypothetical_daily_budget"},
      &sanguinius::AppearanceGate::name);
  REQUIRE(budget_gate != rejected->gates.end());
  REQUIRE_FALSE(budget_gate->passed);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}
