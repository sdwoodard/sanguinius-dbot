#include "sanguinius/health.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {

class FakeStatusProvider final : public sanguinius::DiscordStatusProvider {
public:
  [[nodiscard]] sanguinius::DiscordRuntimeStatus
  status() const noexcept override {
    return {.ready = true,
            .command_registration =
                sanguinius::CommandRegistrationState::synchronized,
            .command_catalog_version = 2};
  }
};

[[nodiscard]] bool contains(const std::string_view value,
                            const std::string_view fragment) {
  return value.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("health snapshot renders build queues and configured modes",
          "[health]") {
  const FakeStatusProvider discord;
  const sanguinius::HealthService service{
      {"2.1.0-test", "revision-test"},
      {.admin_commands_enabled = true, .test_mode = true},
      {.chronicle_enabled = true,
       .tarot_enabled = false,
       .appearances_mode = sanguinius::AppearanceMode::dry_run,
       .vox_enabled = true,
       .voice_input_enabled = false},
      {true, 1, 1, "3.53.4", "00000000-0000-4000-8000-000000000001"},
      {.discord_status = &discord,
       .interaction_queue =
           [] {
             return sanguinius::QueueSnapshot{
                 .capacity = 16, .queued = 1, .active = 1, .accepting = true};
           },
       .scheduler_queue =
           [] {
             return sanguinius::QueueSnapshot{
                 .capacity = 32, .queued = 1, .active = 0, .accepting = true};
           },
       .outbox_queue =
           [] {
             return sanguinius::QueueSnapshot{
                 .capacity = 32, .queued = 2, .active = 1, .accepting = true};
           },
       .pending_notice_count = [] { return std::size_t{7}; },
       .durable_work =
           [] {
             return sanguinius::DurableWorkHealth{
                 .pending_jobs = 2,
                 .claimed_jobs = 1,
                 .dead_jobs = 3,
                 .pending_outbox = 4,
                 .claimed_outbox = 1,
                 .failed_outbox = 2,
                 .dead_outbox = 1,
                 .job_retries = 5,
                 .outbox_retries = 6,
                 .scheduler_lag_ms = 700,
                 .outbox_lag_ms = 800,
                 .last_job_error = "handler_exception",
                 .last_outbox_error = "discord_transient",
             };
           }}};

  const auto snapshot = service.snapshot(
      {.capacity = 64, .queued = 3, .active = 1, .accepting = true},
      {.capacity = 32, .queued = 2, .active = 2, .accepting = false}, true);
  const auto rendered = sanguinius::render_health(snapshot);

  REQUIRE(contains(rendered, "version=2.1.0-test"));
  REQUIRE(contains(rendered, "revision=revision-test"));
  REQUIRE(contains(rendered, "database=ready"));
  REQUIRE(contains(rendered, "schema=1/1"));
  REQUIRE(contains(rendered, "sqlite=3.53.4"));
  REQUIRE(contains(rendered, "message_queue=3/64 queued, 1 active, accepting"));
  REQUIRE(contains(rendered, "ai_queue=2/32 queued, 2 active, stopped"));
  REQUIRE(
      contains(rendered, "interaction_queue=1/16 queued, 1 active, accepting"));
  REQUIRE(contains(rendered, "scheduler_queue=1/32 queued, 0 active"));
  REQUIRE(contains(rendered, "outbox_queue=2/32 queued, 1 active"));
  REQUIRE(contains(rendered, "discord_ready=enabled"));
  REQUIRE(contains(rendered, "command_catalog=2"));
  REQUIRE(contains(rendered, "command_registration=synchronized"));
  REQUIRE(contains(rendered, "pending_notices=7"));
  REQUIRE(contains(rendered, "jobs=2 pending, 1 claimed, 3 dead, 5 retries"));
  REQUIRE(contains(rendered,
                   "outbox_work=4 pending, 1 claimed, 2 failed, 1 dead, 6 "
                   "retries"));
  REQUIRE(contains(rendered, "scheduler_lag_ms=700"));
  REQUIRE(contains(rendered, "last_outbox_error=discord_transient"));
  REQUIRE(contains(rendered, "admin_commands=enabled"));
  REQUIRE(contains(rendered, "test_mode=enabled"));
  REQUIRE(contains(rendered, "appearances=dry_run"));
  REQUIRE(contains(rendered, "voice_input=disabled"));

  const sanguinius::HealthService oversized_metadata{
      {std::string(4'000, 'v'),
       "revision\ninjected=" + std::string(4'000, 'r')},
      {.admin_commands_enabled = true, .test_mode = true},
      {.appearances_mode = sanguinius::AppearanceMode::dry_run},
      {true, 1, 1, std::string(4'000, 's'),
       "instance\ninjected=" + std::string(4'000, 'i')},
      {.interaction_queue = [] { return sanguinius::QueueSnapshot{}; },
       .scheduler_queue = [] { return sanguinius::QueueSnapshot{}; },
       .outbox_queue = [] { return sanguinius::QueueSnapshot{}; },
       .pending_notice_count = [] { return std::size_t{}; },
       .durable_work = [] { return sanguinius::DurableWorkHealth{}; }}};
  const auto bounded = sanguinius::render_health(oversized_metadata.snapshot(
      {.capacity = 64, .queued = 1, .active = 1, .accepting = true},
      {.capacity = 64, .accepting = true}, true));
  REQUIRE(bounded.size() <= sanguinius::maximum_health_message_size);
  REQUIRE_FALSE(contains(bounded, "\ninjected="));
  REQUIRE(contains(bounded, "message_queue=1/64 queued, 1 active, accepting"));
  REQUIRE(contains(bounded, "appearances=dry_run"));
}

TEST_CASE("health types cannot expose secret configuration", "[health]") {
  const sanguinius::HealthService service{
      {"safe-version", "safe-revision"},
      {},
      {},
      {true, 1, 1, "3.53.4", "id"},
      {.interaction_queue = [] { return sanguinius::QueueSnapshot{}; },
       .scheduler_queue = [] { return sanguinius::QueueSnapshot{}; },
       .outbox_queue = [] { return sanguinius::QueueSnapshot{}; },
       .pending_notice_count = [] { return std::size_t{}; },
       .durable_work = [] { return sanguinius::DurableWorkHealth{}; }}};
  const auto rendered = sanguinius::render_health(
      service.snapshot({.capacity = 1}, {.capacity = 1}, true));

  constexpr std::string_view forbidden[]{
      "DISCORD_SECRET_SENTINEL", "OPENAI_SECRET_SENTINEL",
      "PERSONA_SECRET_SENTINEL", "DATABASE_PATH_SENTINEL",
      "18446744073709551615",    "123456789012345678",
  };
  for (const auto value : forbidden) {
    REQUIRE_FALSE(contains(rendered, value));
  }
}
