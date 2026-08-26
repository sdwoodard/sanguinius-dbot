#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_session_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"
#include "sanguinius/tarot_catalog.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "10000000-0000-4000-8000-" + suffix;
}

class HouseFixture {
public:
  HouseFixture() {
    {
      auto database =
          sanguinius::persistence::Database::open_migration(temporary.path());
      const sanguinius::persistence::Migrator migrator{
          sanguinius::persistence::production_migrations(),
          {"test", "revision"},
          clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 13);
    }
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            sanguinius::persistence::Database::open_runtime(temporary.path()));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
    identities.ensure_user({31, "Member", "member", false, 100});
    tarot = std::make_unique<sanguinius::persistence::SqliteTarotRepository>(
        context);
    tarot->initialize_system_accounts({uuid(1), uuid(2), uuid(3), uuid(4)},
                                      100);
    catalogs =
        std::make_unique<sanguinius::persistence::SqliteTarotCatalogRepository>(
            context);
    deck = sanguinius::load_tarot_deck_catalog(
        config_path("emperor-tarot-v1.json"));
    house_catalog = sanguinius::load_tarot_house_catalog(
        config_path("tarot-house-v1.json"), 20);
    catalogs->install(deck, house_catalog, 100);
    draws =
        std::make_unique<sanguinius::persistence::SqliteTarotDrawRepository>(
            context);
    house =
        std::make_unique<sanguinius::persistence::SqliteTarotHouseRepository>(
            context);
  }

  [[nodiscard]] std::filesystem::path
  config_path(const std::string &name) const {
    return std::filesystem::path{__FILE__}
               .parent_path()
               .parent_path()
               .parent_path() /
           "config" / name;
  }

  [[nodiscard]] sanguinius::TarotInvocation
  call(const std::string &key, const std::int64_t now,
       const std::uint64_t user = 30) const {
    return {.user_id = user,
            .guild_id = 10,
            .channel_id = 20,
            .display_name = user == 30 ? "Owner" : "Member",
            .interaction_idempotency_key = key,
            .correlation_id = "house-test",
            .now_ms = now};
  }

  void provision(const std::uint64_t user, const std::int64_t amount,
                 const std::size_t base) {
    static_cast<void>(tarot->ensure_account(
        {.invocation = call("provision:" + std::to_string(user), 100, user),
         .starting_fate = amount,
         .account_id = uuid(base),
         .transaction_id = uuid(base + 1),
         .event_id = uuid(base + 2),
         .mint_posting_id = uuid(base + 3),
         .human_posting_id = uuid(base + 4)}));
  }

  [[nodiscard]] std::function<std::string()> ids(std::size_t start) {
    return [next = start]() mutable { return uuid(next++); };
  }

  void deliver_offer_source(const std::string_view offer_id,
                            const std::int64_t now_ms,
                            const std::size_t id) {
    sanguinius::persistence::SqliteDurableWorkRepository durable{context};
    const auto source = durable.claim_due_outbox(
        now_ms, now_ms + 1'000, "offer-source", uuid(id), true);
    REQUIRE(source.has_value());
    REQUIRE(source->outbox_id ==
            text_scalar("SELECT create_outbox_id FROM tarot_house_public_card "
                        "WHERE offer_id='" +
                        std::string{offer_id} + "'"));
    REQUIRE(durable.mark_public_outbox_submitted(
                *source,
                {.wall_time_ms = now_ms,
                 .elapsed_realtime_ms = clock.elapsed_realtime_ms(),
                 .boot_session_id = std::string{clock.boot_session_id()}},
                now_ms + 1'000) == sanguinius::WorkMutationStatus::applied);
    REQUIRE(durable.complete_public_outbox(
                *source, sanguinius::DiscordId{static_cast<std::uint64_t>(id)},
                now_ms) == sanguinius::WorkMutationStatus::applied);
  }

  [[nodiscard]] sanguinius::TarotDrawRecord
  persist_draw(const std::size_t id, const std::uint64_t user,
               const sanguinius::TarotVisibility visibility,
               const std::int64_t occurred_at_ms, const bool is_test = false) {
    const auto result = draws->draw(
        {.invocation = call("draw:" + std::to_string(id), occurred_at_ms, user),
         .visibility = visibility,
         .cooldown_ms = 86'400'000,
         .bypass_cooldown = true,
         .is_test = is_test,
         .draw_id = uuid(id),
         .event_id = uuid(id + 1),
         .public_outbox_id =
             visibility == sanguinius::TarotVisibility::public_result
                 ? uuid(id + 2)
                 : std::string{},
         .sample = [] { return std::pair<std::int64_t, std::int64_t>{1, 0}; }});
    REQUIRE(result.status == sanguinius::TarotDrawStatus::drawn);
    REQUIRE(result.draw.has_value());
    return *result.draw;
  }

  void seed_player_event(const std::size_t id, const std::uint64_t user,
                         const std::string_view result,
                         const std::string_view wager_kind,
                         const std::int64_t occurred_at_ms) {
    const auto event_id = uuid(id);
    auto event = context->connection().prepare(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,source_message_id,"
        "occurred_at_ms,recorded_at_ms,correlation_id,causation_id,"
        "idempotency_key,payload_json) VALUES(?,'test.player.v1',"
        "'test_player',?,?, '10','20',NULL,?,?,'house-test',NULL,?,'{}')");
    event.bind(1, event_id);
    event.bind(2, event_id);
    event.bind(3, sanguinius::DiscordSnowflake{user}.str());
    event.bind(4, occurred_at_ms);
    event.bind(5, occurred_at_ms);
    event.bind(6, "test:player:" + event_id);
    event.execute();
    auto projection = context->connection().prepare(
        "INSERT INTO tarot_player_event(source_event_id,user_id,result,"
        "wager_kind,is_test,baseline,occurred_at_ms) VALUES(?,?,?,?,0,0,?)");
    projection.bind(1, event_id);
    projection.bind(2, sanguinius::DiscordSnowflake{user}.str());
    projection.bind(3, result);
    projection.bind(4, wager_kind);
    projection.bind(5, occurred_at_ms);
    projection.execute();
  }

  [[nodiscard]] std::int64_t scalar(const std::string_view sql) const {
    auto query = context->connection().prepare(sql);
    REQUIRE(query.step());
    return query.column_int64(0);
  }

  [[nodiscard]] std::string text_scalar(const std::string_view sql) const {
    auto query = context->connection().prepare(sql);
    REQUIRE(query.step());
    return query.column_text(0);
  }

  [[nodiscard]] sanguinius::ClaimedScheduledJob
  weekly_job(const std::int64_t due_at_ms) const {
    return {.job_id = uuid(900),
            .job_type =
                std::string{sanguinius::tarot_house_weekly_offer_job_type},
            .lease_owner = "test",
            .lease_token = uuid(901),
            .attempt_count = 1,
            .max_attempts = 1,
            .due_at_ms = due_at_ms,
            .payload =
                sanguinius::TarotHouseWeeklyOfferJobPayload{
                    .schedule_key = "friday-1800-america-new-york",
                    .catalog_version = house_catalog.version},
            .correlation_id = "weekly-test",
            .causation_event_id = std::nullopt};
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
  std::unique_ptr<sanguinius::persistence::SqliteTarotRepository> tarot;
  std::unique_ptr<sanguinius::persistence::SqliteTarotCatalogRepository>
      catalogs;
  std::unique_ptr<sanguinius::persistence::SqliteTarotDrawRepository> draws;
  std::unique_ptr<sanguinius::persistence::SqliteTarotHouseRepository> house;
  sanguinius::TarotDeckCatalog deck;
  sanguinius::TarotHouseCatalog house_catalog;
};

} // namespace

TEST_CASE("persisted draw replay and cooldown never resample",
          "[tarot][draw][sqlite][restart]") {
  HouseFixture fixture;
  std::size_t samples{};
  const auto request = sanguinius::TarotDrawRequest{
      .invocation = fixture.call("draw:one", 1'000),
      .visibility = sanguinius::TarotVisibility::public_result,
      .cooldown_ms = 86'400'000,
      .bypass_cooldown = false,
      .is_test = false,
      .draw_id = uuid(100),
      .event_id = uuid(101),
      .public_outbox_id = uuid(10'100),
      .sample = [&] {
        ++samples;
        return std::pair<std::int64_t, std::int64_t>{21, 1};
      }};
  const auto first = fixture.draws->draw(request);
  REQUIRE(first.status == sanguinius::TarotDrawStatus::drawn);
  REQUIRE(first.draw->card_ordinal == 21);
  REQUIRE(first.draw->card_name == fixture.deck.cards.at(21).name);
  REQUIRE(first.draw->card_meaning == fixture.deck.cards.at(21).meaning);
  REQUIRE(first.draw->flavor_text ==
          fixture.deck.cards.at(21).flavor_variants.at(1));
  REQUIRE(first.public_delivery_created);
  REQUIRE(
      fixture.scalar(
          "SELECT count(*) FROM tarot_draw_public_delivery WHERE draw_id='" +
          first.draw->draw_id + "'") == 1);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM outbox_message WHERE aggregate_type="
                     "'tarot_draw' AND aggregate_id='" +
                     first.draw->draw_id + "'") == 1);
  const auto public_payload = fixture.text_scalar(
      "SELECT payload_json FROM outbox_message WHERE aggregate_type="
      "'tarot_draw' AND aggregate_id='" +
      first.draw->draw_id + "'");
  REQUIRE(public_payload.find(first.draw->card_name) != std::string::npos);
  REQUIRE(public_payload.find(first.draw->flavor_text) != std::string::npos);
  REQUIRE(samples == 1);
  const auto replay = fixture.draws->draw(request);
  REQUIRE(replay.status == sanguinius::TarotDrawStatus::replay);
  REQUIRE(replay.public_delivery_created);
  REQUIRE(replay.draw->card_name == first.draw->card_name);
  REQUIRE(replay.draw->flavor_text == first.draw->flavor_text);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM outbox_message WHERE aggregate_type="
                     "'tarot_draw' AND aggregate_id='" +
                     first.draw->draw_id + "'") == 1);
  REQUIRE(samples == 1);

  fixture.draws.reset();
  fixture.draws =
      std::make_unique<sanguinius::persistence::SqliteTarotDrawRepository>(
          fixture.context);
  const auto restored = fixture.draws->find(first.draw->draw_id, 30);
  REQUIRE(restored);
  REQUIRE(restored->catalog_version == first.draw->catalog_version);
  REQUIRE(restored->card_name == first.draw->card_name);
  REQUIRE(restored->card_meaning == first.draw->card_meaning);
  REQUIRE(restored->flavor_text == first.draw->flavor_text);
  REQUIRE_FALSE(fixture.draws->find(first.draw->draw_id, 31));

  const auto private_draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:private-member", 1'000, 31),
       .visibility = sanguinius::TarotVisibility::private_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = false,
       .is_test = false,
       .draw_id = uuid(104),
       .event_id = uuid(105),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{3, 0}; }});
  REQUIRE(private_draw.status == sanguinius::TarotDrawStatus::drawn);
  REQUIRE(fixture.draws->find(private_draw.draw->draw_id, 31));
  REQUIRE_FALSE(fixture.draws->find(private_draw.draw->draw_id, 30));

  auto cooldown = request;
  cooldown.invocation = fixture.call("draw:two", 2'000);
  cooldown.draw_id = uuid(102);
  cooldown.event_id = uuid(103);
  REQUIRE(fixture.draws->draw(cooldown).status ==
          sanguinius::TarotDrawStatus::cooldown);
  REQUIRE(samples == 1);
}

TEST_CASE("competing normal draws persist one result and sample once",
          "[tarot][draw][sqlite][concurrency][cooldown]") {
  HouseFixture fixture;
  auto second_context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          sanguinius::persistence::Database::open_runtime(
              fixture.temporary.path()));
  sanguinius::persistence::SqliteTarotDrawRepository second{second_context};
  std::atomic<std::size_t> samples{};
  std::atomic<bool> start{};
  std::optional<sanguinius::TarotDrawResult> first_result;
  std::optional<sanguinius::TarotDrawResult> second_result;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  const auto request = [&](const std::string &key, const std::size_t id) {
    return sanguinius::TarotDrawRequest{
        .invocation = fixture.call(key, 1'000),
        .visibility = sanguinius::TarotVisibility::public_result,
        .cooldown_ms = 86'400'000,
        .bypass_cooldown = false,
        .is_test = false,
        .draw_id = uuid(id),
        .event_id = uuid(id + 1),
        .public_outbox_id = uuid(id + 10'000),
        .sample = [&samples, id] {
          ++samples;
          return std::pair<std::int64_t, std::int64_t>{
              static_cast<std::int64_t>(id % 22), 0};
        }};
  };

  std::thread first_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    try {
      first_result = fixture.draws->draw(request("draw:race:first", 110));
    } catch (...) {
      first_error = std::current_exception();
    }
  }};
  std::thread second_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    try {
      second_result = second.draw(request("draw:race:second", 120));
    } catch (...) {
      second_error = std::current_exception();
    }
  }};
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  REQUIRE(first_error == nullptr);
  REQUIRE(second_error == nullptr);
  REQUIRE(first_result.has_value());
  REQUIRE(second_result.has_value());
  const auto drawn =
      (first_result->status == sanguinius::TarotDrawStatus::drawn ? 1 : 0) +
      (second_result->status == sanguinius::TarotDrawStatus::drawn ? 1 : 0);
  const auto cooldown =
      (first_result->status == sanguinius::TarotDrawStatus::cooldown ? 1 : 0) +
      (second_result->status == sanguinius::TarotDrawStatus::cooldown ? 1 : 0);
  REQUIRE(drawn == 1);
  REQUIRE(cooldown == 1);
  REQUIRE(samples.load() == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_card_draw") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_draw_receipt") == 2);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_observation") == 1);
}

