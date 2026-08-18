#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string_view>

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

[[nodiscard]] sanguinius::CreatePendingNoticeRequest
notice_request(std::string notice_id = "00000000-0000-4000-8000-000000000101",
               std::string token_id = "00000000-0000-4000-8000-000000000102",
               std::string creation_key = "notice:create:101",
               std::string token_key = "token:create:102",
               const std::int64_t created_at_ms = 1'000) {
  return sanguinius::CreatePendingNoticeRequest{
      .notice_id = std::move(notice_id),
      .token_id = std::move(token_id),
      .target_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .notice_type = "test_notice",
      .content = {"Private title", "Private body"},
      .source_aggregate_type = "owner_test",
      .source_aggregate_id = "interaction-101",
      .expires_at_ms = created_at_ms + 10'000,
      .notice_idempotency_key = std::move(creation_key),
      .token_idempotency_key = std::move(token_key),
      .created_at_ms = created_at_ms,
  };
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

TEST_CASE(
    "pending notice creation and component open are atomic and replayable",
    "[repository][notice][idempotency]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteCoreIdentityRepository identities{
      fixture.context};
  identities.ensure_user({.user_id = 30,
                          .display_name = "Owner",
                          .username = "owner",
                          .is_bot = false,
                          .observed_at_ms = 1'000});
  identities.ensure_user({.user_id = 31,
                          .display_name = "Other",
                          .username = "other",
                          .is_bot = false,
                          .observed_at_ms = 1'000});
  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};

  const auto request = notice_request();
  const auto created = notices.create_with_token(request);
  REQUIRE(created.created);
  REQUIRE(created.notice.state == sanguinius::PendingNoticeState::pending);
  const auto duplicate = notices.create_with_token(request);
  REQUIRE_FALSE(duplicate.created);
  REQUIRE(duplicate.notice.notice_id == created.notice.notice_id);
  REQUIRE(duplicate.token_id == created.token_id);
  REQUIRE(notices.pending_count(30, 2'000) == 1);
  REQUIRE(notices.pending_count_all(2'000) == 1);

  REQUIRE(notices
              .open_by_token({
                  .token_id = created.token_id,
                  .interaction_kind = sanguinius::InteractionTokenKind::select,
                  .guild_id = 10,
                  .channel_id = 20,
                  .user_id = 30,
                  .interaction_idempotency_key = "notice:component:wrong-kind",
                  .now_ms = 2'000,
              })
              .status == sanguinius::OpenPendingNoticeStatus::wrong_kind);
  REQUIRE(notices
              .open_by_token({
                  .token_id = created.token_id,
                  .interaction_kind = sanguinius::InteractionTokenKind::button,
                  .guild_id = 10,
                  .channel_id = 21,
                  .user_id = 30,
                  .interaction_idempotency_key = "notice:component:wrong-scope",
                  .now_ms = 2'000,
              })
              .status == sanguinius::OpenPendingNoticeStatus::wrong_scope);
  REQUIRE(notices
              .open_by_token({
                  .token_id = created.token_id,
                  .interaction_kind = sanguinius::InteractionTokenKind::button,
                  .guild_id = 10,
                  .channel_id = 20,
                  .user_id = 31,
                  .interaction_idempotency_key = "notice:component:wrong-user",
                  .now_ms = 2'000,
              })
              .status == sanguinius::OpenPendingNoticeStatus::wrong_user);

  const sanguinius::OpenNoticeByTokenRequest open{
      .token_id = created.token_id,
      .interaction_kind = sanguinius::InteractionTokenKind::button,
      .guild_id = 10,
      .channel_id = 20,
      .user_id = 30,
      .interaction_idempotency_key = "notice:component:open",
      .now_ms = 2'000,
  };
  const auto opened = notices.open_by_token(open);
  REQUIRE(opened.status == sanguinius::OpenPendingNoticeStatus::opened);
  REQUIRE(opened.notice->content.body == "Private body");
  REQUIRE(notices.pending_count(30, 2'000) == 1);
  REQUIRE(notices.confirm_open_delivery("notice:component:open", 2'001) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  REQUIRE(notices.open_by_token(open).notice->content ==
          opened.notice->content);
  REQUIRE(notices.pending_count(30, 2'000) == 0);
  REQUIRE(notices.consume(created.notice.notice_id, 31, 3'000) ==
          sanguinius::PendingNoticeMutationStatus::wrong_user);
  REQUIRE(notices.consume(created.notice.notice_id, 30, 3'000) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  REQUIRE(notices.consume(created.notice.notice_id, 30, 3'001) ==
          sanguinius::PendingNoticeMutationStatus::unchanged);
  REQUIRE(notices.open_by_token(open).status ==
          sanguinius::OpenPendingNoticeStatus::unavailable);
}

TEST_CASE("pending notice inbox cancellation expiry and rollback fail closed",
          "[repository][notice][rollback]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteCoreIdentityRepository identities{
      fixture.context};
  identities.ensure_user({.user_id = 30,
                          .display_name = std::nullopt,
                          .username = "owner",
                          .is_bot = false,
                          .observed_at_ms = 1'000});
  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};

  const auto first = notices.create_with_token(notice_request());
  const auto second = notices.create_with_token(
      notice_request("00000000-0000-4000-8000-000000000103",
                     "00000000-0000-4000-8000-000000000104",
                     "notice:create:103", "token:create:104", 2'000));
  const auto inbox =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:500",
                         .now_ms = 3'000});
  REQUIRE(inbox.notice->notice_id == first.notice.notice_id);
  const auto replay =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:500",
                         .now_ms = 3'001});
  REQUIRE(replay.notice->notice_id == first.notice.notice_id);
  REQUIRE(notices.confirm_open_delivery("notice:inbox:500", 3'002) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  REQUIRE(notices.cancel(second.notice.notice_id, 30, 3'000) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  REQUIRE(notices.cancel(second.notice.notice_id, 30, 3'001) ==
          sanguinius::PendingNoticeMutationStatus::unchanged);

  const auto expiring = notices.create_with_token(
      notice_request("00000000-0000-4000-8000-000000000105",
                     "00000000-0000-4000-8000-000000000106",
                     "notice:create:105", "token:create:106", 4'000));
  REQUIRE(notices.expire_due(14'000) == 2);
  REQUIRE(notices
              .open_by_token({
                  .token_id = expiring.token_id,
                  .interaction_kind = sanguinius::InteractionTokenKind::button,
                  .guild_id = 10,
                  .channel_id = 20,
                  .user_id = 30,
                  .interaction_idempotency_key = "notice:component:expired",
                  .now_ms = 14'000,
              })
              .status == sanguinius::OpenPendingNoticeStatus::expired);

  auto conflict =
      notice_request("00000000-0000-4000-8000-000000000107",
                     "00000000-0000-4000-8000-000000000108",
                     "notice:create:107", "token:create:102", 5'000);
  REQUIRE_THROWS_AS(notices.create_with_token(conflict),
                    sanguinius::persistence::DatabaseError);
  auto count = fixture.context->connection().prepare(
      "SELECT count(*) FROM pending_notice WHERE notice_id = "
      "'00000000-0000-4000-8000-000000000107'");
  REQUIRE(count.step());
  REQUIRE(count.column_int64(0) == 0);

  REQUIRE_THROWS_AS(
      fixture.context->connection().execute(
          "INSERT INTO pending_notice "
          "(notice_id, target_user_id, notice_type, payload_json, state, "
          "expires_at_ms, idempotency_key, created_at_ms) VALUES "
          "('00000000-0000-4000-8000-000000000109', '30', 'bad', "
          "'not-json', 'pending', 10, 'invalid-json', 1)"),
      sanguinius::persistence::DatabaseError);
}

TEST_CASE("pending notice delivery failure and empty inbox replay stay safe",
          "[repository][notice][delivery][idempotency]") {
  RepositoryFixture fixture;
  sanguinius::persistence::SqliteCoreIdentityRepository identities{
      fixture.context};
  identities.ensure_user({.user_id = 30,
                          .display_name = std::nullopt,
                          .username = "owner",
                          .is_bot = false,
                          .observed_at_ms = 1'000});
  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};

  const auto empty =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:empty",
                         .now_ms = 1'000});
  REQUIRE(empty.status ==
          sanguinius::OpenPendingNoticeStatus::no_pending_notice);
  REQUIRE(notices.confirm_open_delivery("notice:inbox:empty", 1'001) ==
          sanguinius::PendingNoticeMutationStatus::applied);

  const auto created = notices.create_with_token(notice_request());
  const auto empty_replay =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:empty",
                         .now_ms = 2'000});
  REQUIRE(empty_replay.status ==
          sanguinius::OpenPendingNoticeStatus::no_pending_notice);
  REQUIRE(notices.pending_count(30, 2'000) == 1);

  const auto failed =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:failed",
                         .now_ms = 2'001});
  REQUIRE(failed.notice->notice_id == created.notice.notice_id);
  REQUIRE(notices.pending_count(30, 2'001) == 1);
  REQUIRE(notices.release_open_delivery("notice:inbox:failed", 2'002) ==
          sanguinius::PendingNoticeMutationStatus::applied);

  const auto retry =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:retry",
                         .now_ms = 2'003});
  REQUIRE(retry.notice->notice_id == created.notice.notice_id);
  REQUIRE(notices.confirm_open_delivery("notice:inbox:retry", 2'004) ==
          sanguinius::PendingNoticeMutationStatus::applied);
  REQUIRE(notices.pending_count(30, 2'004) == 0);
}

TEST_CASE("incomplete notice reveals recover across a repository restart",
          "[repository][notice][delivery][restart]") {
  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  {
    auto database = sanguinius::persistence::Database::open_migration(
        temporary.path(), 25ms);
    sanguinius::persistence::Migrator migrator{
        sanguinius::persistence::production_migrations(),
        {"test", "revision"},
        clock};
    static_cast<void>(migrator.apply(database.connection()));
    auto context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
    sanguinius::persistence::SqliteCoreIdentityRepository identities{context};
    identities.ensure_user({.user_id = 30,
                            .display_name = std::nullopt,
                            .username = "owner",
                            .is_bot = false,
                            .observed_at_ms = 1'000});
    sanguinius::persistence::SqlitePendingNoticeRepository notices{context};
    static_cast<void>(notices.create_with_token(notice_request()));
    const auto prepared = notices.open_next(
        {.user_id = 30,
         .interaction_idempotency_key = "notice:inbox:interrupted",
         .now_ms = 2'000});
    REQUIRE(prepared.status == sanguinius::OpenPendingNoticeStatus::opened);
    REQUIRE(notices.pending_count(30, 2'000) == 1);
  }

  {
    auto database =
        sanguinius::persistence::Database::open_runtime(temporary.path(), 25ms);
    auto context =
        std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
            std::move(database));
    sanguinius::persistence::SqlitePendingNoticeRepository notices{context};
    REQUIRE(notices.recover_incomplete_open_deliveries(3'000) == 1);
    const auto recovered = notices.open_next(
        {.user_id = 30,
         .interaction_idempotency_key = "notice:inbox:restart",
         .now_ms = 3'001});
    REQUIRE(recovered.status == sanguinius::OpenPendingNoticeStatus::opened);
    REQUIRE(recovered.notice->content.title == "Private title");
    REQUIRE(notices.confirm_open_delivery("notice:inbox:restart", 3'002) ==
            sanguinius::PendingNoticeMutationStatus::applied);
    REQUIRE(notices.pending_count(30, 3'002) == 0);
  }

  auto database =
      sanguinius::persistence::Database::open_runtime(temporary.path(), 25ms);
  auto context =
      std::make_shared<sanguinius::persistence::SqliteRepositoryContext>(
          std::move(database));
  sanguinius::persistence::SqlitePendingNoticeRepository notices{context};
  const auto replay =
      notices.open_next({.user_id = 30,
                         .interaction_idempotency_key = "notice:inbox:restart",
                         .now_ms = 4'000});
  REQUIRE(replay.status == sanguinius::OpenPendingNoticeStatus::opened);
  REQUIRE(replay.notice->content.title == "Private title");
}
