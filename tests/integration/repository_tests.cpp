#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

namespace {

using namespace std::chrono_literals;

struct RepositoryFixture {
  RepositoryFixture() {
    auto database = sanguinius::persistence::Database::open_migration(
        temporary.path(), 25ms);
    sanguinius::persistence::Migrator migrator{
        sanguinius::persistence::production_migrations(),
        {"test-version", "test-revision"},
        clock};
    static_cast<void>(migrator.apply(database.connection()));
    context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<sanguinius::persistence::SqliteRepositoryContext> context;
};

[[nodiscard]] std::string
scalar_text(sanguinius::persistence::SqliteConnection &connection,
            const std::string_view sql) {
  auto statement = connection.prepare(sql);
  REQUIRE(statement.step());
  return statement.column_text(0);
}

} // namespace

TEST_CASE("core identity repository pins one configured scope and defaults",
          "[repository]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteCoreIdentityRepository identities{
      fixture.context};
  identities.initialize_or_validate_scope({10, 20, 30}, 1'000);
  REQUIRE_NOTHROW(identities.initialize_or_validate_scope({10, 20, 30}, 2'000));

  REQUIRE(scalar_text(fixture.context->connection(),
                      "SELECT guild_id FROM guild_config") == "10");
  REQUIRE(scalar_text(fixture.context->connection(),
                      "SELECT owner_user_id FROM guild_config") == "30");
  const auto preferences =
      identities.load_preferences(sanguinius::DiscordSnowflake{30});
  REQUIRE(preferences.has_value());
  REQUIRE_FALSE(preferences->chronicle_opt_in);
  REQUIRE_FALSE(preferences->voice_input_opt_in);
  REQUIRE(preferences->public_tarot_results_opt_in);
  REQUIRE_FALSE(preferences->quiet_until_ms.has_value());

  REQUIRE_THROWS_AS(
      identities.initialize_or_validate_scope({10, 21, 30}, 3'000),
      sanguinius::persistence::DatabaseError);
  REQUIRE_THROWS_AS(
      identities.initialize_or_validate_scope({11, 20, 30}, 3'000),
      sanguinius::persistence::DatabaseError);
  REQUIRE_THROWS_AS(
      identities.initialize_or_validate_scope({10, 20, 31}, 3'000),
      sanguinius::persistence::DatabaseError);
  REQUIRE(scalar_text(fixture.context->connection(),
                      "SELECT primary_channel_id FROM guild_config") == "20");
}

TEST_CASE("Discord user cache upsert preserves first observation and defaults",
          "[repository]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteCoreIdentityRepository identities{
      fixture.context};
  identities.ensure_user({.user_id = 42,
                          .display_name = "First",
                          .username = "first-user",
                          .is_bot = false,
                          .observed_at_ms = 100});
  identities.ensure_user({.user_id = 42,
                          .display_name = "Second",
                          .username = std::nullopt,
                          .is_bot = false,
                          .observed_at_ms = 200});
  identities.ensure_user({.user_id = 42,
                          .display_name = "Stale",
                          .username = "stale-user",
                          .is_bot = true,
                          .observed_at_ms = 50});

  auto user = fixture.context->connection().prepare(
      "SELECT display_name_cache, username_cache, first_seen_at_ms, "
      "last_seen_at_ms FROM discord_user WHERE user_id = '42'");
  REQUIRE(user.step());
  REQUIRE(user.column_text(0) == "Second");
  REQUIRE(user.column_text(1) == "first-user");
  REQUIRE(user.column_int64(2) == 50);
  REQUIRE(user.column_int64(3) == 200);
  REQUIRE(identities.load_preferences(sanguinius::DiscordSnowflake{42})
              .has_value());

  REQUIRE_THROWS_AS(
      fixture.context->connection().execute(
          "INSERT INTO user_preference (user_id, chronicle_opt_in, "
          "updated_at_ms) VALUES ('99', 2, 1)"),
      sanguinius::persistence::DatabaseError);
}

TEST_CASE("application instance repository records idempotent terminal state",
          "[repository]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteApplicationInstanceRepository instances{
      fixture.context};
  const std::string id{"00000000-0000-4000-8000-000000000001"};
  instances.record_start({.instance_id = id,
                          .application_version = "2.1.0",
                          .git_revision = "revision",
                          .hostname = "test-host",
                          .process_id = 123,
                          .started_at_ms = 1'000});
  instances.record_stop(id, 2'000,
                        sanguinius::ApplicationStopReason::clean_shutdown);
  REQUIRE_NOTHROW(instances.record_stop(
      id, 3'000, sanguinius::ApplicationStopReason::startup_failure));

  auto row = fixture.context->connection().prepare(
      "SELECT stopped_at_ms, stop_reason FROM application_instance "
      "WHERE instance_id = ?");
  row.bind(1, id);
  REQUIRE(row.step());
  REQUIRE(row.column_int64(0) == 2'000);
  REQUIRE(row.column_text(1) == "clean_shutdown");
  REQUIRE_THROWS_AS(
      instances.record_stop("00000000-0000-4000-8000-000000000002", 2'000,
                            sanguinius::ApplicationStopReason::clean_shutdown),
      sanguinius::persistence::DatabaseError);

  const std::string crash_id{"00000000-0000-4000-8000-000000000003"};
  instances.record_start({.instance_id = crash_id,
                          .application_version = "2.1.0",
                          .git_revision = "revision",
                          .hostname = "test-host",
                          .process_id = 124,
                          .started_at_ms = 3'000});
  auto crash = fixture.context->connection().prepare(
      "SELECT stopped_at_ms, stop_reason FROM application_instance "
      "WHERE instance_id = ?");
  crash.bind(1, crash_id);
  REQUIRE(crash.step());
  REQUIRE(crash.column_is_null(0));
  REQUIRE(crash.column_is_null(1));
}