TEST_CASE("draw-authority House play rejects an impossible cooldown window",
          "[tarot][house][draw][cooldown][eligibility]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 250);
  const auto &herald =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  REQUIRE(fixture.house
              ->availability(fixture.call("offers:before-draw", 999), herald,
                             false, 100)
              .status == sanguinius::HouseAvailabilityStatus::available);
  const auto &returning =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  REQUIRE(fixture.house
              ->availability(fixture.call("offers:recovery", 999), returning,
                             false, 100)
              .status == sanguinius::HouseAvailabilityStatus::ineligible);
  const auto &last_standard =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  REQUIRE(fixture.house
              ->availability(fixture.call("offers:weekly", 999), last_standard,
                             false, 100)
              .status ==
          sanguinius::HouseAvailabilityStatus::no_scheduled_offer);
  const auto draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:before-house", 1'000),
       .visibility = sanguinius::TarotVisibility::private_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = false,
       .is_test = false,
       .draw_id = uuid(260),
       .event_id = uuid(261),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{2, 0}; }});
  REQUIRE(draw.status == sanguinius::TarotDrawStatus::drawn);
  const auto unavailable = fixture.house->availability(
      fixture.call("offers:after-draw", 2'000), herald, false, 100);
  REQUIRE(unavailable.status == sanguinius::HouseAvailabilityStatus::cooldown);
  REQUIRE(unavailable.cooldown_until_ms == draw.draw->cooldown_until_ms);
  const auto rejected = fixture.house->play(
      {.invocation = fixture.call("house:impossible-draw", 2'000),
       .definition = &herald,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(270)});
  REQUIRE(rejected.status == sanguinius::HouseMutationStatus::cooldown);
  REQUIRE_FALSE(rejected.wager);
  REQUIRE(fixture.tarot->balance(30) == 100);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_wager") == 0);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("catalog versions reject canonical checksum collisions",
          "[tarot][catalog][sqlite][version]") {
  HouseFixture fixture;
  auto changed_deck = fixture.deck;
  changed_deck.canonical_json += " ";
  changed_deck.checksum =
      sanguinius::stable_catalog_checksum(changed_deck.canonical_json);
  REQUIRE_THROWS_AS(
      fixture.catalogs->install(changed_deck, fixture.house_catalog, 200),
      std::runtime_error);
  auto changed_house = fixture.house_catalog;
  changed_house.canonical_json += " ";
  changed_house.checksum =
      sanguinius::stable_catalog_checksum(changed_house.canonical_json);
  REQUIRE_THROWS_AS(fixture.catalogs->install(fixture.deck, changed_house, 200),
                    std::runtime_error);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_catalog_snapshot") == 2);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_card_definition") == 22);
}

