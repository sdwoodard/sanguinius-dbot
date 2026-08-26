#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/appearances.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <exception>
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
      REQUIRE(migrator.apply(database.connection()).current_version == 14);
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

[[nodiscard]] std::vector<sanguinius::AppearanceCandidate>
conversation_candidates(AppearanceFixture &fixture,
                        const std::size_t message_count,
                        const std::uint64_t first_message_id,
                        const std::int64_t first_observed_at_ms,
                        const std::size_t first_uuid) {
  std::vector<sanguinius::AppearanceCandidate> result;
  for (std::size_t index = 0; index < message_count; ++index) {
    auto candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = first_message_id + index,
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index % 2 == 0 ? sanguinius::DiscordSnowflake{31}
                                          : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt =
             "ordinary lively game-night banter " + std::to_string(index),
         .observed_at_ms =
             first_observed_at_ms + static_cast<std::int64_t>(index),
         .correlation_id = "live-conversation"},
        uuid(first_uuid + index), uuid(first_uuid + 100 + index));
    if (candidate)
      result.push_back(std::move(*candidate));
  }
  return result;
}

} // namespace

TEST_CASE("schema v8 policy snapshots are immutable and version collision safe",
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
  REQUIRE(fixture.repository->load_policy("m10-live-1").score_threshold == 60);
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

TEST_CASE("schema v8 candidate references reject dangling identifiers",
          "[appearance][repository][migration][foreign-key]") {
  AppearanceFixture fixture;
  fixture.context->connection().execute(
      "INSERT INTO appearance_message_activity(message_id,guild_id,channel_id,"
      "policy_version,author_user_id,author_is_bot,excerpt,observed_at_ms,"
      "expires_at_ms,correlation_id) VALUES('5000','10','20',"
      "'m10-live-1','31',0,'ordinary activity',100,1800100,'fk-activity')");
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
                 "name='appearance_budget_reservation'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 0);
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
      "('730','10','20','m10-live-1','31',0,'ordinary game "
      "night',29990,1829990,"
      "'event-active-1'),"
      "('731','10','20','m10-live-1','32',0,'ordinary shared "
      "victory',29991,1829991,'event-active-2')");
  const auto event = [&](const std::string &id, const std::string &type,
                         const std::string &payload = "{}") {
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
                                         .payload_json = payload};
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
  REQUIRE(durable.append_event(
      event(uuid(704), "chronicle.entry_canonized.v1", R"({"test":true})")));
  REQUIRE(fixture.repository->scan_events(fixture.policy, 30'001, "instance")
              .empty());
  REQUIRE(scalar(*fixture.context,
                 "SELECT extraction_result='source_not_enabled' FROM "
                 "appearance_event_observation WHERE source_event_id="
                 "'00000000-0000-4000-8000-000000000704'") == 1);
  REQUIRE(durable.append_event(event(uuid(705),
                                     "chronicle.anniversary_delivered.v1",
                                     R"({"test_run":true})")));
  REQUIRE(fixture.repository->scan_events(fixture.policy, 30'001, "instance")
              .empty());
  REQUIRE(scalar(*fixture.context,
                 "SELECT extraction_result='source_not_enabled' FROM "
                 "appearance_event_observation WHERE source_event_id="
                 "'00000000-0000-4000-8000-000000000705'") == 1);
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
      "('2701','10','20','m10-live-1','31',0,'ordinary current "
      "activity',89990,1889990,"
      "'rollback-1'),"
      "('2702','10','20','m10-live-1','32',0,'ordinary current "
      "reply',89991,1889991,"
      "'rollback-2'),"
      "('2703','10','20','m10-live-1','31',0,'A serious medical "
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

TEST_CASE(
    "post-model active humans use the candidate policy and current window",
    "[appearance][repository][activity][recheck][privacy]") {
  AppearanceFixture fixture;
  auto policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  policy_json["policy_version"] = "m10-active-window-recheck";
  policy_json["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(policy_json.dump());
  fixture.repository->register_policy(fixture.policy, 900);
  std::optional<sanguinius::AppearanceCandidate> candidate;
  for (std::size_t index = 0; index < 8; ++index) {
    candidate = fixture.repository->observe_message(
        fixture.policy,
        {.message_id = sanguinius::DiscordSnowflake{37'000 + index},
         .guild_id = 10,
         .channel_id = 20,
         .author_user_id = index == 0 ? sanguinius::DiscordSnowflake{31}
                                      : sanguinius::DiscordSnowflake{32},
         .author_is_bot = false,
         .excerpt =
             "ordinary active-window conversation " + std::to_string(index),
         .observed_at_ms =
             index == 0 ? 1'000 : 590'000 + static_cast<std::int64_t>(index),
         .correlation_id = "active-window-recheck"},
        uuid(37'100 + index), uuid(37'200 + index));
  }
  REQUIRE(candidate);
  REQUIRE(candidate->actors ==
          std::vector<sanguinius::DiscordSnowflake>{31, 32});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate, 590'008);
  INFO("preparation reason: " << evaluation.reason);
  REQUIRE(evaluation.eligible_for_model);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(37'300), uuid(37'301), "active-window", 590'008));

  auto next_policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  next_policy_json["policy_version"] = "m10-active-window-next-policy";
  const auto next_policy =
      sanguinius::parse_appearance_policy(next_policy_json.dump());
  fixture.repository->register_policy(next_policy, 600'999);
  REQUIRE_FALSE(fixture.repository->observe_message(
      next_policy,
      {.message_id = 37'050,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 31,
       .author_is_bot = false,
       .excerpt = "ordinary activity under the next policy",
       .observed_at_ms = 601'000,
       .correlation_id = "active-window-next-policy"},
      uuid(37'350), uuid(37'351)));

  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, *candidate,
      evaluation, uuid(37'300), uuid(37'302), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "This must remain silent.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      601'001));
  const auto stored = fixture.repository->decision(uuid(37'300));
  REQUIRE(stored);
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "active_humans");
  const auto active_gate =
      std::ranges::find(stored->gates, std::string{"active_humans"},
                        &sanguinius::AppearanceGate::name);
  REQUIRE(active_gate != stored->gates.end());
  REQUIRE_FALSE(active_gate->passed);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("newly active opted-out humans suppress finalization",
          "[appearance][repository][activity][recheck][privacy][consent]") {
  AppearanceFixture fixture;
  auto policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  policy_json["policy_version"] = "m10-new-active-opt-out";
  policy_json["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(policy_json.dump());
  fixture.repository->register_policy(fixture.policy, 900);
  const auto candidates =
      conversation_candidates(fixture, 8, 39'000, 1'000, 39'100);
  REQUIRE(candidates.size() == 1);
  const auto &candidate = candidates.front();
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 1'008);
  REQUIRE(evaluation.eligible_for_model);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(39'300), uuid(39'301), "new-active-opt-out", 1'008));

  SqliteCoreIdentityRepository identities{fixture.context};
  identities.ensure_user({33, "Third", "third", false, 1'009});
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 39'010,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 33,
       .author_is_bot = false,
       .excerpt = "ordinary newly active conversation",
       .observed_at_ms = 1'009,
       .correlation_id = "new-active-opt-out"},
      uuid(39'302), uuid(39'303)));

  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(39'300), uuid(39'304), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "This must remain silent.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      1'010));
  const auto stored = fixture.repository->decision(uuid(39'300));
  REQUIRE(stored);
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "callback_consent");
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("newly active humans remain durable opt-out dependencies after queue",
          "[appearance][repository][activity][live][privacy][consent]") {
  AppearanceFixture fixture;
  auto policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  policy_json["policy_version"] = "m10-new-active-delivery-participant";
  policy_json["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(policy_json.dump());
  fixture.repository->register_policy(fixture.policy, 900);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 901);
  const auto candidates =
      conversation_candidates(fixture, 8, 39'500, 1'000, 39'600);
  REQUIRE(candidates.size() == 1);
  const auto &candidate = candidates.front();
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, 1'008);
  REQUIRE(evaluation.eligible_for_model);
  const auto decision_id = uuid(39'800);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, evaluation,
      decision_id, uuid(39'801), "new-active-delivery", 1'008));

  SqliteCoreIdentityRepository identities{fixture.context};
  identities.ensure_user({33, "Third", "third", false, 1'009});
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=1,"
      "updated_at_ms=1009 WHERE user_id='33'");
  REQUIRE_FALSE(fixture.repository->observe_message(
      fixture.policy,
      {.message_id = 39'510,
       .guild_id = 10,
       .channel_id = 20,
       .author_user_id = 33,
       .author_is_bot = false,
       .excerpt = "ordinary newly active live conversation",
       .observed_at_ms = 1'009,
       .correlation_id = "new-active-delivery"},
      uuid(39'802), uuid(39'803)));

  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(39'804),
      .outbox_id = uuid(39'805),
      .feedback_control_ids = {uuid(39'806), uuid(39'807), uuid(39'808),
                               uuid(39'809)}};
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, evaluation,
      decision_id, uuid(39'810), "model_accepted",
      sanguinius::AppearanceModelResult{
          .serious_context = false,
          .serious_categories = {},
          .should_speak = true,
          .text = "A safe final-participant dependency check.",
          .tone = "warm",
          .memory_ids_used = {},
          .confidence = .95},
      delivery, 1'010));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_delivery_participant WHERE "
                 "decision_id='" +
                     decision_id + "'") == 3);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_delivery_participant WHERE "
                 "decision_id='" +
                     decision_id + "' AND user_id='33'") == 1);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE appearance_delivery_participant SET created_at_ms=1011 WHERE "
      "decision_id='" +
      decision_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM appearance_delivery_participant WHERE decision_id='" +
      decision_id + "'"));

  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=0,"
      "updated_at_ms=1011 WHERE user_id='33'");
  REQUIRE(scalar(*fixture.context,
                 "SELECT state='cancelled' AND last_error_code='appearance_opt_"
                 "out' FROM outbox_message WHERE outbox_id='" +
                     delivery.outbox_id + "'") == 1);
}

TEST_CASE("post-model runtime degradation suppresses final delivery",
          "[appearance][repository][runtime][recheck]") {
  AppearanceFixture fixture;
  auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "lively_game_night_banter",
                       .idempotency_key = "runtime-recheck",
                       .correlation_id = "runtime-recheck",
                       .owner_user_id = 30,
                       .now_ms = 60'000,
                       .candidate_id = uuid(33'000),
                       .event_id = uuid(33'001)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate, 60'001);
  REQUIRE(fixture.repository->prepare_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(33'002), uuid(33'003), "runtime-recheck", 60'001));
  candidate.operational = false;
  candidate.degraded = true;
  REQUIRE(fixture.repository->complete_model(
      fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
      evaluation, uuid(33'002), uuid(33'004), "model_accepted",
      sanguinius::AppearanceModelResult{.serious_context = false,
                                        .serious_categories = {},
                                        .should_speak = true,
                                        .text = "This must remain private.",
                                        .tone = "warm",
                                        .memory_ids_used = {},
                                        .confidence = .95},
      60'002));
  const auto stored = fixture.repository->decision(uuid(33'002));
  REQUIRE(stored);
  REQUIRE(stored->action == "reject");
  REQUIRE(stored->reason == "operational");
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
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

TEST_CASE("schema v8 off and dry-run trigger make appearance outbox impossible",
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
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 1'002);
  rejected_insert(uuid(8'010), sanguinius::public_discord_outbox_kind,
                  "appearance_decision", uuid(806),
                  "appearance-dry-run-forbidden", "0000000000000000000000005");
  REQUIRE(fixture.repository->public_outbox_violation_count() == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("schema v8 rejects fully linked off dry-run and wrong-channel rows",
          "[appearance][repository][live][outbox][scope]") {
  AppearanceFixture fixture;
  auto mode = sanguinius::AppearanceMode::live;
  std::string target_channel{"21"};
  SECTION("off mode") {
    mode = sanguinius::AppearanceMode::off;
    target_channel = "20";
  }
  SECTION("dry-run mode") {
    mode = sanguinius::AppearanceMode::dry_run;
    target_channel = "20";
  }
  SECTION("wrong primary-channel scope") {}
  fixture.repository->activate_mode(mode, 500);
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "wrong-scope-candidate",
                       .correlation_id = "wrong-scope",
                       .owner_user_id = 30,
                       .now_ms = 1'000,
                       .candidate_id = uuid(34'000),
                       .event_id = uuid(34'001)});
  const auto decision_id = uuid(34'002);
  const auto live_event_id = uuid(34'003);
  const auto reservation_id = uuid(34'004);
  const auto outbox_id = uuid(34'005);
  auto decision = fixture.context->connection().prepare(
      "INSERT INTO appearance_decision(decision_id,candidate_id,"
      "policy_version,application_instance_id,revision,state,action,reason,"
      "gate_json,score_json,score,human_message_count,model_status,"
      "serious_categories_json,created_at_ms,finalized_at_ms) VALUES("
      "?,?,?,'wrong-scope-proof',1,'final','live_queued','live_queued','[]',"
      "'[]',80,0,'owner_fixture','[]',1001,1001)");
  decision.bind(1, decision_id);
  decision.bind(2, candidate.candidate_id);
  decision.bind(3, fixture.policy.policy_version);
  decision.execute();
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event(
      {.event_id = live_event_id,
       .event_type = "appearance.live_queued.v1",
       .aggregate_type = "appearance_decision",
       .aggregate_id = decision_id,
       .actor_user_id = 30,
       .guild_id = 10,
       .channel_id = 20,
       .source_message_id = std::nullopt,
       .occurred_at_ms = 1'001,
       .recorded_at_ms = 1'001,
       .correlation_id = "wrong-scope",
       .causation_id = std::nullopt,
       .idempotency_key = "wrong-scope-live-event",
       .payload_json = R"({"action":"live_queued","reason":"live_queued"})"}));
  auto &connection = fixture.context->connection();
  connection.execute("BEGIN IMMEDIATE");
  try {
    auto reservation = connection.prepare(
        "INSERT INTO appearance_budget_reservation VALUES(?,?,?,?,?,1001,0,1)");
    reservation.bind(1, reservation_id);
    reservation.bind(2, decision_id);
    reservation.bind(3, candidate.candidate_id);
    reservation.bind(4, outbox_id);
    reservation.bind(5, "appearance.reservation:" + decision_id);
    reservation.execute();
    constexpr std::array<std::string_view, 4> control_actions{
        "more", "less", "not_relevant", "quiet_tonight"};
    for (std::size_t index = 0; index < control_actions.size(); ++index) {
      auto control = connection.prepare(
          "INSERT INTO appearance_feedback_control(control_id,decision_id,"
          "action,created_at_ms,expires_at_ms) VALUES(?,?,?,?,?)");
      control.bind(1, uuid(34'010 + index));
      control.bind(2, decision_id);
      control.bind(3, control_actions[index]);
      control.bind(4, 1'001);
      control.bind(5, 3'000'000);
      control.execute();
    }
    const auto payload =
        nlohmann::json{{"payload_version", 1},
                       {"guild_id", "10"},
                       {"channel_id", target_channel},
                       {"content", "[TEST] Wrong channel must fail."},
                       {"embed", nullptr},
                       {"buttons",
                        {{{"custom_id", "sga:1:" + uuid(34'010)},
                          {"label", "More like this"},
                          {"disabled", false},
                          {"style", "secondary"}},
                         {{"custom_id", "sga:1:" + uuid(34'011)},
                          {"label", "Less like this"},
                          {"disabled", false},
                          {"style", "secondary"}},
                         {{"custom_id", "sga:1:" + uuid(34'012)},
                          {"label", "Not relevant"},
                          {"disabled", false},
                          {"style", "secondary"}},
                         {{"custom_id", "sga:1:" + uuid(34'013)},
                          {"label", "Quiet for tonight"},
                          {"disabled", false},
                          {"style", "secondary"}}}},
                       {"allowed_user_mentions", nlohmann::json::array()},
                       {"fail_before_first_send", false},
                       {"correlation_id", "wrong-scope"},
                       {"causation_event_id", live_event_id}};
    auto outbox = connection.prepare(
        "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
        "target_guild_id,target_channel_id,payload_json,state,attempt_count,"
        "max_attempts,idempotency_key,provider_nonce,created_at_ms,"
        "available_at_ms,updated_at_ms) VALUES(?,?,?,?,'10',?,?,'pending',"
        "0,5,?,?,1001,1001,1001)");
    outbox.bind(1, outbox_id);
    outbox.bind(2, sanguinius::public_discord_outbox_kind);
    outbox.bind(3, "appearance_decision");
    outbox.bind(4, decision_id);
    outbox.bind(5, target_channel);
    outbox.bind(6, payload.dump());
    outbox.bind(7, "appearance.public:" + decision_id);
    outbox.bind(8, sanguinius::discord_nonce_from_uuid(outbox_id));
    REQUIRE_THROWS(outbox.execute());
  } catch (...) {
    connection.execute("ROLLBACK");
    throw;
  }
  connection.execute("ROLLBACK");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 0);
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
      "DROP TRIGGER appearance_live_outbox_guard");
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

TEST_CASE("appearance reservations cannot adopt existing unrelated outbox rows",
          "[appearance][repository][outbox][migration][provenance]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "reverse-reservation-candidate",
                       .correlation_id = "reverse-reservation",
                       .owner_user_id = 30,
                       .now_ms = 1'000,
                       .candidate_id = uuid(38'000),
                       .event_id = uuid(38'001)});
  const auto decision_id = uuid(38'002);
  const auto outbox_id = uuid(38'003);
  auto decision = fixture.context->connection().prepare(
      "INSERT INTO appearance_decision(decision_id,candidate_id,policy_version,"
      "application_instance_id,revision,state,action,reason,gate_json,score_"
      "json,score,human_message_count,model_status,serious_categories_json,"
      "created_at_ms,finalized_at_ms) VALUES(?,?,?,'reverse-reservation',1,"
      "'final','live_queued','live_queued','[]','[]',80,0,'owner_fixture',"
      "'[]',1001,1001)");
  decision.bind(1, decision_id);
  decision.bind(2, candidate.candidate_id);
  decision.bind(3, fixture.policy.policy_version);
  decision.execute();

  auto outbox = fixture.context->connection().prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,payload_json,state,attempt_count,max_"
      "attempts,idempotency_key,provider_nonce,provider_message_id,created_at_"
      "ms,available_at_ms,first_attempt_at_ms,first_attempt_elapsed_ms,first_"
      "attempt_boot_id,delivered_at_ms,terminal_at_ms,updated_at_ms) VALUES("
      "?,'discord.public.v1','chronicle_entry',?,'10','20',?,'delivered',1,5,"
      "'unrelated-public-row','0000000000000000000000038','9010',1001,1001,"
      "1001,1,'reverse-test-boot',1002,1002,1002)");
  outbox.bind(1, outbox_id);
  outbox.bind(2, uuid(38'999));
  outbox.bind(3, R"({"content":"Unrelated bot output."})");
  outbox.execute();

  const auto insert_reservation = [&] {
    auto reservation = fixture.context->connection().prepare(
        "INSERT INTO appearance_budget_reservation(reservation_id,decision_id,"
        "candidate_id,outbox_id,idempotency_key,reserved_at_ms,human_message_"
        "count,is_test) VALUES(?,?,?,?,?,1001,0,1)");
    reservation.bind(1, uuid(38'004));
    reservation.bind(2, decision_id);
    reservation.bind(3, candidate.candidate_id);
    reservation.bind(4, outbox_id);
    reservation.bind(5, "appearance.reservation:" + decision_id);
    reservation.execute();
  };
  REQUIRE_THROWS(insert_reservation());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 0);

  fixture.context->connection().execute(
      "DROP TRIGGER appearance_budget_reservation_requires_new_outbox");
  insert_reservation();
  REQUIRE(fixture.repository->public_outbox_violation_count() == 1);
  REQUIRE_FALSE(fixture.repository->verify_public_delivery(
      {.reference = {.message_id = 9'010, .guild_id = 10, .channel_id = 20},
       .author = {.user_id = 42,
                  .username = "sanguinius",
                  .display_name = "Sanguinius",
                  .is_bot = true},
       .content = "Unrelated bot output."}));

  sanguinius::persistence::SqliteChronicleRepository chronicle{fixture.context};
  REQUIRE(chronicle
              .create_or_get_proposal(
                  {.entry_id = uuid(38'010),
                   .event_id = uuid(38'011),
                   .actions = {.edit_token_id = uuid(38'012),
                               .submit_token_id = uuid(38'013),
                               .retract_token_id = uuid(38'014)},
                   .source = {.reference = {.message_id = 9'010,
                                            .guild_id = 10,
                                            .channel_id = 20},
                              .author = {.user_id = 42,
                                         .username = "sanguinius",
                                         .display_name = "Sanguinius",
                                         .is_bot = true},
                              .content = "Unrelated bot output.",
                              .occurred_at_ms = 1'002},
                   .proposer_user_id = 30,
                   .owner_user_id = 30,
                   .title = "Invalid appearance source",
                   .body = "Unrelated bot output.",
                   .owner_test = true,
                   .appearance_decision_id = decision_id,
                   .correlation_id = "reverse-reservation",
                   .idempotency_key = "reverse-reservation-proposal",
                   .now_ms = 1'003,
                   .action_expires_at_ms = 10'000,
                   .notice_expires_at_ms = 20'000})
              .code == sanguinius::ChronicleResultCode::unauthorized);
}

TEST_CASE("reservation-linked unrelated outbox rows fail in forward order",
          "[appearance][repository][outbox][migration][provenance]") {
  AppearanceFixture fixture;
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_dry_run",
                       .idempotency_key = "forward-reservation-candidate",
                       .correlation_id = "forward-reservation",
                       .owner_user_id = 30,
                       .now_ms = 1'000,
                       .candidate_id = uuid(38'100),
                       .event_id = uuid(38'101)});
  const auto decision_id = uuid(38'102);
  const auto outbox_id = uuid(38'103);
  auto &connection = fixture.context->connection();
  auto decision = connection.prepare(
      "INSERT INTO appearance_decision(decision_id,candidate_id,policy_version,"
      "application_instance_id,revision,state,action,reason,gate_json,score_"
      "json,score,human_message_count,model_status,serious_categories_json,"
      "created_at_ms,finalized_at_ms) VALUES(?,?,?,'forward-reservation',1,"
      "'final','live_queued','live_queued','[]','[]',80,0,'owner_fixture',"
      "'[]',1001,1001)");
  decision.bind(1, decision_id);
  decision.bind(2, candidate.candidate_id);
  decision.bind(3, fixture.policy.policy_version);
  decision.execute();

  connection.execute("BEGIN IMMEDIATE");
  auto reservation = connection.prepare(
      "INSERT INTO appearance_budget_reservation(reservation_id,decision_id,"
      "candidate_id,outbox_id,idempotency_key,reserved_at_ms,human_message_"
      "count,is_test) VALUES(?,?,?,?,?,1001,0,1)");
  reservation.bind(1, uuid(38'104));
  reservation.bind(2, decision_id);
  reservation.bind(3, candidate.candidate_id);
  reservation.bind(4, outbox_id);
  reservation.bind(5, "appearance.reservation:" + decision_id);
  reservation.execute();

  auto outbox = connection.prepare(
      "INSERT INTO outbox_message(outbox_id,kind,aggregate_type,aggregate_id,"
      "target_guild_id,target_channel_id,payload_json,state,attempt_count,max_"
      "attempts,idempotency_key,provider_nonce,created_at_ms,available_at_ms,"
      "updated_at_ms) VALUES(?,'discord.public.v1','chronicle_entry',?,"
      "'10','20',?,'pending',0,5,'unrelated-forward-public-row',"
      "'0000000000000000000000039',1001,1001,1001)");
  outbox.bind(1, outbox_id);
  outbox.bind(2, uuid(38'999));
  outbox.bind(3, R"({"content":"Unrelated forward bot output."})");
  REQUIRE_THROWS(outbox.execute());
  connection.execute("ROLLBACK");

  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
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
  REQUIRE(recovered.front().memory_available);
  REQUIRE_FALSE(recovered.front().consented);
  REQUIRE(recovered.front().visible);
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
  REQUIRE(memory_gate->passed);
  REQUIRE_FALSE(consent_gate->passed);
  REQUIRE(visibility_gate->passed);
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
  constexpr std::int64_t eight_days = 8LL * 24 * 60 * 60 * 1'000;
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A remembered victory.",
      .tone = "warm",
      .memory_ids_used = {uuid(1'201)},
      .confidence = .95};
  const auto require_history = [&](const sanguinius::AppearanceMode mode) {
    const auto second = extract(100'000 + eight_days, 1'600);
    REQUIRE(second.novelty_age_ms.has_value());
    REQUIRE(second.repetition_age_ms.has_value());
    REQUIRE(*second.novelty_age_ms >= eight_days - 20);
    REQUIRE(*second.repetition_age_ms >= eight_days - 20);
    const auto second_evaluation = sanguinius::evaluate_appearance(
        fixture.policy, mode, second, 100'010 + eight_days);
    REQUIRE(std::ranges::any_of(
        second_evaluation.score_components, [](const auto &component) {
          return component.name == "repetition" && component.points == -25;
        }));
  };

  SECTION("dry-run decisions supply recurrence history") {
    const auto first = extract(100'000, 1'300);
    const auto first_evaluation = sanguinius::evaluate_appearance(
        fixture.policy, sanguinius::AppearanceMode::dry_run, first, 100'010);
    REQUIRE(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::dry_run, first,
        first_evaluation, uuid(1'500), uuid(1'501), "history-instance",
        "model_accepted", model, 100'010));
    require_history(sanguinius::AppearanceMode::dry_run);
  }

  SECTION("live reservations supply recurrence history") {
    fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 90'000);
    const auto first = extract(100'000, 1'300);
    const auto first_evaluation = sanguinius::evaluate_appearance(
        fixture.policy, sanguinius::AppearanceMode::live, first, 100'010);
    REQUIRE(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::live, first,
        first_evaluation, uuid(1'500), uuid(1'501), "history-instance",
        "model_accepted", model,
        {.reservation_id = uuid(1'502),
         .outbox_id = uuid(1'503),
         .feedback_control_ids = {uuid(1'504), uuid(1'505), uuid(1'506),
                                  uuid(1'507)}},
        100'010));
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM appearance_budget_reservation") == 1);
    require_history(sanguinius::AppearanceMode::live);
  }
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
                 "reason='daily_budget'") == 1);
  const auto decisions = fixture.repository->recent(10);
  const auto rejected =
      std::ranges::find(decisions, std::string{"daily_budget"},
                        &sanguinius::AppearanceDecisionRecord::reason);
  REQUIRE(rejected != decisions.end());
  const auto budget_gate =
      std::ranges::find(rejected->gates, std::string{"daily_budget"},
                        &sanguinius::AppearanceGate::name);
  REQUIRE(budget_gate != rejected->gates.end());
  REQUIRE_FALSE(budget_gate->passed);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("independent SQLite connections reserve one automatic live budget",
          "[appearance][repository][live][concurrency]") {
  AppearanceFixture fixture;
  auto race_policy = nlohmann::json::parse(fixture.policy.canonical_json);
  race_policy["policy_version"] = "m10-live-race";
  race_policy["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(race_policy.dump());
  fixture.repository->register_policy(fixture.policy, 39'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 40'000);
  const auto candidates =
      conversation_candidates(fixture, 16, 10'000, 50'000, 10'000);
  REQUIRE(candidates.size() == 2);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A safe automatic live appearance.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  const auto evaluation = [&](const auto &candidate) {
    return sanguinius::evaluate_appearance(
        fixture.policy, sanguinius::AppearanceMode::live, candidate, 50'020);
  };
  const auto first_prepared = evaluation(candidates[0]);
  const auto second_prepared = evaluation(candidates[1]);
  INFO("first reason: " << first_prepared.reason);
  INFO("second reason: " << second_prepared.reason);
  REQUIRE(first_prepared.eligible_for_model);
  REQUIRE(second_prepared.eligible_for_model);
  std::barrier start{3};
  std::atomic_bool first_created{};
  std::atomic_bool second_created{};
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  const auto finish = [&](const std::size_t index, const std::size_t base,
                          std::atomic_bool &created,
                          std::exception_ptr &error) {
    try {
      auto context = std::make_shared<SqliteRepositoryContext>(
          Database::open_runtime(fixture.temporary.path(), 2s));
      SqliteAppearanceRepository repository{context};
      start.arrive_and_wait();
      created = repository.record_final(
          fixture.policy, sanguinius::AppearanceMode::live, candidates[index],
          evaluation(candidates[index]), uuid(base), uuid(base + 1),
          "live-race", "model_accepted", model,
          {.reservation_id = uuid(base + 2),
           .outbox_id = uuid(base + 3),
           .feedback_control_ids = {uuid(base + 4), uuid(base + 5),
                                    uuid(base + 6), uuid(base + 7)}},
          50'020);
    } catch (...) {
      error = std::current_exception();
    }
  };
  std::thread first{finish, 0, 11'000, std::ref(first_created),
                    std::ref(first_error)};
  std::thread second{finish, 1, 12'000, std::ref(second_created),
                     std::ref(second_error)};
  start.arrive_and_wait();
  first.join();
  second.join();
  if (first_error)
    std::rethrow_exception(first_error);
  if (second_error)
    std::rethrow_exception(second_error);
  REQUIRE(first_created.load());
  REQUIRE(second_created.load());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation WHERE "
                 "is_test=0") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE "
                 "action='live_queued'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE "
                 "reason='daily_budget'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 1);
}

TEST_CASE(
    "a mode change invalidates old candidates but a same-mode restart does not",
    "[appearance][repository][live][epoch][restart]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 1'000);
  const auto stale = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "old-epoch",
                       .correlation_id = "old-epoch",
                       .owner_user_id = 30,
                       .now_ms = 2'000,
                       .candidate_id = uuid(13'000),
                       .event_id = uuid(13'001)});
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 3'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 4'000);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "An old epoch must remain silent.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, stale,
      sanguinius::evaluate_appearance(
          fixture.policy, sanguinius::AppearanceMode::live, stale, 4'001),
      uuid(13'002), uuid(13'003), "epoch-restart", "model_accepted", model,
      {.reservation_id = uuid(13'004),
       .outbox_id = uuid(13'005),
       .feedback_control_ids = {uuid(13'006), uuid(13'007), uuid(13'008),
                                uuid(13'009)}},
      4'001));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE "
                 "reason='mode_epoch'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms FROM appearance_mode_state") == 3'000);

  const auto current = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "same-epoch-restart",
                       .correlation_id = "same-epoch-restart",
                       .owner_user_id = 30,
                       .now_ms = 4'002,
                       .candidate_id = uuid(13'010),
                       .event_id = uuid(13'011)});
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 4'003);
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, current,
      sanguinius::evaluate_appearance(
          fixture.policy, sanguinius::AppearanceMode::live, current, 4'004),
      uuid(13'012), uuid(13'013), "same-epoch-restart", "owner_fixture", model,
      {.reservation_id = uuid(13'014),
       .outbox_id = uuid(13'015),
       .feedback_control_ids = {uuid(13'016), uuid(13'017), uuid(13'018),
                                uuid(13'019)}},
      4'004));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_decision WHERE "
                 "reason='live_queued'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms FROM appearance_mode_state") == 3'000);
}

TEST_CASE("mode epochs advance despite duplicate or backward wall time",
          "[appearance][repository][epoch][restart][clock]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 1'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms FROM appearance_mode_state") == 1'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms FROM appearance_mode_state") == 1'001);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 500);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms FROM appearance_mode_state") == 1'002);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 200);
  REQUIRE(scalar(*fixture.context,
                 "SELECT activated_at_ms=1002 AND updated_at_ms>=1002 "
                 "FROM appearance_mode_state") == 1);
}

TEST_CASE("rollback dry-run gates retain recent live reservation history",
          "[appearance][repository][live][dry-run][rollback][budget]") {
  AppearanceFixture fixture;
  auto rollback_policy = nlohmann::json::parse(fixture.policy.canonical_json);
  rollback_policy["policy_version"] = "m10-live-rollback-budget";
  rollback_policy["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(rollback_policy.dump());
  fixture.repository->register_policy(fixture.policy, 90'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 90'001);
  const auto first =
      conversation_candidates(fixture, 8, 40'000, 100'000, 40'000);
  REQUIRE(first.size() == 1);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A rollback budget history fixture.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, first.front(),
      sanguinius::evaluate_appearance(fixture.policy,
                                      sanguinius::AppearanceMode::live,
                                      first.front(), 100'010),
      uuid(40'200), uuid(40'201), "rollback-budget", "model_accepted", model,
      {.reservation_id = uuid(40'202),
       .outbox_id = uuid(40'203),
       .feedback_control_ids = {uuid(40'204), uuid(40'205), uuid(40'206),
                                uuid(40'207)}},
      100'010));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation WHERE "
                 "is_test=0") == 1);

  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run,
                                    100'020);
  const auto rollback =
      conversation_candidates(fixture, 8, 41'000, 100'030, 41'000);
  REQUIRE(rollback.size() == 1);
  REQUIRE_FALSE(rollback.front().budget_available);
  REQUIRE_FALSE(rollback.front().gap_available);
  REQUIRE(rollback.front().messages_after_previous);
  REQUIRE(sanguinius::evaluate_appearance(fixture.policy,
                                          sanguinius::AppearanceMode::dry_run,
                                          rollback.front(), 100'040)
              .reason == "daily_budget");
}

TEST_CASE(
    "live finalization atomically reserves queues and creates opaque feedback",
    "[appearance][repository][live][outbox]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "live-safe-one",
                       .correlation_id = "live-safe",
                       .owner_user_id = 30,
                       .now_ms = 2'000,
                       .candidate_id = uuid(3'001),
                       .event_id = uuid(3'002)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, 2'001);
  REQUIRE(evaluation.eligible_for_model);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "[TEST] A safe live appearance.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 1.0};
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(3'003),
      .outbox_id = uuid(3'004),
      .feedback_control_ids = {uuid(3'005), uuid(3'006), uuid(3'007),
                               uuid(3'008)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, evaluation,
      uuid(3'009), uuid(3'010), "instance-live", "owner_fixture", model,
      delivery, 2'001));
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM appearance_decision "
                                   "WHERE action='live_queued'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation WHERE "
                 "is_test=1") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_feedback_control") == 4);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "kind='discord.public.v1' AND state='pending'") == 1);
  auto payload = fixture.context->connection().prepare(
      "SELECT payload_json,provider_nonce FROM outbox_message");
  REQUIRE(payload.step());
  const auto json = nlohmann::json::parse(payload.column_text(0));
  REQUIRE(json.at("content") == model.text);
  REQUIRE(json.at("correlation_id") == "appearance-live");
  REQUIRE(payload.column_text(0).find(candidate.candidate_id) ==
          std::string::npos);
  REQUIRE(payload.column_text(0).find(candidate.safe_summary) ==
          std::string::npos);
  REQUIRE(payload.column_text(0).find(candidate.excerpts.front()) ==
          std::string::npos);
  REQUIRE(json.at("embed").is_null());
  REQUIRE(json.at("allowed_user_mentions").empty());
  REQUIRE(json.at("buttons").size() == 4);
  for (const auto &button : json.at("buttons")) {
    REQUIRE(button.at("style") == "secondary");
    REQUIRE(button.at("custom_id").get<std::string>().starts_with("sga:1:"));
  }
  REQUIRE(payload.column_text(1) ==
          sanguinius::discord_nonce_from_uuid(delivery.outbox_id));
  REQUIRE(fixture.repository->public_outbox_violation_count() == 0);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE outbox_message SET payload_json='{}' WHERE outbox_id='" +
      delivery.outbox_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE outbox_message SET provider_nonce='0000000000000000000000000' "
      "WHERE outbox_id='" +
      delivery.outbox_id + "'"));
}

TEST_CASE("delivered appearance nonce and provider receipt survive restart",
          "[appearance][repository][live][outbox][restart]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "live-receipt-restart",
                       .correlation_id = "live-receipt-restart",
                       .owner_user_id = 30,
                       .now_ms = 2'000,
                       .candidate_id = uuid(35'000),
                       .event_id = uuid(35'001)});
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "[TEST] A restart-safe delivered appearance.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 1.0};
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(35'002),
      .outbox_id = uuid(35'003),
      .feedback_control_ids = {uuid(35'004), uuid(35'005), uuid(35'006),
                               uuid(35'007)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidate,
      sanguinius::evaluate_appearance(
          fixture.policy, sanguinius::AppearanceMode::live, candidate, 2'001),
      uuid(35'008), uuid(35'009), "receipt-restart", "owner_fixture", model,
      delivery, 2'001));
  SqliteDurableWorkRepository durable{fixture.context};
  const auto claimed = durable.claim_due_outbox(2'001, 3'000, "receipt-worker",
                                                uuid(35'010), true);
  REQUIRE(claimed);
  REQUIRE(claimed->provider_nonce ==
          sanguinius::discord_nonce_from_uuid(delivery.outbox_id));
  REQUIRE(durable.mark_public_outbox_submitted(
              *claimed,
              {.wall_time_ms = 2'002,
               .elapsed_realtime_ms = 400,
               .boot_session_id = "appearance-receipt-boot"},
              3'000) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*claimed, 9'002, 2'003) ==
          sanguinius::WorkMutationStatus::applied);

  auto restarted_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteDurableWorkRepository restarted_durable{restarted_context};
  SqliteAppearanceRepository restarted_appearance{restarted_context};
  REQUIRE_FALSE(restarted_durable.claim_due_outbox(
      4'000, 5'000, "restart-worker", uuid(35'011), true));
  REQUIRE(restarted_appearance.verify_public_delivery(
      {.reference = {.message_id = 9'002, .guild_id = 10, .channel_id = 20},
       .author = {.user_id = 42,
                  .username = "sanguinius",
                  .display_name = "Sanguinius",
                  .is_bot = true},
       .content = model.text}));
  REQUIRE(scalar(*restarted_context,
                 "SELECT provider_message_id='9002' AND state='delivered' "
                 "FROM outbox_message WHERE outbox_id='" +
                     delivery.outbox_id + "'") == 1);
  REQUIRE(scalar(*restarted_context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 1);
}

TEST_CASE("live feedback is private replay safe and quiet is independent",
          "[appearance][repository][live][feedback][privacy]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  const auto candidate = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "feedback-live",
                       .correlation_id = "feedback-live",
                       .owner_user_id = 30,
                       .now_ms = 2'000,
                       .candidate_id = uuid(3'101),
                       .event_id = uuid(3'102)});
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, 2'001);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "[TEST] Feedback provenance.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 1.0};
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(3'103),
      .outbox_id = uuid(3'104),
      .feedback_control_ids = {uuid(3'105), uuid(3'106), uuid(3'107),
                               uuid(3'108)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidate, evaluation,
      uuid(3'109), uuid(3'110), "instance-live", "owner_fixture", model,
      delivery, 2'001));
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='delivered',provider_message_id='9001',"
      "delivered_at_ms=2100,terminal_at_ms=2100,updated_at_ms=2100");

  auto feedback = sanguinius::AppearanceFeedbackMutation{
      .actor_user_id = 31,
      .guild_id = 10,
      .channel_id = 20,
      .action = sanguinius::AppearanceFeedbackAction::more,
      .control_id = delivery.feedback_control_ids[0],
      .reference = std::nullopt,
      .quiet_until_ms = 10'000,
      .feedback_id = uuid(3'111),
      .event_id = uuid(3'112),
      .idempotency_key = "feedback-click-one",
      .correlation_id = "feedback",
      .now_ms = 3'000};
  REQUIRE(fixture.repository->record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE(fixture.repository->record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::unchanged);
  auto replay_conflict = feedback;
  replay_conflict.control_id = delivery.feedback_control_ids[1];
  REQUIRE_THROWS(fixture.repository->record_feedback(replay_conflict));
  feedback.control_id = delivery.feedback_control_ids[1];
  feedback.feedback_id = uuid(3'113);
  feedback.event_id = uuid(3'114);
  feedback.idempotency_key = "feedback-click-conflict";
  REQUIRE(fixture.repository->record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::conflict);

  feedback.control_id = std::nullopt;
  feedback.action = sanguinius::AppearanceFeedbackAction::more;
  feedback.feedback_id = uuid(3'117);
  feedback.event_id = uuid(3'118);
  feedback.idempotency_key = "feedback-slash-fallback-duplicate";
  REQUIRE(fixture.repository->record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::unchanged);

  feedback.control_id = delivery.feedback_control_ids[3];
  feedback.feedback_id = uuid(3'115);
  feedback.event_id = uuid(3'116);
  feedback.idempotency_key = "feedback-click-quiet";
  REQUIRE(fixture.repository->record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::quiet_applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_feedback") == 2);
  REQUIRE(scalar(*fixture.context, "SELECT quiet_until_ms FROM "
                                   "appearance_control_state") == 10'000);
  REQUIRE(fixture.repository
              ->verify_public_delivery(sanguinius::ContextMessageSnapshot{
                  .reference = {.message_id = 9'001,
                                .guild_id = 10,
                                .channel_id = 20},
                  .author = {.user_id = 42,
                             .username = {},
                             .display_name = {},
                             .is_bot = true},
                  .content = model.text})
              .has_value());
  REQUIRE_FALSE(fixture.repository->verify_public_delivery(
      sanguinius::ContextMessageSnapshot{
          .reference = {.message_id = 9'001, .guild_id = 10, .channel_id = 20},
          .author = {.user_id = 42,
                     .username = {},
                     .display_name = {},
                     .is_bot = true},
          .content = "mismatched public text"}));

  sanguinius::persistence::SqliteChronicleRepository chronicle{fixture.context};
  const auto proposal = [&](const std::size_t base, std::string content) {
    return sanguinius::CreateProposalRequest{
        .entry_id = uuid(base),
        .event_id = uuid(base + 1),
        .actions = {.edit_token_id = uuid(base + 2),
                    .submit_token_id = uuid(base + 3),
                    .retract_token_id = uuid(base + 4)},
        .source = {.reference = {.message_id = 9'001,
                                 .guild_id = 10,
                                 .channel_id = 20},
                   .author = {.user_id = 42,
                              .username = "sanguinius",
                              .display_name = "Sanguinius",
                              .is_bot = true},
                   .content = std::move(content),
                   .occurred_at_ms = 2'100},
        .proposer_user_id = 30,
        .owner_user_id = 30,
        .title = "A delivered appearance",
        .body = model.text,
        .owner_test = true,
        .appearance_decision_id = uuid(3'109),
        .correlation_id = "appearance-chronicle",
        .idempotency_key = "appearance-chronicle:" + std::to_string(base),
        .now_ms = 3'100,
        .action_expires_at_ms = 10'000,
        .notice_expires_at_ms = 20'000};
  };
  const auto accepted =
      chronicle.create_or_get_proposal(proposal(26'000, model.text));
  REQUIRE(accepted.code == sanguinius::ChronicleResultCode::created);
  REQUIRE(accepted.entry);
  REQUIRE(accepted.entry->status == sanguinius::ChronicleEntryStatus::proposed);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_entry WHERE status='canon'") ==
          0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_appearance_source") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_participant WHERE "
                 "user_id='42'") == 0);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM chronicle_appearance_source WHERE entry_id='" +
      uuid(26'000) + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM chronicle_entry WHERE entry_id='" + uuid(26'000) + "'"));
  REQUIRE(
      chronicle
          .create_or_get_proposal(proposal(27'000, "mismatched public text"))
          .code == sanguinius::ChronicleResultCode::unauthorized);

  auto restarted_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteAppearanceRepository restarted{restarted_context};
  REQUIRE(restarted.control_summary(3'100).quiet_until_ms ==
          std::optional<std::int64_t>{10'000});
  REQUIRE(restarted.record_feedback(feedback) ==
          sanguinius::AppearanceMutationResult::unchanged);
  REQUIRE(restarted.verify_public_delivery(sanguinius::ContextMessageSnapshot{
      .reference = {.message_id = 9'001, .guild_id = 10, .channel_id = 20},
      .author = {.user_id = 42,
                 .username = "sanguinius",
                 .display_name = "Sanguinius",
                 .is_bot = true},
      .content = model.text}));
  auto expired_slash = feedback;
  expired_slash.actor_user_id = 32;
  expired_slash.control_id = std::nullopt;
  expired_slash.reference = std::nullopt;
  expired_slash.action = sanguinius::AppearanceFeedbackAction::more;
  expired_slash.feedback_id = uuid(3'119);
  expired_slash.event_id = uuid(3'120);
  expired_slash.idempotency_key = "feedback-slash-expired";
  expired_slash.now_ms = 3'000'000'000;
  REQUIRE(restarted.record_feedback(expired_slash) ==
          sanguinius::AppearanceMutationResult::not_found);
}

TEST_CASE("server quiet authorization and kill switch cancel only unsent work",
          "[appearance][repository][live][quiet][kill]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  const auto start =
      fixture.repository->set_quiet({.actor_user_id = 31,
                                     .quiet_until_ms = 10'000,
                                     .reason = "duration",
                                     .request_value = "2h",
                                     .event_id = uuid(3'201),
                                     .idempotency_key = "quiet-start",
                                     .correlation_id = "quiet",
                                     .now_ms = 2'000});
  REQUIRE(start == sanguinius::AppearanceMutationResult::applied);
  auto restarted_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteAppearanceRepository restarted{restarted_context};
  REQUIRE(restarted.set_quiet({.actor_user_id = 31,
                               .quiet_until_ms = 11'000,
                               .reason = "duration",
                               .request_value = "2h",
                               .event_id = uuid(3'207),
                               .idempotency_key = "quiet-start",
                               .correlation_id = "quiet",
                               .now_ms = 2'001}) ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE_THROWS(
      fixture.repository->set_quiet({.actor_user_id = 31,
                                     .quiet_until_ms = 11'000,
                                     .reason = "tonight",
                                     .request_value = {},
                                     .event_id = uuid(3'208),
                                     .idempotency_key = "quiet-start",
                                     .correlation_id = "quiet",
                                     .now_ms = 2'002}));
  auto replay_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteAppearanceRepository replayed{replay_context};
  REQUIRE(replayed.set_quiet({.actor_user_id = 32,
                              .quiet_until_ms = 9'000,
                              .reason = "duration",
                              .request_value = "2h",
                              .event_id = uuid(3'209),
                              .idempotency_key = "quiet-noop",
                              .correlation_id = "quiet",
                              .now_ms = 2'050}) ==
          sanguinius::AppearanceMutationResult::unchanged);
  REQUIRE(replayed.set_quiet({.actor_user_id = 32,
                              .quiet_until_ms = std::nullopt,
                              .reason = {},
                              .request_value = {},
                              .event_id = uuid(3'202),
                              .idempotency_key = "quiet-clear-other",
                              .correlation_id = "quiet",
                              .now_ms = 2'100}) ==
          sanguinius::AppearanceMutationResult::unauthorized);
  REQUIRE(
      fixture.repository->set_quiet({.actor_user_id = 31,
                                     .quiet_until_ms = std::nullopt,
                                     .reason = {},
                                     .request_value = {},
                                     .event_id = uuid(3'203),
                                     .idempotency_key = "quiet-clear-setter",
                                     .correlation_id = "quiet",
                                     .now_ms = 2'200}) ==
      sanguinius::AppearanceMutationResult::applied);
  auto outcome_replay_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteAppearanceRepository outcome_replay{outcome_replay_context};
  REQUIRE(outcome_replay.set_quiet({.actor_user_id = 32,
                                    .quiet_until_ms = 9'500,
                                    .reason = "duration",
                                    .request_value = "2h",
                                    .event_id = uuid(3'210),
                                    .idempotency_key = "quiet-noop",
                                    .correlation_id = "quiet",
                                    .now_ms = 2'201}) ==
          sanguinius::AppearanceMutationResult::unchanged);
  REQUIRE(outcome_replay.set_quiet({.actor_user_id = 32,
                                    .quiet_until_ms = std::nullopt,
                                    .reason = {},
                                    .request_value = {},
                                    .event_id = uuid(3'211),
                                    .idempotency_key = "quiet-clear-other",
                                    .correlation_id = "quiet",
                                    .now_ms = 2'202}) ==
          sanguinius::AppearanceMutationResult::unauthorized);
  REQUIRE_FALSE(
      fixture.repository->control_summary(2'203).quiet_until_ms.has_value());
  REQUIRE(fixture.repository->set_global_disabled(31, true, 2'300, uuid(3'204),
                                                  "kill-not-owner", "kill") ==
          sanguinius::AppearanceMutationResult::unauthorized);
  REQUIRE(fixture.repository->set_global_disabled(30, true, 2'400, uuid(3'205),
                                                  "kill-owner", "kill") ==
          sanguinius::AppearanceMutationResult::applied);
  const auto state = fixture.repository->control_summary(2'500);
  REQUIRE(state.globally_disabled);
  REQUIRE(state.persisted_mode == sanguinius::AppearanceMode::live);
  REQUIRE(fixture.repository->set_global_disabled(30, false, 2'600, uuid(3'206),
                                                  "kill-owner-clear", "kill") ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE_FALSE(fixture.repository->control_summary(2'700).globally_disabled);
}

TEST_CASE("quiet cancels unsubmitted claims while submitted work reconciles",
          "[appearance][repository][live][quiet][outbox]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "[TEST] A cancellation boundary fixture.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 1.0};
  const auto finalize = [&](const std::int64_t candidate_time,
                            const std::int64_t final_time,
                            const std::size_t base) {
    const auto candidate = fixture.repository->simulate(
        fixture.policy,
        {.fixture = "owner_live_safe",
         .idempotency_key = "cancel-boundary-" + std::to_string(base),
         .correlation_id = "cancel-boundary",
         .owner_user_id = 30,
         .now_ms = candidate_time,
         .candidate_id = uuid(base),
         .event_id = uuid(base + 1)});
    REQUIRE(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::live, candidate,
        sanguinius::evaluate_appearance(fixture.policy,
                                        sanguinius::AppearanceMode::live,
                                        candidate, final_time),
        uuid(base + 2), uuid(base + 3), "cancel-boundary", "owner_fixture",
        model,
        {.reservation_id = uuid(base + 4),
         .outbox_id = uuid(base + 5),
         .feedback_control_ids = {uuid(base + 6), uuid(base + 7),
                                  uuid(base + 8), uuid(base + 9)}},
        final_time));
    return uuid(base + 5);
  };
  SqliteDurableWorkRepository durable{fixture.context};
  const auto cancellable_id = finalize(2'000, 2'001, 24'000);
  const auto cancellable = durable.claim_due_outbox(
      2'001, 3'000, "cancel-worker", uuid(24'100), true);
  REQUIRE(cancellable);
  REQUIRE(cancellable->outbox_id == cancellable_id);
  REQUIRE_FALSE(cancellable->submission_started_at_ms);
  REQUIRE(
      fixture.repository->set_quiet({.actor_user_id = 31,
                                     .quiet_until_ms = 10'000,
                                     .reason = "duration",
                                     .request_value = "2h",
                                     .event_id = uuid(24'101),
                                     .idempotency_key = "quiet-cancel-claim",
                                     .correlation_id = "cancel-boundary",
                                     .now_ms = 2'100}) ==
      sanguinius::AppearanceMutationResult::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT state='cancelled' FROM outbox_message WHERE "
                 "outbox_id='" +
                     cancellable_id + "'") == 1);
  REQUIRE(fixture.repository->set_quiet(
              {.actor_user_id = 31,
               .quiet_until_ms = std::nullopt,
               .reason = {},
               .request_value = {},
               .event_id = uuid(24'102),
               .idempotency_key = "quiet-clear-for-submit",
               .correlation_id = "cancel-boundary",
               .now_ms = 2'200}) ==
          sanguinius::AppearanceMutationResult::applied);

  constexpr std::int64_t later = 90'000'000;
  const auto submitted_id = finalize(later, later + 1, 25'000);
  const auto submitted = durable.claim_due_outbox(
      later + 1, later + 10'000, "submit-worker", uuid(25'100), true);
  REQUIRE(submitted);
  REQUIRE(submitted->outbox_id == submitted_id);
  REQUIRE(durable.mark_public_outbox_submitted(
              *submitted,
              {.wall_time_ms = later + 2,
               .elapsed_realtime_ms = 500,
               .boot_session_id = "appearance-test-boot"},
              later + 10'000) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.fail_outbox(*submitted, later + 3, later + 1'000,
                              "discord_unknown_outcome",
                              sanguinius::OutboxFailureMode::retryable) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE(fixture.repository->set_global_disabled(
              30, true, later + 4, uuid(25'101), "kill-after-submission",
              "cancel-boundary") ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT state='pending' AND first_attempt_at_ms IS NOT NULL "
             "AND submission_started_at_ms IS NULL AND last_error_code="
             "'discord_unknown_outcome' FROM outbox_message WHERE outbox_id='" +
                 submitted_id + "'") == 1);
  REQUIRE(fixture.repository->control_summary(later + 5).ambiguous_outbox == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 2);
}

TEST_CASE(
    "preference memory and Chronicle withdrawals cancel unsent appearances",
    "[appearance][repository][live][privacy][outbox]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 1'000);
  struct QueuedAppearance {
    std::string candidate_id;
    std::string decision_id;
    std::string outbox_id;
  };
  const auto queue = [&](const std::size_t base) {
    const auto candidate = fixture.repository->simulate(
        fixture.policy,
        {.fixture = "owner_live_safe",
         .idempotency_key = "withdrawal-candidate-" + std::to_string(base),
         .correlation_id = "withdrawal",
         .owner_user_id = 30,
         .now_ms = 2'000,
         .candidate_id = uuid(base),
         .event_id = uuid(base + 1)});
    const auto decision_id = uuid(base + 2);
    const auto outbox_id = uuid(base + 4);
    REQUIRE(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::live, candidate,
        sanguinius::evaluate_appearance(
            fixture.policy, sanguinius::AppearanceMode::live, candidate, 2'001),
        decision_id, uuid(base + 3), "withdrawal", "owner_fixture",
        sanguinius::AppearanceModelResult{
            .serious_context = false,
            .serious_categories = {},
            .should_speak = true,
            .text = "[TEST] An unsent privacy-withdrawal fixture.",
            .tone = "warm",
            .memory_ids_used = {},
            .confidence = 1.0},
        {.reservation_id = uuid(base + 5),
         .outbox_id = outbox_id,
         .feedback_control_ids = {uuid(base + 6), uuid(base + 7),
                                  uuid(base + 8), uuid(base + 9)}},
        2'001));
    return QueuedAppearance{candidate.candidate_id, decision_id, outbox_id};
  };
  const auto cancelled = [&](const QueuedAppearance &queued,
                             const std::string_view reason) {
    auto state = fixture.context->connection().prepare(
        "SELECT state,last_error_code FROM outbox_message WHERE outbox_id=?");
    state.bind(1, queued.outbox_id);
    REQUIRE(state.step());
    REQUIRE(state.column_text(0) == "cancelled");
    REQUIRE(state.column_text(1) == reason);
  };

  SECTION("appearance callback opt-out") {
    const auto queued = queue(36'000);
    REQUIRE(fixture.repository->set_callback_consent(
        30, false, 2'100, uuid(36'100), "withdrawal-callback", "withdrawal"));
    cancelled(queued, "appearance_opt_out");
  }

  SECTION("unrelated memory callback withdrawal preserves conversation work") {
    const auto queued = queue(36'200);
    fixture.context->connection().execute(
        "UPDATE user_preference SET memory_callback_opt_in=0,"
        "updated_at_ms=2100 WHERE user_id='30'");
    REQUIRE(
        scalar(*fixture.context,
               "SELECT state='pending' FROM outbox_message WHERE outbox_id='" +
                   queued.outbox_id + "'") == 1);
  }

  SECTION("memory callback withdrawal cancels memory-dependent work") {
    const auto queued = queue(36'300);
    SqliteDurableWorkRepository durable{fixture.context};
    REQUIRE(durable.append_event(
        {.event_id = uuid(36'400),
         .event_type = "chronicle.memory_confirmed.v1",
         .aggregate_type = "memory",
         .aggregate_id = uuid(36'401),
         .actor_user_id = 31,
         .guild_id = 10,
         .channel_id = 20,
         .source_message_id = std::nullopt,
         .occurred_at_ms = 1'500,
         .recorded_at_ms = 1'500,
         .correlation_id = "withdrawal",
         .causation_id = std::nullopt,
         .idempotency_key = "withdrawal-memory-callback-event",
         .payload_json = "{}"}));
    auto &connection = fixture.context->connection();
    connection.execute_script(
        "INSERT INTO memory(memory_id,memory_type,text,visibility,"
        "sensitivity,status,confidence_basis,source_event_id,"
        "created_by_user_id,confirmed_by_user_id,created_at_ms,"
        "confirmed_at_ms,revision,creation_idempotency_key) VALUES("
        "'00000000-0000-4000-8000-000000036401','explicit',"
        "'A callback-dependent shared memory.','shared','ordinary','confirmed',"
        "'user_confirmed','00000000-0000-4000-8000-000000036400','31',"
        "'31',1500,1500,1,'withdrawal-memory-callback');"
        "INSERT INTO memory_subject(memory_id,subject_type,subject_id) VALUES("
        "'00000000-0000-4000-8000-000000036401','user','31');");
    auto link = connection.prepare(
        "INSERT INTO appearance_decision_memory VALUES(?,?,1,0,1)");
    link.bind(1, queued.decision_id);
    link.bind(2, uuid(36'401));
    link.execute();
    connection.execute("UPDATE user_preference SET memory_callback_opt_in=0,"
                       "updated_at_ms=2100 WHERE user_id='31'");
    cancelled(queued, "appearance_opt_out");
  }

  SECTION("memory retraction") {
    const auto queued = queue(37'000);
    SqliteDurableWorkRepository durable{fixture.context};
    REQUIRE(durable.append_event({.event_id = uuid(37'100),
                                  .event_type = "chronicle.memory_confirmed.v1",
                                  .aggregate_type = "memory",
                                  .aggregate_id = uuid(37'101),
                                  .actor_user_id = 30,
                                  .guild_id = 10,
                                  .channel_id = 20,
                                  .source_message_id = std::nullopt,
                                  .occurred_at_ms = 1'500,
                                  .recorded_at_ms = 1'500,
                                  .correlation_id = "withdrawal",
                                  .causation_id = std::nullopt,
                                  .idempotency_key = "withdrawal-memory-event",
                                  .payload_json = "{}"}));
    auto &connection = fixture.context->connection();
    connection.execute_script(
        "INSERT INTO memory(memory_id,memory_type,text,visibility,"
        "sensitivity,status,confidence_basis,source_event_id,"
        "created_by_user_id,confirmed_by_user_id,created_at_ms,"
        "confirmed_at_ms,revision,creation_idempotency_key) VALUES("
        "'00000000-0000-4000-8000-000000037101','explicit',"
        "'A retractable shared memory.','shared','ordinary','confirmed',"
        "'user_confirmed','00000000-0000-4000-8000-000000037100','30',"
        "'30',1500,1500,1,'withdrawal-memory');");
    auto link = connection.prepare(
        "INSERT INTO appearance_decision_memory VALUES(?,?,1,0,1)");
    link.bind(1, queued.decision_id);
    link.bind(2, uuid(37'101));
    link.execute();
    connection.execute(
        "UPDATE memory SET status='retracted',revision=2,retracted_at_ms=2100 "
        "WHERE memory_id='00000000-0000-4000-8000-000000037101'");
    cancelled(queued, "appearance_memory_withdrawn");
  }

  SECTION("Chronicle callback withdrawal") {
    const auto queued = queue(38'000);
    auto &connection = fixture.context->connection();
    connection.execute_script(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,"
        "visibility,status,occurred_at_ms,created_at_ms,created_by_user_id,"
        "submitted_at_ms,approved_at_ms,approved_by_user_id,source_guild_id,"
        "source_channel_id,source_message_id,source_author_user_id,source_text,"
        "revision,source_kind) VALUES("
        "'00000000-0000-4000-8000-000000038100','incident','Withdrawal',"
        "'A callback source.','shared','canon',1500,1500,'31',1500,1500,'30',"
        "'10','20','38100','31','A callback source.',1,'discord_message');"
        "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES("
        "'00000000-0000-4000-8000-000000038100','31','source_author');");
    auto link =
        connection.prepare("INSERT INTO appearance_candidate_source VALUES(?,"
                           "'chronicle_entry',?,1)");
    link.bind(1, queued.candidate_id);
    link.bind(2, uuid(38'100));
    link.execute();
    connection.execute(
        "UPDATE user_preference SET chronicle_opt_in=0,updated_at_ms=2100 "
        "WHERE user_id='31'");
    cancelled(queued, "appearance_opt_out");
  }

  SECTION("Chronicle opt-out preserves Tarot appearance callback authority") {
    const auto queued = queue(38'200);
    auto &connection = fixture.context->connection();
    connection.execute(
        "UPDATE appearance_candidate SET candidate_type='tarot_event' WHERE "
        "candidate_id='" +
        queued.candidate_id + "'");
    connection.execute_script(
        "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,"
        "visibility,status,occurred_at_ms,created_at_ms,created_by_user_id,"
        "submitted_at_ms,approved_at_ms,approved_by_user_id,source_guild_id,"
        "source_channel_id,source_message_id,source_author_user_id,source_text,"
        "revision,source_kind) VALUES("
        "'00000000-0000-4000-8000-000000038300','incident','Tarot source',"
        "'A Tarot callback source.','shared','canon',1500,1500,'31',1500,"
        "1500,'30','10','20','38300','31','A Tarot callback source.',1,"
        "'discord_message');"
        "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES("
        "'00000000-0000-4000-8000-000000038300','31','source_author');");
    auto link =
        connection.prepare("INSERT INTO appearance_candidate_source VALUES(?,"
                           "'chronicle_entry',?,1)");
    link.bind(1, queued.candidate_id);
    link.bind(2, uuid(38'300));
    link.execute();
    connection.execute(
        "UPDATE user_preference SET chronicle_opt_in=0,updated_at_ms=2100 "
        "WHERE user_id='31'");
    REQUIRE(
        scalar(*fixture.context,
               "SELECT state='pending' FROM outbox_message WHERE outbox_id='" +
                   queued.outbox_id + "'") == 1);
    REQUIRE(fixture.repository->set_callback_consent(
        31, false, 2'101, uuid(38'301), "tarot-callback-opt-out",
        "withdrawal"));
    cancelled(queued, "appearance_opt_out");
  }
}

TEST_CASE("event-backed Chronicle withdrawals cancel unsent appearances",
          "[appearance][repository][events][live][privacy][outbox]") {
  AppearanceFixture fixture;
  auto policy_json = nlohmann::json::parse(fixture.policy.canonical_json);
  policy_json["policy_version"] = "m10-event-withdrawal";
  policy_json["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(policy_json.dump());
  fixture.repository->register_policy(fixture.policy, 900);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 901);

  SqliteCoreIdentityRepository identities{fixture.context};
  identities.ensure_user({33, "Source", "source", false, 902});
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1,memory_callback_opt_in=1,"
      "appearance_callback_opt_in=1 WHERE user_id='33'");
  fixture.context->connection().execute_script(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,source_guild_id,source_channel_id,"
      "source_message_id,source_author_user_id,source_text,revision,source_"
      "kind) "
      "VALUES('00000000-0000-4000-8000-000000040000','incident','Shared "
      "source',"
      "'An ordinary shared Chronicle "
      "source.','shared','canon',500,500,'33',500,"
      "500,'30','10','20','40000','33','An ordinary shared Chronicle "
      "source.',1,"
      "'discord_message');"
      "INSERT INTO chronicle_participant(entry_id,user_id,role) VALUES("
      "'00000000-0000-4000-8000-000000040000','33','source_author');");

  const auto activity =
      conversation_candidates(fixture, 8, 40'010, 1'000, 40'020);
  REQUIRE(activity.size() == 1);
  SqliteDurableWorkRepository durable{fixture.context};
  REQUIRE(durable.append_event({.event_id = uuid(40'100),
                                .event_type = "chronicle.entry_canonized.v1",
                                .aggregate_type = "chronicle_entry",
                                .aggregate_id = uuid(40'000),
                                .actor_user_id = 33,
                                .guild_id = 10,
                                .channel_id = 20,
                                .source_message_id = std::nullopt,
                                .occurred_at_ms = 1'010,
                                .recorded_at_ms = 1'010,
                                .correlation_id = "event-withdrawal",
                                .causation_id = std::nullopt,
                                .idempotency_key = "event-withdrawal-source",
                                .payload_json = "{}"}));
  const auto candidates =
      fixture.repository->scan_events(fixture.policy, 1'010, "instance");
  REQUIRE(candidates.size() == 1);
  const auto evaluation = sanguinius::evaluate_appearance(
      fixture.policy, sanguinius::AppearanceMode::live, candidates.front(),
      1'011);
  REQUIRE(evaluation.eligible_for_model);
  const auto decision_id = uuid(40'101);
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(40'102),
      .outbox_id = uuid(40'103),
      .feedback_control_ids = {uuid(40'104), uuid(40'105), uuid(40'106),
                               uuid(40'107)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidates.front(),
      evaluation, decision_id, uuid(40'108), "event-withdrawal",
      "model_accepted",
      sanguinius::AppearanceModelResult{
          .serious_context = false,
          .serious_categories = {},
          .should_speak = true,
          .text = "A safe event-backed Chronicle observation.",
          .tone = "warm",
          .memory_ids_used = {},
          .confidence = .95},
      delivery, 1'011));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate_source WHERE "
                 "candidate_id='" +
                     candidates.front().candidate_id +
                     "' AND source_kind='event'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_candidate_source_user WHERE "
                 "candidate_id='" +
                     candidates.front().candidate_id + "' AND user_id='33'") ==
          1);
  const auto require_cancelled = [&](const std::string_view reason) {
    auto state = fixture.context->connection().prepare(
        "SELECT state,last_error_code FROM outbox_message WHERE outbox_id=?");
    state.bind(1, delivery.outbox_id);
    REQUIRE(state.step());
    REQUIRE(state.column_text(0) == "cancelled");
    REQUIRE(state.column_text(1) == reason);
  };

  SECTION("appearance callback withdrawal") {
    REQUIRE(fixture.repository->set_callback_consent(
        33, false, 1'012, uuid(40'109), "event-source-opt-out",
        "event-withdrawal"));
    require_cancelled("appearance_opt_out");
  }

  SECTION("Chronicle callback withdrawal") {
    fixture.context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=0,updated_at_ms=1012 "
        "WHERE user_id='33'");
    require_cancelled("appearance_opt_out");
  }

  SECTION("Chronicle source retraction") {
    fixture.context->connection().execute(
        "UPDATE chronicle_entry SET status='retracted',revision=2,"
        "retracted_at_ms=1012,retracted_by_user_id='30' WHERE entry_id='"
        "00000000-0000-4000-8000-000000040000'");
    require_cancelled("appearance_chronicle_withdrawn");
  }
}

TEST_CASE("model failure alerts are thresholded hourly and restart safe",
          "[appearance][repository][alerts][restart]") {
  AppearanceFixture fixture;
  fixture.repository->activate_mode(sanguinius::AppearanceMode::dry_run, 1'000);
  for (std::size_t index = 0; index < 3; ++index) {
    const auto candidate = fixture.repository->simulate(
        fixture.policy,
        {.fixture = "owner_dry_run",
         .idempotency_key = "alert-model-" + std::to_string(index),
         .correlation_id = "alert-model",
         .owner_user_id = 30,
         .now_ms = 2'000,
         .candidate_id = uuid(14'000 + index),
         .event_id = uuid(14'100 + index)});
    REQUIRE(fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::dry_run, candidate,
        sanguinius::evaluate_appearance(fixture.policy,
                                        sanguinius::AppearanceMode::dry_run,
                                        candidate, 3'000),
        uuid(14'200 + index), uuid(14'300 + index), "alert-instance",
        "model_timeout", std::nullopt, 3'000));
  }
  REQUIRE(fixture.repository->control_summary(3'000).recent_model_failures ==
          3);
  const auto first = fixture.repository->claim_failure_alerts(3'000);
  REQUIRE(first.size() == 1);
  REQUIRE(first.front().category == "model");
  REQUIRE(first.front().occurrences == 3);
  REQUIRE(fixture.repository->claim_failure_alerts(3'001).empty());

  auto restarted_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path(), 2s));
  SqliteAppearanceRepository restarted{restarted_context};
  REQUIRE(restarted.claim_failure_alerts(3'600'000 + 3'000).size() == 1);
}

TEST_CASE("test and automatic namespaces independently reach delivery alert "
          "threshold",
          "[appearance][repository][live][alerts]") {
  AppearanceFixture fixture;
  auto alert_policy = nlohmann::json::parse(fixture.policy.canonical_json);
  alert_policy["policy_version"] = "m10-live-alert";
  alert_policy["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(alert_policy.dump());
  fixture.repository->register_policy(fixture.policy, 90'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 90'001);
  const auto automatic =
      conversation_candidates(fixture, 8, 20'000, 100'000, 20'000);
  REQUIRE(automatic.size() == 1);
  const auto owner = fixture.repository->simulate(
      fixture.policy, {.fixture = "owner_live_safe",
                       .idempotency_key = "alert-owner-live",
                       .correlation_id = "alert-delivery",
                       .owner_user_id = 30,
                       .now_ms = 100'010,
                       .candidate_id = uuid(21'000),
                       .event_id = uuid(21'001)});
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A safe alert delivery fixture.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  const auto finalize = [&](const sanguinius::AppearanceCandidate &candidate,
                            const std::size_t base) {
    return fixture.repository->record_final(
        fixture.policy, sanguinius::AppearanceMode::live, candidate,
        sanguinius::evaluate_appearance(fixture.policy,
                                        sanguinius::AppearanceMode::live,
                                        candidate, 100'020),
        uuid(base), uuid(base + 1), "alert-delivery", "model_accepted", model,
        {.reservation_id = uuid(base + 2),
         .outbox_id = uuid(base + 3),
         .feedback_control_ids = {uuid(base + 4), uuid(base + 5),
                                  uuid(base + 6), uuid(base + 7)}},
        100'020);
  };
  REQUIRE(finalize(automatic.front(), 22'000));
  REQUIRE(finalize(owner, 23'000));
  REQUIRE(scalar(*fixture.context, "SELECT count(DISTINCT is_test) FROM "
                                   "appearance_budget_reservation") == 2);
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='failed',terminal_at_ms=100030,"
      "updated_at_ms=100030,last_error_code='discord_delivery_failed'");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM appearance_budget_reservation") == 2);
  const auto health = fixture.repository->control_summary(100'040);
  REQUIRE(health.recent_delivery_failures == 2);
  const auto alerts = fixture.repository->claim_failure_alerts(100'040);
  const auto delivery =
      std::ranges::find(alerts, std::string{"delivery"},
                        &sanguinius::AppearanceFailureAlert::category);
  REQUIRE(delivery != alerts.end());
  REQUIRE(delivery->occurrences == 2);
  REQUIRE(fixture.repository->claim_failure_alerts(100'041).empty());
}

TEST_CASE(
    "non-test feedback produces conservative policy and theme recommendations",
    "[appearance][repository][live][feedback][metrics]") {
  AppearanceFixture fixture;
  auto feedback_policy = nlohmann::json::parse(fixture.policy.canonical_json);
  feedback_policy["policy_version"] = "m10-live-feedback-metrics";
  feedback_policy["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(feedback_policy.dump());
  fixture.repository->register_policy(fixture.policy, 90'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 90'001);
  const auto candidates =
      conversation_candidates(fixture, 8, 30'000, 100'000, 30'000);
  REQUIRE(candidates.size() == 1);
  fixture.context->connection().execute(
      "UPDATE appearance_candidate SET theme_key='theme-review-fixture' "
      "WHERE candidate_id='" +
      candidates.front().candidate_id + "'");
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A safe feedback metrics fixture.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(31'000),
      .outbox_id = uuid(31'001),
      .feedback_control_ids = {uuid(31'002), uuid(31'003), uuid(31'004),
                               uuid(31'005)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidates.front(),
      sanguinius::evaluate_appearance(fixture.policy,
                                      sanguinius::AppearanceMode::live,
                                      candidates.front(), 100'020),
      uuid(31'006), uuid(31'007), "feedback-metrics", "model_accepted", model,
      delivery, 100'020));
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='delivered',"
      "provider_message_id='31001',delivered_at_ms=100030,"
      "terminal_at_ms=100030,updated_at_ms=100030");
  const auto add_feedback = [&](const sanguinius::DiscordSnowflake user,
                                const std::string &control,
                                const std::size_t base) {
    return fixture.repository->record_feedback(
        {.actor_user_id = user,
         .guild_id = 10,
         .channel_id = 20,
         .action = sanguinius::AppearanceFeedbackAction::more,
         .control_id = control,
         .reference = std::nullopt,
         .quiet_until_ms = std::nullopt,
         .feedback_id = uuid(base),
         .event_id = uuid(base + 1),
         .idempotency_key = "feedback-metric-" + std::to_string(base),
         .correlation_id = "feedback-metrics",
         .now_ms = 100'040});
  };
  REQUIRE(add_feedback(30, delivery.feedback_control_ids[1], 32'000) ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE(add_feedback(31, delivery.feedback_control_ids[2], 32'010) ==
          sanguinius::AppearanceMutationResult::applied);
  REQUIRE(add_feedback(32, delivery.feedback_control_ids[2], 32'020) ==
          sanguinius::AppearanceMutationResult::applied);
  const auto summary = fixture.repository->control_summary(100'050);
  REQUIRE(summary.feedback_more == 0);
  REQUIRE(summary.feedback_less == 1);
  REQUIRE(summary.feedback_not_relevant == 2);
  REQUIRE(summary.recommendation == "return_to_dry_run");
  REQUIRE(summary.theme_review_keys ==
          std::vector<std::string>{"theme-review-fixture"});
}

TEST_CASE(
    "owner-submitted non-test appearance proposals require explicit approval",
    "[appearance][repository][live][chronicle][approval]") {
  AppearanceFixture fixture;
  auto chronicle_policy = nlohmann::json::parse(fixture.policy.canonical_json);
  chronicle_policy["policy_version"] = "m10-live-chronicle-approval";
  chronicle_policy["scoring"]["threshold"] = 1;
  fixture.policy = sanguinius::parse_appearance_policy(chronicle_policy.dump());
  fixture.repository->register_policy(fixture.policy, 90'000);
  fixture.repository->activate_mode(sanguinius::AppearanceMode::live, 90'001);
  const auto candidates =
      conversation_candidates(fixture, 8, 36'000, 100'000, 36'000);
  REQUIRE(candidates.size() == 1);
  const sanguinius::AppearanceModelResult model{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "A safe non-test Chronicle appearance fixture.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = .95};
  const auto decision_id = uuid(37'100);
  const sanguinius::AppearanceDeliveryIds delivery{
      .reservation_id = uuid(37'101),
      .outbox_id = uuid(37'102),
      .feedback_control_ids = {uuid(37'103), uuid(37'104), uuid(37'105),
                               uuid(37'106)}};
  REQUIRE(fixture.repository->record_final(
      fixture.policy, sanguinius::AppearanceMode::live, candidates.front(),
      sanguinius::evaluate_appearance(fixture.policy,
                                      sanguinius::AppearanceMode::live,
                                      candidates.front(), 100'020),
      decision_id, uuid(37'107), "chronicle-approval", "model_accepted", model,
      delivery, 100'020));
  fixture.context->connection().execute(
      "UPDATE outbox_message SET state='delivered',"
      "provider_message_id='37102',delivered_at_ms=100030,"
      "terminal_at_ms=100030,updated_at_ms=100030 WHERE outbox_id='" +
      delivery.outbox_id + "'");

  sanguinius::persistence::SqliteChronicleRepository chronicle{fixture.context};
  const auto proposal = sanguinius::CreateProposalRequest{
      .entry_id = uuid(37'200),
      .event_id = uuid(37'201),
      .actions = {.edit_token_id = uuid(37'202),
                  .submit_token_id = uuid(37'203),
                  .retract_token_id = uuid(37'204)},
      .source = {.reference = {.message_id = 37'102,
                               .guild_id = 10,
                               .channel_id = 20},
                 .author = {.user_id = 42,
                            .username = "sanguinius",
                            .display_name = "Sanguinius",
                            .is_bot = true},
                 .content = model.text,
                 .occurred_at_ms = 100'030},
      .proposer_user_id = 30,
      .owner_user_id = 30,
      .title = "A delivered appearance",
      .body = model.text,
      .owner_test = false,
      .appearance_decision_id = decision_id,
      .correlation_id = "appearance-chronicle-approval",
      .idempotency_key = "appearance-chronicle-approval:create",
      .now_ms = 100'040,
      .action_expires_at_ms = 200'000,
      .notice_expires_at_ms = 300'000};
  const auto created = chronicle.create_or_get_proposal(proposal);
  REQUIRE(created.code == sanguinius::ChronicleResultCode::created);

  const auto approval_id = uuid(37'210);
  const auto approve_token_id = uuid(37'213);
  const auto submitted = chronicle.submit_proposal(
      {.token_id = proposal.actions.submit_token_id,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .proposer_approval_id = uuid(37'205),
       .submit_event_id = uuid(37'206),
       .immediate_canon_event_id = uuid(37'207),
       .reviewer_dispatches = {{.approval_id = approval_id,
                                .notice_id = uuid(37'211),
                                .notice_open_token_id = uuid(37'212),
                                .approve_token_id = approve_token_id,
                                .decline_token_id = uuid(37'214),
                                .notice_event_id = uuid(37'215),
                                .notice_outbox_id = uuid(37'216)}},
       .correlation_id = "appearance-chronicle-approval",
       .interaction_idempotency_key = "appearance-chronicle-approval:submit",
       .now_ms = 100'050,
       .notice_expires_at_ms = 300'000});
  REQUIRE(submitted.code == sanguinius::ChronicleResultCode::updated);
  REQUIRE_FALSE(submitted.became_canon);
  REQUIRE(submitted.entry->status ==
          sanguinius::ChronicleEntryStatus::proposed);
  REQUIRE(
      scalar(*fixture.context, "SELECT count(*) FROM chronicle_approval WHERE "
                               "approval_id='" +
                                   approval_id +
                                   "' AND approval_role='owner' AND "
                                   "state='pending'") == 1);

  const auto approved = chronicle.apply_approval(
      {.token_id = approve_token_id,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .action_event_id = uuid(37'217),
       .canon_event_id = uuid(37'218),
       .public_outbox_id = uuid(37'219),
       .correlation_id = "appearance-chronicle-approval",
       .interaction_idempotency_key = "appearance-chronicle-approval:approve",
       .now_ms = 100'060});
  REQUIRE(approved.code == sanguinius::ChronicleResultCode::updated);
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == sanguinius::ChronicleEntryStatus::canon);
}