TEST_CASE(
    "Herald's Call survives clock rollback and pays one draw exactly once",
    "[tarot][house][sqlite][payout][restart][rollback]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 10);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  auto played = fixture.house->play(
      {.invocation = fixture.call("house:herald", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(100)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(30) == 95);
  REQUIRE(fixture.house->economy().expected_house_escrow == 10);
  sanguinius::persistence::SqliteWagerRepository wagers{fixture.context};
  const auto funded_audit = wagers.check_invariants();
  REQUIRE(funded_audit.valid);
  REQUIRE(funded_audit.open_funded_obligation_count == 1);
  REQUIRE(funded_audit.open_funded_obligation_amount == 10);
  REQUIRE(funded_audit.escrow_balance == 10);

  const auto draw = fixture.persist_draw(
      200, 30, sanguinius::TarotVisibility::private_result, 500);
  const auto settled = fixture.house->observe_draw(draw, 500, fixture.ids(300));
  REQUIRE(settled.size() == 1);
  REQUIRE(settled.front().wager->result == sanguinius::HouseResult::win);
  REQUIRE(fixture.scalar("SELECT terminal_at_ms FROM tarot_house_wager WHERE "
                         "wager_id='" +
                         played.wager->wager_id + "'") == 1'000);
  REQUIRE(fixture.tarot->balance(30) == 105);
  REQUIRE(fixture.house->economy().valid);
  REQUIRE(wagers.check_invariants().valid);
  REQUIRE(fixture.house->observe_draw(draw, 500, fixture.ids(400)).empty());
  REQUIRE(fixture.tarot->balance(30) == 105);

  const auto cooldown_end = 1'000 + definition.terminal_cooldown_ms;
  const auto replay_before_cooldown = fixture.house->play(
      {.invocation = fixture.call("house:herald:cooldown", cooldown_end - 1),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 1,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(450)});
  REQUIRE(replay_before_cooldown.status ==
          sanguinius::HouseMutationStatus::cooldown);
  const auto boundary = fixture.house->play(
      {.invocation = fixture.call("house:herald:boundary", cooldown_end),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 1,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(460)});
  REQUIRE(boundary.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(30) == 104);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("House draw qualification follows persisted event order",
          "[tarot][house][draw][ordering][rollback]") {
  SECTION("a later draw at the acceptance timestamp qualifies") {
    HouseFixture fixture;
    fixture.provision(30, 100, 120);
    const auto &definition =
        sanguinius::house_template(fixture.house_catalog, "heralds-call");
    const auto played = fixture.house->play(
        {.invocation = fixture.call("house:order:equal", 1'000),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "answer",
         .stake = 1,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(200)});
    REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
    const auto draw = fixture.persist_draw(
        300, 30, sanguinius::TarotVisibility::private_result, 1'000);
    const auto settled =
        fixture.house->observe_draw(draw, 1'000, fixture.ids(320));
    REQUIRE(settled.size() == 1);
    REQUIRE(settled.front().wager->result == sanguinius::HouseResult::win);
  }

  SECTION("a previously persisted future-dated draw does not qualify") {
    HouseFixture fixture;
    fixture.provision(30, 100, 400);
    const auto draw = fixture.persist_draw(
        500, 31, sanguinius::TarotVisibility::public_result, 2'000);
    const auto &definition =
        sanguinius::house_template(fixture.house_catalog, "final-hour");
    const auto played = fixture.house->play(
        {.invocation = fixture.call("house:order:future", 1'000),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "yes",
         .stake = 1,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(600)});
    REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
    REQUIRE(fixture.house->observe_draw(draw, 2'000, fixture.ids(620)).empty());
    REQUIRE(fixture.house->reconcile_draws(3'000, fixture.ids(640)).empty());
    const auto settled = fixture.house->resolve_due(
        played.wager->outcome_due_at_ms, false, fixture.ids(660));
    REQUIRE(settled.size() == 1);
    REQUIRE(settled.front().wager->result == sanguinius::HouseResult::loss);
  }
}

TEST_CASE("House play receipts reject interaction-key request collisions",
          "[tarot][house][sqlite][idempotency]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 15);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  auto request = sanguinius::HousePlayRequest{
      .invocation = fixture.call("house:play:fingerprint", 1'000),
      .definition = &definition,
      .catalog_version = fixture.house_catalog.version,
      .choice_slug = "answer",
      .stake = 1,
      .visibility = sanguinius::TarotVisibility::private_result,
      .exposure_cap = 100,
      .profit_cap = 20,
      .starting_fate = 100,
      .is_test = false,
      .offer_id = std::nullopt,
      .next_id = fixture.ids(475)};
  REQUIRE(fixture.house->play(request).status ==
          sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.house->play(request).status ==
          sanguinius::HouseMutationStatus::replay);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM tarot_house_receipt WHERE operation="
                     "'play' AND request_fingerprint IS NOT NULL") == 1);

  auto collision = request;
  collision.stake = 5;
  REQUIRE_THROWS_AS(fixture.house->play(collision), std::invalid_argument);
  collision = request;
  collision.visibility = sanguinius::TarotVisibility::public_result;
  REQUIRE_THROWS_AS(fixture.house->play(collision), std::invalid_argument);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_wager") == 1);

  const auto &recovery =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  auto rejected = sanguinius::HousePlayRequest{
      .invocation = fixture.call("house:play:rejected-fingerprint", 2'000),
      .definition = &recovery,
      .catalog_version = fixture.house_catalog.version,
      .choice_slug = "rise",
      .stake = 0,
      .visibility = sanguinius::TarotVisibility::private_result,
      .exposure_cap = 100,
      .profit_cap = 20,
      .starting_fate = 100,
      .is_test = false,
      .offer_id = std::nullopt,
      .next_id = fixture.ids(490)};
  REQUIRE(fixture.house->play(rejected).status ==
          sanguinius::HouseMutationStatus::ineligible);
  rejected.visibility = sanguinius::TarotVisibility::public_result;
  REQUIRE_THROWS_AS(fixture.house->play(rejected), std::invalid_argument);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_receipt") == 2);
}

TEST_CASE("Returning Dawn recovery is MINT collateralized and restart safe",
          "[tarot][house][recovery][sqlite]") {
  HouseFixture fixture;
  fixture.provision(31, 40, 20);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:recovery", 5'000, 31),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(500)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(31) == 40);
  REQUIRE(fixture.house->economy().expected_house_escrow == 5);
  const auto stacked = fixture.house->play(
      {.invocation = fixture.call("house:recovery:stacked", 5'001, 31),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(550)});
  REQUIRE(stacked.status == sanguinius::HouseMutationStatus::cooldown);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_wager WHERE user_id='31' "
              "AND template_slug='returning-dawn' AND state="
              "'accepted_funded'") == 1);
  REQUIRE(fixture.house->economy().expected_house_escrow == 5);

  const auto draw = fixture.persist_draw(
      600, 31, sanguinius::TarotVisibility::private_result, 6'000);
  REQUIRE(fixture.house->observe_draw(draw, 6'000, fixture.ids(700)).size() ==
          1);
  REQUIRE(fixture.tarot->balance(31) == 45);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("Returning Dawn expiry returns MINT collateral and starts cooldown",
          "[tarot][house][recovery][expiry][cooldown]") {
  HouseFixture fixture;
  fixture.provision(31, 40, 25);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:recovery:expiry", 5'000, 31),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(750)});
  const auto due = played.wager->outcome_due_at_ms;
  const auto expired = fixture.house->resolve_due(due, false, fixture.ids(800));
  REQUIRE(expired.size() == 1);
  REQUIRE(expired.front().wager->result == sanguinius::HouseResult::void_wager);
  REQUIRE(fixture.text_scalar(
              "SELECT authority FROM tarot_house_resolution WHERE wager_id='" +
              played.wager->wager_id + "'") == "deadline");
  REQUIRE(fixture.text_scalar(
              "SELECT action_kind FROM tarot_house_action WHERE wager_id='" +
              played.wager->wager_id +
              "' AND expected_revision=1 ORDER BY "
              "created_at_ms DESC LIMIT 1") == "deadline");
  REQUIRE(fixture.tarot->balance(31) == 40);
  REQUIRE(
      fixture.scalar(
          "SELECT COALESCE(sum(post.amount),0) FROM tarot_posting post JOIN "
          "tarot_transaction tx ON tx.transaction_id=post.transaction_id "
          "JOIN tarot_account account ON account.account_id=post.account_id "
          "WHERE account.account_kind='MINT' AND "
          "tx.transaction_type IN ('WAGER_ESCROW_FUND','WAGER_REFUND')") == 0);
  const auto cooldown = fixture.house->play(
      {.invocation =
           fixture.call("house:recovery:blocked",
                        due + definition.terminal_cooldown_ms - 1, 31),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(850)});
  REQUIRE(cooldown.status == sanguinius::HouseMutationStatus::cooldown);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("Final Hour observes only another human public draw",
          "[tarot][house][final-hour][event][deadline][payout]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 65);
  fixture.provision(31, 100, 75);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "final-hour");
  const auto yes = fixture.house->play(
      {.invocation = fixture.call("house:final:yes", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 10,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(900)});
  REQUIRE(yes.wager->profit == 20);
  const auto draw = [&](const std::size_t id, const std::uint64_t user,
                        const sanguinius::TarotVisibility draw_visibility,
                        const std::int64_t at) {
    return fixture.persist_draw(id, user, draw_visibility, at);
  };
  REQUIRE(
      fixture.house
          ->observe_draw(
              draw(950, 30, sanguinius::TarotVisibility::public_result, 2'000),
              2'000, fixture.ids(960))
          .empty());
  REQUIRE(
      fixture.house
          ->observe_draw(
              draw(970, 31, sanguinius::TarotVisibility::private_result, 2'500),
              2'500, fixture.ids(980))
          .empty());
  fixture.context->connection().execute(
      "UPDATE discord_user SET is_bot=1 WHERE user_id='31'");
  const auto bot_draw =
      draw(990, 31, sanguinius::TarotVisibility::public_result, 3'000);
  REQUIRE(fixture.house
              ->observe_draw(bot_draw, 3'000, fixture.ids(1'000))
              .empty());
  REQUIRE(fixture.house->reconcile_draws(3'001, fixture.ids(1'020)).empty());
  fixture.context->connection().execute(
      "UPDATE discord_user SET is_bot=0 WHERE user_id='31'");
  const auto observed = fixture.house->observe_draw(
      draw(1'040, 31, sanguinius::TarotVisibility::public_result, 3'500),
      3'500, fixture.ids(1'060));
  REQUIRE(observed.size() == 1);
  REQUIRE(observed.front().wager->result == sanguinius::HouseResult::win);
  REQUIRE(fixture.tarot->balance(30) == 120);

  const auto no = fixture.house->play(
      {.invocation = fixture.call("house:final:no", 4'000, 31),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "no",
       .stake = 10,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(1'100)});
  fixture.context->connection().execute(
      "UPDATE discord_user SET is_bot=1 WHERE user_id='30'");
  REQUIRE(fixture.house
              ->observe_draw(
                  draw(1'150, 30, sanguinius::TarotVisibility::public_result,
                       5'000),
                  5'000, fixture.ids(1'170))
              .empty());
  const auto deadline = fixture.house->resolve_due(no.wager->outcome_due_at_ms,
                                                   false, fixture.ids(1'200));
  fixture.context->connection().execute(
      "UPDATE discord_user SET is_bot=0 WHERE user_id='30'");
  REQUIRE(deadline.size() == 1);
  REQUIRE(deadline.front().wager->result == sanguinius::HouseResult::win);
  REQUIRE(fixture.text_scalar(
              "SELECT authority FROM tarot_house_resolution WHERE wager_id='" +
              no.wager->wager_id + "'") == "deadline");
  REQUIRE(fixture.tarot->balance(31) == 110);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("public House settlement waits for funded and draw deliveries",
          "[tarot][house][outbox][ordering][idempotency]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 1'300);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:ordered-public", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(1'400)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  const auto draw = fixture.persist_draw(
      1'500, 30, sanguinius::TarotVisibility::public_result, 1'000);
  const auto settled =
      fixture.house->observe_draw(draw, 1'000, fixture.ids(1'600));
  REQUIRE(settled.size() == 1);
  const auto terminal_id = fixture.text_scalar(
      "SELECT outbox_id FROM outbox_message WHERE idempotency_key='outbox:"
      "tarot-house:" +
      played.wager->wager_id + ":terminal'");
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_public_outbox_dependency WHERE "
              "successor_outbox_id='" +
              terminal_id + "'") == 2);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_public_outbox_dependency WHERE "
              "successor_outbox_id='" +
              terminal_id + "' AND dependency_kind='funded_before_terminal'") ==
          1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_public_outbox_dependency WHERE "
              "successor_outbox_id='" +
              terminal_id + "' AND dependency_kind='draw_before_terminal'") ==
          1);

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  for (std::size_t index = 0; index < 2; ++index) {
    const auto predecessor = durable.claim_due_outbox(
        1'000, 2'000, "ordering", uuid(1'700 + index), true);
    REQUIRE(predecessor.has_value());
    REQUIRE(predecessor->outbox_id != terminal_id);
    REQUIRE(durable.mark_public_outbox_submitted(
                *predecessor,
                {.wall_time_ms = 1'000,
                 .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
                 .boot_session_id = std::string{fixture.clock.boot_session_id()}},
                2'000) == sanguinius::WorkMutationStatus::applied);
    REQUIRE(durable.complete_public_outbox(
                *predecessor, sanguinius::DiscordId{6'000 + index}, 1'000) ==
            sanguinius::WorkMutationStatus::applied);
  }
  const auto terminal = durable.claim_due_outbox(
      1'000, 2'000, "ordering", uuid(1'702), true);
  REQUIRE(terminal.has_value());
  REQUIRE(terminal->outbox_id == terminal_id);
}

TEST_CASE("House funding rejects insufficient balances and serializes exposure",
          "[tarot][house][funding][exposure][concurrency]") {
  {
    HouseFixture fixture;
    fixture.provision(30, 1, 1'300);
    const auto &herald =
        sanguinius::house_template(fixture.house_catalog, "heralds-call");
    const auto rejected = fixture.house->play(
        {.invocation = fixture.call("house:insufficient", 1'000),
         .definition = &herald,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "answer",
         .stake = 5,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(1'400)});
    REQUIRE(rejected.status ==
            sanguinius::HouseMutationStatus::insufficient_funds);
    REQUIRE(fixture.tarot->balance(30) == 1);
    REQUIRE(fixture.house->economy().valid);
  }

  HouseFixture fixture;
  fixture.provision(30, 100, 1'500);
  fixture.provision(31, 100, 1'600);
  const auto &final_hour =
      sanguinius::house_template(fixture.house_catalog, "final-hour");
  auto second_context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          sanguinius::persistence::Database::open_runtime(
              fixture.temporary.path()));
  sanguinius::persistence::SqliteTarotHouseRepository competitor{
      second_context};
  std::atomic_bool start{};
  sanguinius::HouseMutationResult first;
  sanguinius::HouseMutationResult second;
  const auto play =
      [&](sanguinius::persistence::SqliteTarotHouseRepository &repo,
          const std::uint64_t user, const std::size_t ids) {
        return repo.play(
            {.invocation = fixture.call(
                 "house:exposure:" + std::to_string(user), 2'000, user),
             .definition = &final_hour,
             .catalog_version = fixture.house_catalog.version,
             .choice_slug = "yes",
             .stake = 10,
             .visibility = sanguinius::TarotVisibility::private_result,
             .exposure_cap = 20,
             .profit_cap = 20,
             .starting_fate = 100,
             .is_test = false,
             .offer_id = std::nullopt,
             .next_id = fixture.ids(ids)});
      };
  std::thread first_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    first = play(*fixture.house, 30, 1'700);
  }};
  std::thread second_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    second = play(competitor, 31, 1'800);
  }};
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();
  REQUIRE(static_cast<int>(first.status ==
                           sanguinius::HouseMutationStatus::applied) +
              static_cast<int>(second.status ==
                               sanguinius::HouseMutationStatus::applied) ==
          1);
  REQUIRE((first.status == sanguinius::HouseMutationStatus::exposure_blocked ||
           second.status == sanguinius::HouseMutationStatus::exposure_blocked));
  REQUIRE(fixture.house->economy().non_test_exposure == 20);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("MINT-backed recovery does not consume House profit exposure",
          "[tarot][house][recovery][exposure][economy]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 1'900);
  fixture.provision(31, 40, 2'000);
  const auto &final_hour =
      sanguinius::house_template(fixture.house_catalog, "final-hour");
  const auto saturated = fixture.house->play(
      {.invocation = fixture.call("house:exposure:saturated", 1'000),
       .definition = &final_hour,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 10,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 20,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(2'100)});
  REQUIRE(saturated.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.house->economy().non_test_exposure == 20);

  const auto &recovery =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  const auto recovered = fixture.house->play(
      {.invocation = fixture.call("house:recovery:at-cap", 2'000, 31),
       .definition = &recovery,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 20,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(2'200)});
  REQUIRE(recovered.status == sanguinius::HouseMutationStatus::applied);

  const auto economy = fixture.house->economy();
  REQUIRE(economy.non_test_exposure == 20);
  REQUIRE(economy.expected_house_escrow == 35);
  REQUIRE(economy.valid);
}

TEST_CASE("durable House deadline settles once after repository restart",
          "[tarot][house][deadline][sqlite][restart]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 30);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:deadline", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(1'000)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  const auto due_at_ms = played.wager->outcome_due_at_ms;

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto claimed = durable.claim_due_job(due_at_ms, due_at_ms + 10'000,
                                             "restart", uuid(1'100));
  REQUIRE(claimed);
  REQUIRE(claimed->job_type == sanguinius::tarot_house_deadline_job_type);
  fixture.house.reset();
  fixture.house =
      std::make_unique<sanguinius::persistence::SqliteTarotHouseRepository>(
          fixture.context);
  const auto settled = fixture.house->handle_deadline(
      {.job = *claimed, .now_ms = due_at_ms, .next_id = fixture.ids(1'200)});
  REQUIRE(settled.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(settled.wager->result == sanguinius::HouseResult::loss);
  REQUIRE(fixture.text_scalar(
              "SELECT authority FROM tarot_house_resolution WHERE wager_id='" +
              played.wager->wager_id + "'") == "deadline");
  REQUIRE(fixture.text_scalar(
              "SELECT action_kind FROM tarot_house_action WHERE wager_id='" +
              played.wager->wager_id + "' AND action_kind='deadline'") ==
          "deadline");
  REQUIRE(fixture.tarot->balance(30) == 95);
  REQUIRE_FALSE(durable.claim_due_job(due_at_ms + 1, due_at_ms + 10'001,
                                      "restart", uuid(1'300)));
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("persisted draws reconcile after a missed House observer",
          "[tarot][house][draw][recovery][deadline][restart]") {
  SECTION("startup reconciliation settles a completed challenge") {
    HouseFixture fixture;
    fixture.provision(30, 100, 1'350);
    const auto &definition =
        sanguinius::house_template(fixture.house_catalog, "heralds-call");
    const auto played = fixture.house->play(
        {.invocation = fixture.call("house:reconcile:herald", 1'000),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "answer",
         .stake = 5,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(1'400)});
    REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
    const auto draw = fixture.draws->draw(
        {.invocation = fixture.call("draw:reconcile:herald", 2'000),
         .visibility = sanguinius::TarotVisibility::private_result,
         .cooldown_ms = 86'400'000,
         .bypass_cooldown = false,
         .is_test = false,
         .draw_id = uuid(1'500),
         .event_id = uuid(1'501),
         .sample = [] { return std::pair<std::int64_t, std::int64_t>{3, 0}; }});
    REQUIRE(draw.status == sanguinius::TarotDrawStatus::drawn);

    fixture.house.reset();
    fixture.house =
        std::make_unique<sanguinius::persistence::SqliteTarotHouseRepository>(
            fixture.context);
    const auto reconciled =
        fixture.house->reconcile_draws(3'000, fixture.ids(1'600));
    REQUIRE(reconciled.size() == 1);
    REQUIRE(reconciled.front().wager->result == sanguinius::HouseResult::win);
    REQUIRE(fixture.tarot->balance(30) == 105);
    REQUIRE(fixture.house->reconcile_draws(3'001, fixture.ids(1'700)).empty());
    REQUIRE(fixture.tarot->balance(30) == 105);
    REQUIRE(fixture.house->economy().valid);
  }

  SECTION("deadline reconciliation honors a persisted recovery draw") {
    HouseFixture fixture;
    fixture.provision(31, 40, 1'800);
    const auto &definition =
        sanguinius::house_template(fixture.house_catalog, "returning-dawn");
    const auto played = fixture.house->play(
        {.invocation = fixture.call("house:reconcile:recovery", 1'000, 31),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "rise",
         .stake = 0,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(1'900)});
    REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
    const auto draw = fixture.draws->draw(
        {.invocation = fixture.call("draw:reconcile:recovery", 2'000, 31),
         .visibility = sanguinius::TarotVisibility::private_result,
         .cooldown_ms = 86'400'000,
         .bypass_cooldown = false,
         .is_test = false,
         .draw_id = uuid(2'000),
         .event_id = uuid(2'001),
         .sample = [] { return std::pair<std::int64_t, std::int64_t>{4, 0}; }});
    REQUIRE(draw.status == sanguinius::TarotDrawStatus::drawn);

    sanguinius::persistence::SqliteDurableWorkRepository durable{
        fixture.context};
    const auto due = played.wager->outcome_due_at_ms;
    const auto claimed =
        durable.claim_due_job(due, due + 10'000, "restart", uuid(2'100));
    REQUIRE(claimed.has_value());
    fixture.house.reset();
    fixture.house =
        std::make_unique<sanguinius::persistence::SqliteTarotHouseRepository>(
            fixture.context);
    const auto settled = fixture.house->handle_deadline(
        {.job = *claimed, .now_ms = due, .next_id = fixture.ids(2'200)});
    REQUIRE(settled.status == sanguinius::HouseMutationStatus::applied);
    REQUIRE(settled.wager->result == sanguinius::HouseResult::win);
    REQUIRE(
        fixture.text_scalar(
            "SELECT authority FROM tarot_house_resolution WHERE wager_id='" +
            played.wager->wager_id + "'") == "draw");
    REQUIRE(fixture.text_scalar(
                "SELECT action_kind FROM tarot_house_action WHERE wager_id='" +
                played.wager->wager_id +
                "' AND action_kind='automatic_observation'") ==
            "automatic_observation");
    REQUIRE(fixture.tarot->balance(31) == 45);
    REQUIRE(fixture.house->economy().valid);
  }
}

TEST_CASE("weekly Last Standard reserves exposure and only one claim wins",
          "[tarot][house][weekly][sqlite][concurrency][privacy]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 40);
  fixture.provision(31, 100, 50);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(10'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = 10'000,
       .exposure_cap = 100,
       .operational = true,
       .is_test = true,
       .next_id = fixture.ids(2'000)});
  REQUIRE(offer.status == sanguinius::HouseWeeklyOfferStatus::created);
  REQUIRE(offer.offer_id);
  REQUIRE(fixture.house->economy().test_exposure == 10);
  REQUIRE(fixture.scalar(
              "SELECT COALESCE(sum(reserved_profit),0) FROM tarot_house_offer "
              "WHERE is_test=1 AND state='open'") == 10);
  std::string safe_payload;
  {
    auto public_payload = fixture.context->connection().prepare(
        "SELECT payload_json FROM outbox_message WHERE aggregate_type="
        "'tarot_house_offer' ORDER BY available_at_ms LIMIT 1");
    REQUIRE(public_payload.step());
    safe_payload = public_payload.column_text(0);
  }
  REQUIRE(safe_payload.find("[TEST]") != std::string::npos);
  REQUIRE(safe_payload.find("stake") == std::string::npos);
  REQUIRE(safe_payload.find("profit") == std::string::npos);
  REQUIRE(safe_payload.find("balance") == std::string::npos);
  std::string control_id;
  {
    auto control_query = fixture.context->connection().prepare(
        "SELECT token_id FROM tarot_house_control WHERE offer_id=?");
    control_query.bind(1, *offer.offer_id);
    REQUIRE(control_query.step());
    control_id = control_query.column_text(0);
  }
  REQUIRE(
      safe_payload.find(std::string{sanguinius::tarot_house_component_prefix} +
                        control_id) != std::string::npos);
  REQUIRE(fixture.house
              ->inspect_control({.invocation = fixture.call("control", 10'500),
                                 .token_id = control_id})
              .status == sanguinius::HouseControlStatus::available);

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto source = durable.claim_due_outbox(10'500, 20'500, "claim-source",
                                               uuid(2'500), true);
  REQUIRE(source.has_value());
  REQUIRE(source->kind == sanguinius::public_discord_outbox_kind);
  REQUIRE(durable.mark_public_outbox_submitted(
              *source,
              {.wall_time_ms = 10'500,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              20'500) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*source, sanguinius::DiscordId{5'500},
                                         10'500) ==
          sanguinius::WorkMutationStatus::applied);

  auto second_context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          sanguinius::persistence::Database::open_runtime(
              fixture.temporary.path()));
  sanguinius::persistence::SqliteTarotHouseRepository competitor{
      second_context};
  std::atomic_bool start{};
  sanguinius::HouseMutationResult first;
  sanguinius::HouseMutationResult second;
  std::thread first_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    first = fixture.house->play(
        {.invocation = fixture.call("house:claim:owner", 11'000, 30),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "yes",
         .stake = 10,
         .visibility = sanguinius::TarotVisibility::public_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = true,
         .offer_id = offer.offer_id,
         .next_id = fixture.ids(3'000)});
  }};
  std::thread second_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    second = competitor.play(
        {.invocation = fixture.call("house:claim:member", 11'000, 31),
         .definition = &definition,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "no",
         .stake = 10,
         .visibility = sanguinius::TarotVisibility::public_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = true,
         .offer_id = offer.offer_id,
         .next_id = fixture.ids(4'000)});
  }};
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();
  const auto applied =
      static_cast<int>(first.status ==
                       sanguinius::HouseMutationStatus::applied) +
      static_cast<int>(second.status ==
                       sanguinius::HouseMutationStatus::applied);
  REQUIRE(applied == 1);
  REQUIRE((first.status == sanguinius::HouseMutationStatus::invalid_state ||
           second.status == sanguinius::HouseMutationStatus::invalid_state));
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_wager WHERE offer_id IN "
              "(SELECT offer_id FROM tarot_house_offer WHERE is_test=1)") == 1);
  REQUIRE(fixture.house
              ->inspect_control(
                  {.invocation = fixture.call("control:closed", 11'001),
                   .token_id = control_id})
              .status == sanguinius::HouseControlStatus::unavailable);
  REQUIRE(fixture.house->economy().valid);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM outbox_message WHERE "
              "last_error_code='house_offer_claimed' AND state='cancelled'") ==
          1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_public_card WHERE offer_id='" +
              *offer.offer_id + "' AND terminal_edit_outbox_id IS NOT NULL") ==
          1);
  const auto edit =
      durable.claim_due_outbox(11'000, 21'000, "claim-edit", uuid(2'501), true);
  REQUIRE(edit.has_value());
  REQUIRE(edit->kind == "discord.message_edit.v1");
  const auto *edit_payload =
      std::get_if<sanguinius::PublicEditOutboxPayload>(&edit->payload);
  REQUIRE(edit_payload != nullptr);
  REQUIRE(edit_payload->replacement.message.buttons.empty());
  REQUIRE(edit_payload->replacement.message.content.find("has been claimed") !=
          std::string::npos);
  REQUIRE_FALSE(durable.claim_due_outbox(11'001, 21'001, "claim-funded-blocked",
                                         uuid(2'502), true));
  REQUIRE(durable.mark_public_outbox_submitted(
              *edit,
              {.wall_time_ms = 11'002,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              21'002) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*edit, sanguinius::DiscordId{5'500},
                                         11'002) ==
          sanguinius::WorkMutationStatus::applied);
  const auto funded = durable.claim_due_outbox(11'003, 21'003, "claim-funded",
                                               uuid(2'503), true);
  REQUIRE(funded.has_value());
  REQUIRE(funded->kind == sanguinius::public_discord_outbox_kind);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM scheduled_job WHERE "
              "job_type='tarot.house-offer-expiry.v1' AND state='cancelled' "
              "AND last_error_code='house_offer_claimed'") == 1);
}

TEST_CASE("weekly offer reminder and expiry are durable and idempotent",
          "[tarot][house][weekly][reminder][expiry][restart][idempotency]") {
  HouseFixture fixture;
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  constexpr std::int64_t reminder_due = slot + 24LL * 60 * 60 * 1'000;
  const auto expiry_due =
      sanguinius::house_weekly_boundaries_ms(slot).closes_at_ms;
  auto request = sanguinius::HouseWeeklyOfferRequest{
      .job = fixture.weekly_job(slot),
      .definition = &definition,
      .catalog_version = fixture.house_catalog.version,
      .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      .now_ms = slot,
      .exposure_cap = 100,
      .operational = true,
      .is_test = false,
      .next_id = fixture.ids(4'500)};
  const auto created = fixture.house->handle_weekly_offer(request);
  REQUIRE(created.status == sanguinius::HouseWeeklyOfferStatus::created);
  REQUIRE(created.offer_id.has_value());
  const auto replay = fixture.house->handle_weekly_offer(request);
  REQUIRE(replay.status == sanguinius::HouseWeeklyOfferStatus::replay);
  REQUIRE(replay.offer_id == created.offer_id);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM outbox_message WHERE aggregate_type="
                     "'tarot_house_offer' AND aggregate_id='" +
                     *created.offer_id + "'") == 2);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM scheduled_job job JOIN "
              "tarot_house_offer_deadline deadline ON deadline.job_id=job."
              "job_id WHERE deadline.offer_id='" +
              *created.offer_id +
              "' AND job.job_type='tarot.house-offer-expiry.v1' AND "
              "job.due_at_ms=" +
              std::to_string(expiry_due)) == 1);

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto complete_public = [&](const sanguinius::ClaimedOutboxMessage &row,
                                   const std::int64_t now_ms,
                                   const std::uint64_t message_id) {
    REQUIRE(
        durable.mark_public_outbox_submitted(
            row,
            {.wall_time_ms = now_ms,
             .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
             .boot_session_id = std::string{fixture.clock.boot_session_id()}},
            now_ms + 10'000) == sanguinius::WorkMutationStatus::applied);
    REQUIRE(durable.complete_public_outbox(
                row, sanguinius::DiscordId{message_id}, now_ms) ==
            sanguinius::WorkMutationStatus::applied);
  };
  const auto source = durable.claim_due_outbox(
      slot, slot + 60'000, "weekly-source", uuid(4'600), true);
  REQUIRE(source.has_value());
  REQUIRE(source->kind == sanguinius::public_discord_outbox_kind);
  complete_public(*source, slot, 5'000);

  const auto reminder =
      durable.claim_due_outbox(reminder_due, reminder_due + 60'000,
                               "weekly-reminder", uuid(4'601), true);
  REQUIRE(reminder.has_value());
  REQUIRE(reminder->kind == sanguinius::public_discord_outbox_kind);
  const auto *reminder_payload =
      std::get_if<sanguinius::PublicOutboxPayload>(&reminder->payload);
  REQUIRE(reminder_payload != nullptr);
  REQUIRE(reminder_payload->request.message.content.find("remains open") !=
          std::string::npos);
  REQUIRE(reminder_payload->request.message.content.find("balance") ==
          std::string::npos);
  REQUIRE(reminder_payload->request.message.content.find("stake") ==
          std::string::npos);
  complete_public(*reminder, reminder_due, 5'001);
  REQUIRE_FALSE(durable.claim_due_outbox(reminder_due + 1,
                                         reminder_due + 60'001, "weekly-extra",
                                         uuid(4'602), true));

  const auto expiry = durable.claim_due_job(expiry_due, expiry_due + 60'000,
                                            "weekly-expiry", uuid(4'603));
  REQUIRE(expiry.has_value());
  REQUIRE(expiry->job_type == sanguinius::tarot_house_offer_expiry_job_type);
  const auto *expiry_payload =
      std::get_if<sanguinius::HouseOfferExpiryJobPayload>(&expiry->payload);
  REQUIRE(expiry_payload != nullptr);
  REQUIRE(expiry_payload->offer_id == created.offer_id);
  fixture.house.reset();
  fixture.house =
      std::make_unique<sanguinius::persistence::SqliteTarotHouseRepository>(
          fixture.context);
  const auto expired = fixture.house->handle_offer_expiry(
      {.job = *expiry, .now_ms = expiry_due, .next_id = fixture.ids(4'700)});
  REQUIRE(expired.status == sanguinius::HouseOfferExpiryStatus::expired);
  REQUIRE(expired.offer_id == created.offer_id);
  REQUIRE(fixture.text_scalar("SELECT state FROM tarot_house_offer WHERE "
                              "offer_id='" +
                              *created.offer_id + "'") == "closed");
  REQUIRE(fixture.scalar("SELECT count(*) FROM event_journal WHERE event_type="
                         "'tarot.house_offer_expired.v1' AND aggregate_id='" +
                         *created.offer_id + "'") == 1);
  REQUIRE(fixture.house->economy().non_test_exposure == 0);
  REQUIRE(fixture.house->economy().valid);
  const auto terminal_edit = durable.claim_due_outbox(
      expiry_due, expiry_due + 60'000, "weekly-expiry-edit", uuid(4'605), true);
  REQUIRE(terminal_edit.has_value());
  REQUIRE(terminal_edit->kind == "discord.message_edit.v1");
  const auto *terminal_payload =
      std::get_if<sanguinius::PublicEditOutboxPayload>(&terminal_edit->payload);
  REQUIRE(terminal_payload != nullptr);
  REQUIRE(terminal_payload->replacement.message.buttons.empty());
  REQUIRE(terminal_payload->replacement.message.content.find(
              "closed unclaimed") != std::string::npos);
  complete_public(*terminal_edit, expiry_due, 5'000);
  REQUIRE_FALSE(durable.claim_due_job(expiry_due + 1, expiry_due + 60'001,
                                      "weekly-expiry-replay", uuid(4'604)));
  const auto expiry_replay =
      fixture.house->handle_offer_expiry({.job = *expiry,
                                          .now_ms = expiry_due + 1,
                                          .next_id = fixture.ids(4'800)});
  REQUIRE(expiry_replay.status == sanguinius::HouseOfferExpiryStatus::replay);
  REQUIRE(fixture.scalar("SELECT count(*) FROM event_journal WHERE event_type="
                         "'tarot.house_offer_expired.v1' AND aggregate_id='" +
                         *created.offer_id + "'") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_public_card WHERE offer_id='" +
              *created.offer_id +
              "' AND terminal_edit_outbox_id IS NOT NULL") == 1);
}

TEST_CASE("weekly reminder waits for its source card and cancels if it fails",
          "[tarot][house][weekly][reminder][outbox][ordering]") {
  HouseFixture fixture;
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  constexpr std::int64_t reminder_due = slot + 24LL * 60 * 60 * 1'000;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = false,
       .next_id = fixture.ids(4'900)});
  REQUIRE(offer.offer_id.has_value());

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto source = durable.claim_due_outbox(
      slot, reminder_due + 60'000, "source-worker", uuid(4'950), true);
  REQUIRE(source.has_value());
  REQUIRE(source->outbox_id ==
          fixture.text_scalar("SELECT create_outbox_id FROM "
                              "tarot_house_public_card WHERE offer_id='" +
                              *offer.offer_id + "'"));

  REQUIRE_FALSE(durable.claim_due_outbox(reminder_due, reminder_due + 60'000,
                                         "reminder-worker", uuid(4'951), true));
  const auto reminder_id =
      fixture.text_scalar("SELECT reminder_outbox_id FROM "
                          "tarot_house_public_card WHERE offer_id='" +
                          *offer.offer_id + "'");
  REQUIRE(fixture.text_scalar("SELECT state FROM outbox_message WHERE "
                              "outbox_id='" +
                              reminder_id + "'") == "pending");

  REQUIRE(durable.fail_outbox(*source, reminder_due, reminder_due,
                              "injected_source_failure",
                              sanguinius::OutboxFailureMode::failed) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE_FALSE(durable.claim_due_outbox(reminder_due + 1,
                                         reminder_due + 60'001,
                                         "reminder-worker", uuid(4'952), true));
  REQUIRE(fixture.text_scalar("SELECT state FROM outbox_message WHERE "
                              "outbox_id='" +
                              reminder_id + "'") == "cancelled");
  REQUIRE(fixture.text_scalar("SELECT last_error_code FROM outbox_message "
                              "WHERE outbox_id='" +
                              reminder_id + "'") ==
          "house_offer_source_unavailable");
  REQUIRE(fixture.text_scalar("SELECT state FROM tarot_house_offer WHERE "
                              "offer_id='" +
                              *offer.offer_id + "'") == "closed");
  REQUIRE(fixture.house->economy().non_test_exposure == 0);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_offer_delivery_failure WHERE "
              "offer_id='" +
              *offer.offer_id +
              "' AND source_outbox_id='" + source->outbox_id +
              "' AND terminal_state='failed' AND "
              "error_code='injected_source_failure'") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_control WHERE offer_id='" +
              *offer.offer_id + "' AND state='cancelled'") == 1);
  REQUIRE(fixture.house
              ->availability(fixture.call("availability:failed-source",
                                          reminder_due + 2),
                             definition, false, 100)
              .status ==
          sanguinius::HouseAvailabilityStatus::no_scheduled_offer);
  REQUIRE(fixture.house
              ->play({.invocation = fixture.call("play:failed-source",
                                                 reminder_due + 2),
                      .definition = &definition,
                      .catalog_version = fixture.house_catalog.version,
                      .choice_slug = "yes",
                      .stake = 1,
                      .visibility = sanguinius::TarotVisibility::public_result,
                      .exposure_cap = 100,
                      .profit_cap = 20,
                      .starting_fate = 100,
                      .is_test = false,
                      .offer_id = offer.offer_id,
                      .next_id = fixture.ids(4'960)})
              .status == sanguinius::HouseMutationStatus::invalid_state);
}

TEST_CASE("scheduled House offers exclude their exact closing instant",
          "[tarot][house][weekly][boundary][sqlite]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 4'980);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = true,
       .next_id = fixture.ids(5'000)});
  REQUIRE(offer.offer_id.has_value());
  const auto closes_at_ms = fixture.scalar(
      "SELECT closes_at_ms FROM tarot_house_offer WHERE offer_id='" +
      *offer.offer_id + "'");

  const auto play = fixture.house->play(
      {.invocation = fixture.call("house:exact-close", closes_at_ms),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 1,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = true,
       .offer_id = offer.offer_id,
       .next_id = fixture.ids(5'100)});
  REQUIRE(play.status == sanguinius::HouseMutationStatus::invalid_state);
  REQUIRE_FALSE(play.wager.has_value());
  REQUIRE(fixture.tarot->balance(30) == 100);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_wager") == 0);
  REQUIRE(fixture.text_scalar("SELECT state FROM tarot_house_offer WHERE "
                              "offer_id='" +
                              *offer.offer_id + "'") == "open");
}

TEST_CASE("weekly play waits for its public source card delivery",
          "[tarot][house][weekly][outbox][ordering][privacy]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 55);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(10'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = 10'000,
       .exposure_cap = 100,
       .operational = true,
       .is_test = true,
       .next_id = fixture.ids(5'000)});
  REQUIRE(offer.offer_id.has_value());
  REQUIRE(fixture.house
              ->play({.invocation =
                          fixture.call("house:undelivered-source", 11'000),
                      .definition = &definition,
                      .catalog_version = fixture.house_catalog.version,
                      .choice_slug = "yes",
                      .stake = 1,
                      .visibility = sanguinius::TarotVisibility::public_result,
                      .exposure_cap = 100,
                      .profit_cap = 20,
                      .starting_fate = 100,
                      .is_test = true,
                      .offer_id = offer.offer_id,
                      .next_id = fixture.ids(5'100)})
              .status == sanguinius::HouseMutationStatus::invalid_state);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_wager") == 0);
  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto source = durable.claim_due_outbox(11'000, 21'000, "source",
                                               uuid(5'200), true);
  REQUIRE(source.has_value());
  REQUIRE(fixture.text_scalar(
              "SELECT aggregate_type FROM outbox_message WHERE outbox_id='" +
              source->outbox_id + "'") == "tarot_house_offer");
  REQUIRE(durable.mark_public_outbox_submitted(
              *source,
              {.wall_time_ms = 11'000,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              21'000) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*source, sanguinius::DiscordId{5'200},
                                         11'000) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE(fixture.house
              ->play({.invocation =
                          fixture.call("house:delivered-source", 11'001),
                      .definition = &definition,
                      .catalog_version = fixture.house_catalog.version,
                      .choice_slug = "yes",
                      .stake = 1,
                      .visibility = sanguinius::TarotVisibility::public_result,
                      .exposure_cap = 100,
                      .profit_cap = 20,
                      .starting_fate = 100,
                      .is_test = true,
                      .offer_id = offer.offer_id,
                      .next_id = fixture.ids(5'300)})
              .status == sanguinius::HouseMutationStatus::applied);
  const auto edit = durable.claim_due_outbox(11'001, 21'001, "edit",
                                             uuid(5'500), true);
  REQUIRE(edit.has_value());
  REQUIRE(edit->kind == "discord.message_edit.v1");
}

TEST_CASE("weekly Last Standard skips unsafe slots without catch-up",
          "[tarot][house][weekly][sqlite][schedule]") {
  HouseFixture fixture;
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  auto request = sanguinius::HouseWeeklyOfferRequest{
      .job = fixture.weekly_job(slot),
      .definition = &definition,
      .catalog_version = fixture.house_catalog.version,
      .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
      .now_ms = slot,
      .exposure_cap = 100,
      .operational = true,
      .is_test = false,
      .next_id = fixture.ids(4'100)};

  SECTION("a backward clock defers the same slot without artifacts") {
    request.now_ms = slot - 1;
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::deferred);
    REQUIRE_FALSE(result.offer_id.has_value());
    REQUIRE_FALSE(result.outbox_created);
    REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_offer") == 0);
    REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE "
                           "aggregate_type='tarot_house_offer'") == 0);
    REQUIRE(fixture.scalar("SELECT count(*) FROM scheduled_job WHERE "
                           "job_type='tarot.house-offer-expiry.v1'") == 0);
  }

  SECTION("a slot more than fifteen minutes late is missed") {
    request.now_ms = slot + 15 * 60 * 1'000 + 1;
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE_FALSE(result.outbox_created);
    REQUIRE(fixture.text_scalar(
                "SELECT skip_reason FROM tarot_house_offer WHERE offer_id='" +
                *result.offer_id + "'") == "missed_slot");
  }

  SECTION("degraded runtime skips the slot") {
    request.operational = false;
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE(fixture.text_scalar(
                "SELECT skip_reason FROM tarot_house_offer WHERE offer_id='" +
                *result.offer_id + "'") == "runtime_degraded");
  }

  SECTION("quiet mode skips the slot") {
    fixture.context->connection().execute(
        "UPDATE appearance_control_state SET quiet_until_ms=20000,"
        "quiet_set_by_user_id='30',quiet_reason='duration',updated_at_ms=10000 "
        "WHERE singleton=1");
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE(fixture.text_scalar(
                "SELECT skip_reason FROM tarot_house_offer WHERE offer_id='" +
                *result.offer_id + "'") == "quiet_or_disabled");
  }

  SECTION("global appearance disable skips the slot") {
    fixture.context->connection().execute(
        "UPDATE appearance_control_state SET globally_disabled=1,"
        "disabled_by_user_id='30',disabled_at_ms=10000,updated_at_ms=10000 "
        "WHERE singleton=1");
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE(fixture.text_scalar(
                "SELECT skip_reason FROM tarot_house_offer WHERE offer_id='" +
                *result.offer_id + "'") == "quiet_or_disabled");
  }

  SECTION("insufficient exposure headroom skips the slot") {
    request.exposure_cap = 5;
    const auto result = fixture.house->handle_weekly_offer(request);
    REQUIRE(result.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE(fixture.text_scalar(
                "SELECT skip_reason FROM tarot_house_offer WHERE offer_id='" +
                *result.offer_id + "'") == "exposure_blocked");
  }

  SECTION("duplicate delivery replays the skipped audit without an outbox") {
    request.operational = false;
    const auto first = fixture.house->handle_weekly_offer(request);
    request.next_id = fixture.ids(4'200);
    const auto replay = fixture.house->handle_weekly_offer(request);
    REQUIRE(first.status == sanguinius::HouseWeeklyOfferStatus::skipped);
    REQUIRE(replay.status == sanguinius::HouseWeeklyOfferStatus::replay);
    REQUIRE(replay.offer_id == first.offer_id);
    REQUIRE_FALSE(replay.outbox_created);
    REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_offer") == 1);
    REQUIRE(fixture.scalar("SELECT count(*) FROM outbox_message WHERE "
                           "aggregate_type='tarot_house_offer'") == 0);
  }
}

TEST_CASE("Last Standard records observed outcomes through owner authority",
          "[tarot][house][manual][authorization][idempotency]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 55);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  const auto observed_at =
      sanguinius::house_weekly_boundaries_ms(slot).closes_at_ms;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = false,
       .next_id = fixture.ids(4'500)});
  REQUIRE(offer.offer_id);
  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto source = durable.claim_due_outbox(
      slot, slot + 1'000, "source-worker", uuid(4'550), true);
  REQUIRE(source);
  REQUIRE(source->outbox_id ==
          fixture.text_scalar("SELECT create_outbox_id FROM "
                              "tarot_house_public_card WHERE offer_id='" +
                              *offer.offer_id + "'"));
  REQUIRE(durable.fail_outbox(*source, slot, slot + 1,
                              "injected_source_retry",
                              sanguinius::OutboxFailureMode::retryable) ==
          sanguinius::WorkMutationStatus::applied);
  const auto retried_source = durable.claim_due_outbox(
      slot + 1, slot + 900, "source-worker", uuid(4'551), true);
  REQUIRE(retried_source.has_value());
  REQUIRE(durable.mark_public_outbox_submitted(
              *retried_source,
              {.wall_time_ms = slot + 1,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              slot + 900) == sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*retried_source,
                                         sanguinius::DiscordId{4'551},
                                         slot + 1) ==
          sanguinius::WorkMutationStatus::applied);
  const auto play = fixture.house->play(
      {.invocation = fixture.call("house:manual:play", slot + 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = offer.offer_id,
       .next_id = fixture.ids(4'600)});
  REQUIRE(play.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(30) == 95);

  const auto early_request = sanguinius::HouseResolveRequest{
      .invocation = fixture.call("house:manual:early", observed_at - 1),
      .wager_id = play.wager->wager_id,
      .result = sanguinius::HouseResult::loss,
      .observed_choice = "no",
      .reason = "The observed result was not yet due",
      .automatic = false,
      .next_id = fixture.ids(4'700)};
  const auto early = fixture.house->resolve(early_request);
  REQUIRE(early.status == sanguinius::HouseMutationStatus::invalid_state);
  auto crossed_boundary = early_request;
  crossed_boundary.invocation.now_ms = observed_at;
  crossed_boundary.next_id = fixture.ids(4'750);
  const auto rejected_replay = fixture.house->resolve(crossed_boundary);
  REQUIRE(rejected_replay.status ==
          sanguinius::HouseMutationStatus::invalid_state);
  REQUIRE(rejected_replay.wager->state ==
          sanguinius::HouseWagerState::accepted_funded);
  REQUIRE(fixture.tarot->balance(30) == 95);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_resolution") == 0);
  auto early_reason_collision = crossed_boundary;
  early_reason_collision.reason = "A different owner observation";
  REQUIRE_THROWS_AS(fixture.house->resolve(early_reason_collision),
                    std::invalid_argument);
  const auto unauthorized = fixture.house->resolve(
      {.invocation = fixture.call("house:manual:member", observed_at, 31),
       .wager_id = play.wager->wager_id,
       .result = sanguinius::HouseResult::loss,
       .observed_choice = "no",
       .reason = "A member cannot record the House outcome",
       .automatic = false,
       .next_id = fixture.ids(4'800)});
  REQUIRE(unauthorized.status == sanguinius::HouseMutationStatus::forbidden);

  const auto request = sanguinius::HouseResolveRequest{
      .invocation = fixture.call("house:manual:owner", observed_at),
      .wager_id = play.wager->wager_id,
      .result = sanguinius::HouseResult::loss,
      .observed_choice = "no",
      .reason = "Play had ended before the appointed hour",
      .automatic = false,
      .next_id = fixture.ids(4'900)};
  auto forged_automatic = request;
  forged_automatic.automatic = true;
  REQUIRE_THROWS_AS(fixture.house->resolve(forged_automatic),
                    std::invalid_argument);
  const auto resolved = fixture.house->resolve(request);
  REQUIRE(resolved.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(resolved.wager->result == sanguinius::HouseResult::loss);
  REQUIRE(fixture.text_scalar(
              "SELECT authority FROM tarot_house_resolution WHERE wager_id='" +
              play.wager->wager_id + "'") == "owner");
  REQUIRE(fixture.tarot->balance(30) == 95);
  REQUIRE(fixture.house->resolve(request).status ==
          sanguinius::HouseMutationStatus::replay);
  auto conflicting = request;
  conflicting.observed_choice = "yes";
  REQUIRE_THROWS_AS(fixture.house->resolve(conflicting), std::invalid_argument);
  auto conflicting_reason = request;
  conflicting_reason.reason = "A conflicting owner reason";
  REQUIRE_THROWS_AS(fixture.house->resolve(conflicting_reason),
                    std::invalid_argument);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_public_card WHERE "
                         "outcome_outbox_id IS NOT NULL") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_receipt WHERE "
              "operation='resolve' AND request_fingerprint IS NOT NULL") == 3);
  REQUIRE(fixture.text_scalar(
              "SELECT status FROM tarot_house_receipt WHERE "
              "idempotency_key='house:manual:early'") == "invalid_state");
  REQUIRE(fixture.text_scalar(
              "SELECT status FROM tarot_house_receipt WHERE "
              "idempotency_key='house:manual:member'") == "forbidden");
  REQUIRE(
      fixture.text_scalar("SELECT state FROM outbox_message WHERE outbox_id='" +
                          source->outbox_id + "'") == "delivered");
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_public_card WHERE offer_id='" +
              *offer.offer_id + "' AND terminal_edit_outbox_id IS NOT NULL") ==
          1);
  const auto terminal_edit = durable.claim_due_outbox(
      observed_at, observed_at + 1'000, "edit-worker", uuid(4'999), true);
  REQUIRE(terminal_edit.has_value());
  REQUIRE(terminal_edit->kind == "discord.message_edit.v1");
  REQUIRE(durable.mark_public_outbox_submitted(
              *terminal_edit,
              {.wall_time_ms = observed_at,
               .elapsed_realtime_ms = 99,
               .boot_session_id = uuid(4'998)},
              observed_at + 1'000) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*terminal_edit, 7'001, observed_at) ==
          sanguinius::WorkMutationStatus::applied);
  const auto funded_public = durable.claim_due_outbox(
      observed_at, observed_at + 1'000, "funded-worker", uuid(5'000), true);
  REQUIRE(funded_public);
  const auto outcome_outbox = fixture.text_scalar(
      "SELECT outcome_outbox_id FROM tarot_house_public_card WHERE offer_id='" +
      *offer.offer_id + "'");
  REQUIRE(funded_public->outbox_id != outcome_outbox);
  REQUIRE(durable.mark_public_outbox_submitted(*funded_public,
                                               {.wall_time_ms = observed_at,
                                                .elapsed_realtime_ms = 100,
                                                .boot_session_id = uuid(5'001)},
                                               observed_at + 1'000) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE(durable.complete_public_outbox(*funded_public, 7'002, observed_at) ==
          sanguinius::WorkMutationStatus::applied);
  const auto outcome_public =
      durable.claim_due_outbox(observed_at + 1, observed_at + 1'001,
                               "outcome-worker", uuid(5'002), true);
  REQUIRE(outcome_public);
  REQUIRE(outcome_public->outbox_id == outcome_outbox);
  REQUIRE(fixture.house->economy().valid);
}

TEST_CASE("test Last Standard can be resolved immediately for staged review",
          "[tarot][house][manual][test][one-person]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 5'100);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 20'000;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = true,
       .next_id = fixture.ids(5'200)});
  REQUIRE(offer.status == sanguinius::HouseWeeklyOfferStatus::created);
  fixture.deliver_offer_source(*offer.offer_id, slot, 5'290);
  const auto play = fixture.house->play(
      {.invocation = fixture.call("house:test-manual:play", slot + 1),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = true,
       .offer_id = offer.offer_id,
       .next_id = fixture.ids(5'300)});
  REQUIRE(play.status == sanguinius::HouseMutationStatus::applied);
  const auto disabled = fixture.house->resolve(
      {.invocation = fixture.call("house:test-manual:disabled", slot + 2),
       .wager_id = play.wager->wager_id,
       .result = sanguinius::HouseResult::loss,
       .observed_choice = "no",
       .reason = "Deterministic staged-review observation",
       .automatic = false,
       .test_mode = false,
       .next_id = fixture.ids(5'400)});
  REQUIRE(disabled.status == sanguinius::HouseMutationStatus::forbidden);
  REQUIRE(disabled.wager->state ==
          sanguinius::HouseWagerState::accepted_funded);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_resolution") == 0);

  const auto resolved = fixture.house->resolve(
      {.invocation = fixture.call("house:test-manual:resolve", slot + 3),
       .wager_id = play.wager->wager_id,
       .result = sanguinius::HouseResult::loss,
       .observed_choice = "no",
       .reason = "Deterministic staged-review observation",
       .automatic = false,
       .test_mode = true,
       .next_id = fixture.ids(5'450)});
  REQUIRE(resolved.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(resolved.wager->result == sanguinius::HouseResult::loss);
  REQUIRE(fixture.house->economy().valid);
  REQUIRE(fixture.house
              ->cleanup_test_wager({.invocation = fixture.call(
                                        "house:test-manual:cleanup", slot + 4),
                                    .wager_id = play.wager->wager_id,
                                    .reason = "Retain staged-review audit",
                                    .owner = true,
                                    .test_mode = true,
                                    .next_id = fixture.ids(5'500)})
              .status == sanguinius::HouseMutationStatus::applied);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_offer WHERE offer_id='" + *offer.offer_id +
      "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_control WHERE offer_id='" + *offer.offer_id +
      "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_deadline WHERE wager_id='" +
      play.wager->wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_offer_deadline WHERE offer_id='" +
      *offer.offer_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_public_card WHERE offer_id='" + *offer.offer_id +
      "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_house_test_cleanup WHERE wager_id='" +
      play.wager->wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_offer SET proposition='rewritten' WHERE offer_id='" +
      *offer.offer_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_control SET action='rewritten' WHERE offer_id='" +
      *offer.offer_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_deadline SET due_at_ms=due_at_ms+1 WHERE wager_id='" +
      play.wager->wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_offer_deadline SET due_at_ms=due_at_ms+1 WHERE "
      "offer_id='" +
      *offer.offer_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_public_card SET created_revision=created_revision+1 "
      "WHERE offer_id='" +
      *offer.offer_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_house_test_cleanup SET reason='rewritten' WHERE "
      "wager_id='" +
      play.wager->wager_id + "'"));
}

TEST_CASE("test House cleanup exactly reverses loss and recovery ledgers",
          "[tarot][house][cleanup][sqlite][recovery]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 60);
  const auto &herald =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto loss = fixture.house->play(
      {.invocation = fixture.call("house:test-loss", 1'000),
       .definition = &herald,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = true,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(5'000)});
  REQUIRE(loss.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(
      fixture.house
          ->resolve_due(loss.wager->outcome_due_at_ms, true, fixture.ids(5'100))
          .size() == 1);
  REQUIRE(fixture.tarot->balance(30) == 95);
  const auto cleaned = fixture.house->cleanup_test_wager(
      {.invocation = fixture.call("house:cleanup:loss", 30'000),
       .wager_id = loss.wager->wager_id,
       .reason = "Restore staged balance",
       .owner = true,
       .test_mode = true,
       .next_id = fixture.ids(5'200)});
  REQUIRE(cleaned.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(30) == 100);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_house_test_cleanup") == 2);
  const auto replay = fixture.house->cleanup_test_wager(
      {.invocation = fixture.call("house:cleanup:loss", 30'000),
       .wager_id = loss.wager->wager_id,
       .reason = "Restore staged balance",
       .owner = true,
       .test_mode = true,
       .next_id = fixture.ids(5'300)});
  REQUIRE(replay.status == sanguinius::HouseMutationStatus::replay);
  REQUIRE(fixture.tarot->balance(30) == 100);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_house_receipt WHERE "
              "operation='test_cleanup' AND request_fingerprint IS NOT NULL") ==
          1);

  REQUIRE_THROWS_AS(
      fixture.house->cleanup_test_wager(
          {.invocation = fixture.call("house:cleanup:loss", 30'001),
           .wager_id = loss.wager->wager_id,
           .reason = "Changed cleanup reason",
           .owner = true,
           .test_mode = true,
           .next_id = fixture.ids(5'400)}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(
      fixture.house->cleanup_test_wager(
          {.invocation = fixture.call("house:cleanup:loss", 30'001),
           .wager_id = uuid(9'999),
           .reason = "Restore staged balance",
           .owner = true,
           .test_mode = true,
           .next_id = fixture.ids(5'400)}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(
      fixture.house->cleanup_test_wager(
          {.invocation = fixture.call("house:cleanup:loss", 30'001, 31),
           .wager_id = loss.wager->wager_id,
           .reason = "Restore staged balance",
           .owner = true,
           .test_mode = true,
           .next_id = fixture.ids(5'400)}),
      std::invalid_argument);
  auto changed_scope = fixture.call("house:cleanup:loss", 30'001);
  changed_scope.channel_id = 21;
  REQUIRE_THROWS_AS(
      fixture.house->cleanup_test_wager(
          {.invocation = changed_scope,
           .wager_id = loss.wager->wager_id,
           .reason = "Restore staged balance",
           .owner = true,
           .test_mode = true,
           .next_id = fixture.ids(5'400)}),
      std::invalid_argument);
  REQUIRE_THROWS_AS(
      fixture.house->cleanup_test_wager(
          {.invocation = fixture.call("house:test-loss", 30'001),
           .wager_id = loss.wager->wager_id,
           .reason = "Restore staged balance",
           .owner = true,
           .test_mode = true,
           .next_id = fixture.ids(5'400)}),
      std::invalid_argument);

  fixture.provision(31, 40, 70);
  const auto &recovery =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  const auto recovery_play = fixture.house->play(
      {.invocation = fixture.call("house:test-recovery", 40'000, 31),
       .definition = &recovery,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = true,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(6'000)});
  const auto draw = fixture.persist_draw(
      6'100, 31, sanguinius::TarotVisibility::private_result, 41'000, true);
  REQUIRE(
      fixture.house->observe_draw(draw, 41'000, fixture.ids(6'200)).size() ==
      1);
  REQUIRE(fixture.tarot->balance(31) == 45);
  REQUIRE(fixture.house
              ->cleanup_test_wager({.invocation = fixture.call(
                                        "house:cleanup:recovery", 42'000, 31),
                                    .wager_id = recovery_play.wager->wager_id,
                                    .reason = "Restore staged recovery",
                                    .owner = true,
                                    .test_mode = true,
                                    .next_id = fixture.ids(6'300)})
              .status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.tarot->balance(31) == 40);
  REQUIRE(fixture.house->economy().valid);
  REQUIRE(fixture.tarot->check_invariants().valid);
  sanguinius::persistence::SqliteWagerRepository wagers{fixture.context};
  REQUIRE(wagers.check_invariants().valid);
}

TEST_CASE(
    "Tarot integration retries atomically and gates private or opted-out draws",
    "[tarot][integration][sqlite][privacy][restart][idempotency]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 80);
  fixture.provision(31, 100, 90);
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=1 WHERE "
      "user_id='30'");
  const auto public_draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:public", 1'000),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = false,
       .is_test = false,
       .draw_id = uuid(7'000),
       .event_id = uuid(7'001),
       .public_outbox_id = uuid(17'000),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{4, 0}; }});
  REQUIRE(public_draw.status == sanguinius::TarotDrawStatus::drawn);

  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto failed = integration.scan(1'000, 32, [] { return "invalid"; });
  REQUIRE(failed.failed == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_effect_receipt") == 0);
  REQUIRE_FALSE(integration.retry(uuid(7'001), 61'000));

  sanguinius::persistence::SqliteTarotIntegrationRepository restarted{
      fixture.context};
  const auto completed = restarted.scan(61'000, 32, fixture.ids(7'100));
  REQUIRE(completed.completed == 1);
  REQUIRE(completed.failed == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  const auto public_handoff_id = fixture.text_scalar(
      "SELECT candidate_id FROM tarot_appearance_candidate WHERE "
      "source_event_id='" +
      uuid(7'001) + "'");
  REQUIRE(fixture.text_scalar(
              "SELECT sink_reference FROM tarot_integration_effect_receipt "
              "WHERE source_event_id='" +
              uuid(7'001) +
              "' AND sink_kind='appearance' AND sink_key='tarot_event'") ==
          public_handoff_id);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM appearance_event_observation WHERE "
                     "source_event_id='" +
                     uuid(7'001) + "'") == 1);
  std::ifstream policy_stream{fixture.config_path("appearance-policy-v2.json")};
  REQUIRE(policy_stream.good());
  const auto policy = sanguinius::parse_appearance_policy(
      std::string{std::istreambuf_iterator<char>{policy_stream},
                  std::istreambuf_iterator<char>{}});
  sanguinius::persistence::SqliteAppearanceRepository appearances{
      fixture.context};
  REQUIRE_NOTHROW(appearances.register_policy(policy, 61'000));
  std::vector<sanguinius::AppearanceCandidate> appearance_candidates;
  REQUIRE_NOTHROW(appearance_candidates = appearances.scan_events(
                      policy, 61'001, "tarot-integration-test"));
  REQUIRE(appearance_candidates.size() == 1);
  REQUIRE(appearance_candidates.front().type ==
          sanguinius::AppearanceCandidateType::tarot_event);
  REQUIRE(appearance_candidates.front().visible);
  REQUIRE(appearance_candidates.front().consented);
  REQUIRE(appearance_candidates.front().candidate_id == public_handoff_id);
  REQUIRE(fixture.scalar("SELECT count(*) FROM appearance_candidate WHERE "
                         "candidate_type='tarot_event'") == 1);
  REQUIRE(
      fixture.text_scalar("SELECT state FROM tarot_appearance_candidate WHERE "
                          "candidate_id='" +
                          public_handoff_id + "'") == "consumed");
  REQUIRE(fixture.text_scalar(
              "SELECT candidate_id FROM appearance_event_observation WHERE "
              "source_event_id='" +
              uuid(7'001) + "'") == public_handoff_id);
  const auto effect_count =
      fixture.scalar("SELECT count(*) FROM tarot_integration_effect_receipt");
  static_cast<void>(restarted.scan(61'001, 32, fixture.ids(7'200)));
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM tarot_integration_effect_receipt") ==
      effect_count);

  static_cast<void>(fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:private", 2'000, 31),
       .visibility = sanguinius::TarotVisibility::private_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = false,
       .is_test = false,
       .draw_id = uuid(7'300),
       .event_id = uuid(7'301),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{5, 0}; }}));
  static_cast<void>(restarted.scan(62'000, 32, fixture.ids(7'400)));
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);

  static_cast<void>(fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:opted-out", 3'000, 31),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = true,
       .is_test = false,
       .draw_id = uuid(7'500),
       .event_id = uuid(7'501),
       .public_outbox_id = uuid(17'500),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{6, 0}; }}));
  static_cast<void>(restarted.scan(63'000, 32, fixture.ids(7'600)));
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_effect_receipt WHERE "
              "source_event_id='" +
              uuid(7'501) + "' AND sink_reference='consent_suppressed'") == 2);

  appearances.activate_mode(sanguinius::AppearanceMode::dry_run, 63'500);
  const auto test_draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:test", 64'000),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = true,
       .is_test = true,
       .draw_id = uuid(7'700),
       .event_id = uuid(7'701),
       .public_outbox_id = uuid(17'700),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{7, 0}; }});
  REQUIRE(test_draw.status == sanguinius::TarotDrawStatus::drawn);
  const auto test_report = restarted.scan(64'000, 32, fixture.ids(7'800));
  REQUIRE(test_report.failed == 0);
  const auto test_handoff_id = fixture.text_scalar(
      "SELECT candidate_id FROM tarot_appearance_candidate WHERE "
      "source_event_id='" +
      uuid(7'701) + "'");
  const auto test_candidates =
      appearances.scan_events(policy, 64'001, "tarot-test-dry-run");
  REQUIRE(test_candidates.size() == 1);
  REQUIRE(test_candidates.front().candidate_id == test_handoff_id);
  REQUIRE(test_candidates.front().owner_simulation);
  REQUIRE(fixture.scalar("SELECT owner_simulation FROM appearance_candidate "
                         "WHERE candidate_id='" +
                         test_handoff_id + "'") == 1);
  REQUIRE(
      fixture.text_scalar("SELECT state FROM tarot_appearance_candidate WHERE "
                          "candidate_id='" +
                          test_handoff_id + "'") == "consumed");

  const auto suppressed_draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:mode-off", 65'000),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = true,
       .is_test = true,
       .draw_id = uuid(7'900),
       .event_id = uuid(7'901),
       .public_outbox_id = uuid(17'900),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{8, 0}; }});
  REQUIRE(suppressed_draw.status == sanguinius::TarotDrawStatus::drawn);
  REQUIRE(restarted.scan(65'000, 32, fixture.ids(8'000)).failed == 0);
  appearances.activate_mode(sanguinius::AppearanceMode::off, 65'001);
  REQUIRE(
      fixture.text_scalar("SELECT state FROM tarot_appearance_candidate WHERE "
                          "source_event_id='" +
                          uuid(7'901) + "'") == "suppressed");
  REQUIRE(fixture.text_scalar(
              "SELECT extraction_result FROM appearance_event_observation "
              "WHERE source_event_id='" +
              uuid(7'901) + "'") == "mode_off");
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate "
                         "WHERE state='pending'") == 0);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_appearance_candidate SET safe_summary='changed' WHERE "
      "candidate_id='" +
      public_handoff_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_appearance_candidate WHERE candidate_id='" +
      public_handoff_id + "'"));
}

TEST_CASE(
    "integration attempt cap suppresses only its source and test retry resets",
    "[tarot][integration][retry][cap][test]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 7'610);
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=1 WHERE "
      "user_id='30'");
  static_cast<void>(fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:capped", 1'000),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = true,
       .is_test = true,
       .draw_id = uuid(7'620),
       .event_id = uuid(7'621),
       .public_outbox_id = uuid(17'620),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{8, 0}; }}));
  fixture.context->connection().execute(
      "UPDATE tarot_integration_observation SET attempts=99 WHERE "
      "source_event_id='" +
      uuid(7'621) + "'");
  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto capped = integration.scan(1'000, 32, [] { return "invalid"; });
  REQUIRE(capped.suppressed == 1);
  REQUIRE(
      fixture.scalar("SELECT attempts FROM tarot_integration_observation WHERE "
                     "source_event_id='" +
                     uuid(7'621) + "'") == 100);

  static_cast<void>(fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:after-cap", 2'000),
       .visibility = sanguinius::TarotVisibility::private_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = true,
       .is_test = false,
       .draw_id = uuid(7'630),
       .event_id = uuid(7'631),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{9, 0}; }}));
  const auto continued = integration.scan(2'000, 32, fixture.ids(7'640));
  REQUIRE(continued.completed == 1);
  REQUIRE(continued.suppressed == 1);
  REQUIRE(integration.retry(uuid(7'621), 3'000));
  const auto retried = integration.scan(3'000, 32, fixture.ids(7'650));
  REQUIRE(retried.completed == 2);
  REQUIRE(retried.suppressed == 0);
  REQUIRE_FALSE(integration.retry(uuid(7'631), 4'000));
}

TEST_CASE("real settlement retries beyond the cap before later player results",
          "[tarot][integration][retry][cap][ordering][restart]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 7'660);
  const auto &herald =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto play = [&](const std::string &key, const std::int64_t now_ms,
                        const std::size_t id) {
    return fixture.house->play(
        {.invocation = fixture.call(key, now_ms),
         .definition = &herald,
         .catalog_version = fixture.house_catalog.version,
         .choice_slug = "answer",
         .stake = 5,
         .visibility = sanguinius::TarotVisibility::private_result,
         .exposure_cap = 100,
         .profit_cap = 20,
         .starting_fate = 100,
         .is_test = false,
         .offer_id = std::nullopt,
         .next_id = fixture.ids(id)});
  };

  const auto first = play("house:real-cap:first", 1'000, 7'700);
  REQUIRE(first.status == sanguinius::HouseMutationStatus::applied);
  const auto first_terminal = fixture.house->resolve_due(
      first.wager->outcome_due_at_ms, false, fixture.ids(7'800));
  REQUIRE(first_terminal.size() == 1);
  const auto first_event = fixture.text_scalar(
      "SELECT terminal_event_id FROM tarot_house_wager WHERE wager_id='" +
      first.wager->wager_id + "'");

  const auto second_at =
      first.wager->outcome_due_at_ms + first.wager->terminal_cooldown_ms;
  const auto second = play("house:real-cap:second", second_at, 7'900);
  REQUIRE(second.status == sanguinius::HouseMutationStatus::applied);
  const auto second_terminal = fixture.house->resolve_due(
      second.wager->outcome_due_at_ms, false, fixture.ids(8'000));
  REQUIRE(second_terminal.size() == 1);
  const auto second_event = fixture.text_scalar(
      "SELECT terminal_event_id FROM tarot_house_wager WHERE wager_id='" +
      second.wager->wager_id + "'");

  fixture.context->connection().execute(
      "UPDATE tarot_integration_observation SET attempts=99 WHERE "
      "source_event_id='" +
      first_event + "'");
  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto failed = integration.scan(second.wager->outcome_due_at_ms, 32,
                                       [] { return std::string{"invalid"}; });
  REQUIRE(failed.failed == 2);
  REQUIRE(failed.suppressed == 0);
  REQUIRE(
      fixture.scalar("SELECT attempts FROM tarot_integration_observation WHERE "
                     "source_event_id='" +
                     first_event + "'") == 100);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_player_event WHERE "
                         "baseline=0") == 0);

  sanguinius::persistence::SqliteTarotIntegrationRepository restarted{
      fixture.context};
  const auto recovered = restarted.scan(
      second.wager->outcome_due_at_ms + 60'000, 32, fixture.ids(8'100));
  REQUIRE(recovered.failed == 0);
  REQUIRE(recovered.suppressed == 0);
  REQUIRE(recovered.completed == 2);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM tarot_integration_observation WHERE "
                     "source_event_id IN ('" +
                     first_event + "','" + second_event +
                     "') AND state='complete'") == 2);
  REQUIRE(fixture.scalar("SELECT attempts FROM tarot_integration_observation "
                         "WHERE source_event_id='" +
                         first_event + "'") == 101);
  const auto record = fixture.house->record(30);
  REQUIRE(record.losses == 2);
  REQUIRE(record.current_loss_streak == 2);
  REQUIRE(record.settled_house_wagers == 2);
  REQUIRE(fixture.house->check_player_projection().valid);
}

TEST_CASE("competing Tarot integration workers produce each effect once",
          "[tarot][integration][sqlite][concurrency][idempotency]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 80);
  fixture.context->connection().execute(
      "UPDATE user_preference SET appearance_callback_opt_in=1 WHERE "
      "user_id='30'");
  const auto draw = fixture.draws->draw(
      {.invocation = fixture.call("draw:integration:race", 1'000),
       .visibility = sanguinius::TarotVisibility::public_result,
       .cooldown_ms = 86'400'000,
       .bypass_cooldown = false,
       .is_test = false,
       .draw_id = uuid(7'700),
       .event_id = uuid(7'701),
       .public_outbox_id = uuid(17'700),
       .sample = [] { return std::pair<std::int64_t, std::int64_t>{7, 0}; }});
  REQUIRE(draw.status == sanguinius::TarotDrawStatus::drawn);

  auto second_context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          sanguinius::persistence::Database::open_runtime(
              fixture.temporary.path()));
  sanguinius::persistence::SqliteTarotIntegrationRepository first{
      fixture.context};
  sanguinius::persistence::SqliteTarotIntegrationRepository second{
      second_context};
  auto first_ids = fixture.ids(7'800);
  auto second_ids = fixture.ids(7'900);
  std::atomic<bool> start{};
  std::optional<sanguinius::TarotIntegrationReport> first_report;
  std::optional<sanguinius::TarotIntegrationReport> second_report;
  std::exception_ptr first_error;
  std::exception_ptr second_error;

  std::thread first_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    try {
      first_report = first.scan(1'000, 32, first_ids);
    } catch (...) {
      first_error = std::current_exception();
    }
  }};
  std::thread second_thread{[&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    try {
      second_report = second.scan(1'000, 32, second_ids);
    } catch (...) {
      second_error = std::current_exception();
    }
  }};
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  REQUIRE(first_error == nullptr);
  REQUIRE(second_error == nullptr);
  REQUIRE(first_report.has_value());
  REQUIRE(second_report.has_value());
  REQUIRE(first_report->completed == 1);
  REQUIRE(second_report->completed == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_observation WHERE "
              "source_event_id='" +
              uuid(7'701) + "' AND state='complete' AND attempts=1") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_effect_receipt WHERE "
              "source_event_id='" +
              uuid(7'701) + "'") == 2);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);
}

TEST_CASE("disabled Chronicle blocks integration-owned relationship and title sinks",
          "[tarot][integration][chronicle][feature-flag]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 10'100);
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1,"
      "appearance_callback_opt_in=1 WHERE user_id='30'");
  fixture.seed_player_event(10'200, 30, "win", "house", 100);
  fixture.seed_player_event(10'201, 30, "win", "house", 200);
  REQUIRE(fixture.house->rebuild_player_projection().valid);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:chronicle-disabled", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(10'300)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  const auto draw = fixture.persist_draw(
      10'400, 30, sanguinius::TarotVisibility::private_result, 2'000);
  REQUIRE(fixture.house->observe_draw(draw, 2'000, fixture.ids(10'500)).size() ==
          1);

  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto report = integration.scan(
      2'000, 32, fixture.ids(10'600),
      sanguinius::TarotIntegrationSinkPolicy{.chronicle_enabled = false});
  REQUIRE(report.failed == 0);
  REQUIRE(fixture.house->record(30).current_win_streak == 3);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_title_source") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_title_definition") ==
          0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_title_grant") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM relationship_event") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal") == 0);

  const auto &last_standard =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  const auto observed_at =
      sanguinius::house_weekly_boundaries_ms(slot).closes_at_ms;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &last_standard,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = false,
       .next_id = fixture.ids(10'700)});
  REQUIRE(offer.offer_id.has_value());
  fixture.deliver_offer_source(*offer.offer_id, slot, 10'800);
  const auto notable = fixture.house->play(
      {.invocation = fixture.call("house:chronicle-disabled:notable",
                                  slot + 1'000),
       .definition = &last_standard,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = offer.offer_id,
       .next_id = fixture.ids(10'900)});
  REQUIRE(notable.status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(fixture.house
              ->resolve({.invocation = fixture.call(
                             "house:chronicle-disabled:resolve", observed_at),
                         .wager_id = notable.wager->wager_id,
                         .result = sanguinius::HouseResult::win,
                         .observed_choice = "yes",
                         .reason = "Play remained underway at the appointed hour",
                         .automatic = false,
                         .next_id = fixture.ids(11'000)})
              .status == sanguinius::HouseMutationStatus::applied);
  REQUIRE(integration
              .scan(observed_at, 32, fixture.ids(11'200),
                    sanguinius::TarotIntegrationSinkPolicy{
                        .chronicle_enabled = false})
              .failed == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM relationship_event") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal") == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);
}

TEST_CASE("Chronicle consent is independent from appearance callback consent",
          "[tarot][integration][chronicle][consent][privacy]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 10'500);
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1,"
      "appearance_callback_opt_in=0 WHERE user_id='30'");
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "last-standard");
  constexpr std::int64_t slot = 10'000;
  const auto observed_at =
      sanguinius::house_weekly_boundaries_ms(slot).closes_at_ms;
  const auto offer = fixture.house->handle_weekly_offer(
      {.job = fixture.weekly_job(slot),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .scope = {.guild_id = 10, .primary_channel_id = 20, .owner_user_id = 30},
       .now_ms = slot,
       .exposure_cap = 100,
       .operational = true,
       .is_test = false,
       .next_id = fixture.ids(10'600)});
  REQUIRE(offer.offer_id.has_value());
  fixture.deliver_offer_source(*offer.offer_id, slot, 10'700);
  const auto play = fixture.house->play(
      {.invocation = fixture.call("house:consent:play", slot + 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "yes",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = offer.offer_id,
       .next_id = fixture.ids(10'800)});
  REQUIRE(play.status == sanguinius::HouseMutationStatus::applied);
  const auto resolved = fixture.house->resolve(
      {.invocation = fixture.call("house:consent:resolve", observed_at),
       .wager_id = play.wager->wager_id,
       .result = sanguinius::HouseResult::win,
       .observed_choice = "yes",
       .reason = "Play remained underway at the appointed hour",
       .automatic = false,
       .next_id = fixture.ids(11'000)});
  REQUIRE(resolved.status == sanguinius::HouseMutationStatus::applied);

  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto report = integration.scan(observed_at, 32, fixture.ids(11'200));
  REQUIRE(report.failed == 0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          0);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          0);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_integration_effect_receipt WHERE "
              "source_event_id=(SELECT terminal_event_id FROM "
              "tarot_house_wager WHERE wager_id='" +
              play.wager->wager_id +
              "') AND sink_reference='consent_suppressed'") == 2);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal WHERE "
                         "source_event_id=(SELECT terminal_event_id FROM "
                         "tarot_house_wager WHERE wager_id='" +
                         play.wager->wager_id + "')") == 1);
}

TEST_CASE("Tarot player projection rebuilds by durable settlement order",
          "[tarot][projection][sqlite][rebuild][rollback]") {
  HouseFixture fixture;
  fixture.seed_player_event(7'600, 30, "loss", "peer", 300);
  fixture.seed_player_event(7'601, 30, "win", "house", 100);
  fixture.seed_player_event(7'602, 30, "win", "house", 100);
  fixture.seed_player_event(7'603, 30, "void", "house", 100);
  const auto rebuilt = fixture.house->rebuild_player_projection();
  REQUIRE(rebuilt.valid);
  REQUIRE(rebuilt.event_count == 4);
  REQUIRE(rebuilt.projection_count == 1);
  REQUIRE(fixture.scalar(
              "SELECT wins FROM tarot_player_stats WHERE user_id='30'") == 2);
  REQUIRE(
      fixture.scalar("SELECT current_win_streak FROM tarot_player_stats WHERE "
                     "user_id='30'") == 2);
  REQUIRE(
      fixture.scalar("SELECT current_loss_streak FROM tarot_player_stats WHERE "
                     "user_id='30'") == 0);
  REQUIRE(fixture.scalar(
              "SELECT settled_house_wagers FROM tarot_player_stats WHERE "
              "user_id='30'") == 3);
  fixture.context->connection().execute(
      "UPDATE tarot_player_stats SET wins=99,current_win_streak=99 WHERE "
      "user_id='30'");
  const auto drift = fixture.house->check_player_projection();
  REQUIRE_FALSE(drift.valid);
  REQUIRE(drift.mismatch_count == 1);
  REQUIRE(fixture.house->rebuild_player_projection().valid);
  REQUIRE(fixture.scalar(
              "SELECT wins FROM tarot_player_stats WHERE user_id='30'") == 2);
  REQUIRE(
      fixture.scalar("SELECT current_win_streak FROM tarot_player_stats WHERE "
                     "user_id='30'") == 2);
  REQUIRE(fixture.scalar(
              "SELECT settled_house_wagers FROM tarot_player_stats WHERE "
              "user_id='30'") == 3);
}

TEST_CASE("void House settlement counts toward Keeper without changing streaks",
          "[tarot][integration][projection][void][title]") {
  HouseFixture fixture;
  fixture.provision(31, 40, 7'700);
  for (std::size_t index = 0; index < 9; ++index)
    fixture.seed_player_event(7'710 + index, 31, "void", "house",
                              100 + static_cast<std::int64_t>(index));
  REQUIRE(fixture.house->rebuild_player_projection().valid);
  REQUIRE(fixture.house->record(31).settled_house_wagers == 9);

  const auto &recovery =
      sanguinius::house_template(fixture.house_catalog, "returning-dawn");
  const auto played = fixture.house->play(
      {.invocation = fixture.call("house:void-title", 1'000, 31),
       .definition = &recovery,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "rise",
       .stake = 0,
       .visibility = sanguinius::TarotVisibility::private_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(7'800)});
  REQUIRE(played.status == sanguinius::HouseMutationStatus::applied);
  const auto settled = fixture.house->resolve_due(
      played.wager->outcome_due_at_ms, false, fixture.ids(7'900));
  REQUIRE(settled.size() == 1);
  REQUIRE(settled.front().wager->result == sanguinius::HouseResult::void_wager);

  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto report =
      integration.scan(played.wager->outcome_due_at_ms, 32, fixture.ids(8'000));
  REQUIRE(report.failed == 0);
  const auto record = fixture.house->record(31);
  REQUIRE(record.wins == 0);
  REQUIRE(record.losses == 0);
  REQUIRE(record.current_win_streak == 0);
  REQUIRE(record.current_loss_streak == 0);
  REQUIRE(record.settled_house_wagers == 10);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_title_source WHERE user_id='31' "
              "AND title_name='Keeper of the Last Standard'") == 1);
  REQUIRE(fixture.house->check_player_projection().valid);
}

TEST_CASE("House title threshold creates once-only approved-system proposals",
          "[tarot][integration][title][chronicle][appearance][relationship]") {
  HouseFixture fixture;
  fixture.provision(30, 100, 100);
  fixture.context->connection().execute(
      "UPDATE user_preference SET chronicle_opt_in=1,"
      "appearance_callback_opt_in=1 WHERE user_id='30'");
  fixture.seed_player_event(7'800, 30, "win", "house", 100);
  fixture.seed_player_event(7'801, 30, "win", "house", 200);
  REQUIRE(fixture.house->rebuild_player_projection().valid);
  const auto &definition =
      sanguinius::house_template(fixture.house_catalog, "heralds-call");
  const auto wager = fixture.house->play(
      {.invocation = fixture.call("house:title", 1'000),
       .definition = &definition,
       .catalog_version = fixture.house_catalog.version,
       .choice_slug = "answer",
       .stake = 5,
       .visibility = sanguinius::TarotVisibility::public_result,
       .exposure_cap = 100,
       .profit_cap = 20,
       .starting_fate = 100,
       .is_test = false,
       .offer_id = std::nullopt,
       .next_id = fixture.ids(8'000)});
  const auto draw = fixture.persist_draw(
      8'100, 30, sanguinius::TarotVisibility::private_result, 2'000);
  const auto settlement =
      fixture.house->observe_draw(draw, 2'000, fixture.ids(8'200));
  REQUIRE(settlement.size() == 1);
  REQUIRE(settlement.front().wager->result == sanguinius::HouseResult::win);

  sanguinius::persistence::SqliteTarotIntegrationRepository integration{
      fixture.context};
  const auto report = integration.scan(2'000, 32, fixture.ids(8'400));
  REQUIRE(report.failed == 0);
  REQUIRE(
      fixture.scalar(
          "SELECT count(*) FROM tarot_title_source WHERE title_name="
          "'Favored of the Cast Die' AND title_definition_id IS NOT NULL") ==
      1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_title_grant WHERE "
                         "state='proposed'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal WHERE "
                         "status='submitted'") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_entry WHERE status='proposed' "
              "AND source_kind='tarot_event' AND source_message_id IS NULL "
              "AND source_event_id=(SELECT source_event_id FROM "
              "tarot_chronicle_proposal)") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM chronicle_approval WHERE state='pending' "
              "AND reviewer_user_id='30' AND entry_id=(SELECT proposal_id FROM "
              "tarot_chronicle_proposal)") == 1);
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM pending_notice WHERE state='pending' AND "
              "target_user_id='30' AND source_aggregate_id=(SELECT proposal_id "
              "FROM tarot_chronicle_proposal)") == 1);
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM chronicle_title_definition WHERE "
                     "provenance='tarot_system'") == 1);
  sanguinius::persistence::SqliteChronicleSessionRepository titles{
      fixture.context};
  const auto proposed_titles = titles.list_titles(30, 30, true, 0);
  REQUIRE(proposed_titles.total == 1);
  REQUIRE(proposed_titles.grants.size() == 1);
  REQUIRE(proposed_titles.grants.front().provenance ==
          sanguinius::ChronicleTitleProvenance::tarot_system);
  REQUIRE(fixture.scalar("SELECT count(*) FROM relationship_event WHERE "
                         "reason_code='tarot.honored'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_appearance_candidate") ==
          1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_vox_narration_intent") ==
          1);
  const auto effects =
      fixture.scalar("SELECT count(*) FROM tarot_integration_effect_receipt");
  static_cast<void>(integration.scan(2'001, 32, fixture.ids(8'600)));
  REQUIRE(
      fixture.scalar("SELECT count(*) FROM tarot_integration_effect_receipt") ==
      effects);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_title_grant") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal") == 1);
  REQUIRE(fixture.house->record(30).pending_titles.size() == 1);

  const auto title_approved =
      titles.mutate_title({.grant_id = proposed_titles.grants.front().grant_id,
                           .expected_revision = 1,
                           .guild_id = 10,
                           .channel_id = 20,
                           .actor_user_id = 30,
                           .owner_user_id = 30,
                           .action = sanguinius::TitleAction::approve,
                           .award_entry_id = uuid(10'000),
                           .event_id = uuid(10'001),
                           .outbox_id = uuid(10'002),
                           .relationship_event_id = uuid(10'003),
                           .idempotency_key = "tarot:title:approve",
                           .correlation_id = "tarot-title-test",
                           .now_ms = 2'500});
  REQUIRE(title_approved.code ==
          sanguinius::ChronicleSessionResultCode::updated);
  REQUIRE(title_approved.grant->provenance ==
          sanguinius::ChronicleTitleProvenance::tarot_system);
  REQUIRE(fixture.text_scalar(
              "SELECT state FROM tarot_title_source WHERE title_name="
              "'Favored of the Cast Die'") == "approved");
  REQUIRE(fixture.house->record(30).pending_titles.empty());

  fixture.context->connection().execute(
      "INSERT OR IGNORE INTO tarot_title_source(source_event_id,user_id,"
      "title_name,title_definition_id,state,created_at_ms) SELECT "
      "accepted_event_id,'30','Favored of the Cast Die',NULL,'proposed',2600 "
      "FROM tarot_house_wager WHERE wager_id='" +
      wager.wager->wager_id + "'");
  REQUIRE(fixture.scalar(
              "SELECT count(*) FROM tarot_title_source WHERE user_id='30' "
              "AND title_name='Favored of the Cast Die'") == 1);

  sanguinius::persistence::SqliteChronicleSessionRepository restarted_titles{
      fixture.context};
  const auto active_titles = restarted_titles.list_titles(30, 30, true, 0);
  REQUIRE(active_titles.grants.front().provenance ==
          sanguinius::ChronicleTitleProvenance::tarot_system);
  const auto title_revoked = restarted_titles.mutate_title(
      {.grant_id = active_titles.grants.front().grant_id,
       .expected_revision = 2,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 30,
       .owner_user_id = 30,
       .action = sanguinius::TitleAction::revoke,
       .award_entry_id = uuid(10'010),
       .event_id = uuid(10'011),
       .outbox_id = uuid(10'012),
       .relationship_event_id = uuid(10'013),
       .idempotency_key = "tarot:title:revoke",
       .correlation_id = "tarot-title-test",
       .now_ms = 2'600});
  REQUIRE(title_revoked.code ==
          sanguinius::ChronicleSessionResultCode::updated);
  REQUIRE(title_revoked.grant->provenance ==
          sanguinius::ChronicleTitleProvenance::tarot_system);
  REQUIRE(fixture.text_scalar(
              "SELECT state FROM tarot_title_source WHERE title_name="
              "'Favored of the Cast Die'") == "revoked");

  const auto approve_token = fixture.text_scalar(
      "SELECT token_id FROM interaction_token WHERE "
      "action='chronicle.entry.approve' AND entity_id=(SELECT approval_id FROM "
      "chronicle_approval WHERE entry_id=(SELECT proposal_id FROM "
      "tarot_chronicle_proposal))");
  sanguinius::persistence::SqliteChronicleRepository chronicle{fixture.context};
  const auto approval_request = sanguinius::ApplyApprovalRequest{
      .token_id = approve_token,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .action_event_id = uuid(9'000),
      .canon_event_id = uuid(9'001),
      .public_outbox_id = uuid(9'002),
      .correlation_id = "tarot-chronicle-approval",
      .interaction_idempotency_key = "tarot:chronicle:approve",
      .now_ms = 3'000};
  const auto approved = chronicle.apply_approval(approval_request);
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == sanguinius::ChronicleEntryStatus::canon);
  REQUIRE(fixture.scalar("SELECT count(*) FROM tarot_chronicle_proposal WHERE "
                         "status='approved'") == 1);
  REQUIRE(fixture.scalar("SELECT count(*) FROM chronicle_entry WHERE "
                         "status='canon' AND source_kind='tarot_event'") == 1);
  const auto replayed = chronicle.apply_approval(approval_request);
  REQUIRE(replayed.code == sanguinius::ChronicleResultCode::unchanged);
  REQUIRE(fixture.scalar("SELECT count(*) FROM event_journal WHERE "
                         "event_type='chronicle.entry_canonized.v1'") == 1);
  REQUIRE(wager.wager->visibility ==
          sanguinius::TarotVisibility::public_result);
}
