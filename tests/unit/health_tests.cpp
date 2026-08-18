#include "sanguinius/health.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {

[[nodiscard]] bool contains(const std::string_view value,
                            const std::string_view fragment) {
  return value.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("health snapshot renders build queues and configured modes",
          "[health]") {
  const sanguinius::HealthService service{
      {"2.1.0-test", "revision-test"},
      {.admin_commands_enabled = true, .test_mode = true},
      {.chronicle_enabled = true,
       .tarot_enabled = false,
       .appearances_mode = sanguinius::AppearanceMode::dry_run,
       .vox_enabled = true,
       .voice_input_enabled = false}};

  const auto snapshot = service.snapshot(
      {.capacity = 64, .queued = 3, .active = 1, .accepting = true},
      {.capacity = 32, .queued = 2, .active = 2, .accepting = false}, true);
  const auto rendered = sanguinius::render_health(snapshot);

  REQUIRE(contains(rendered, "version=2.1.0-test"));
  REQUIRE(contains(rendered, "revision=revision-test"));
  REQUIRE(contains(rendered, "message_queue=3/64 queued, 1 active, accepting"));
  REQUIRE(contains(rendered, "ai_queue=2/32 queued, 2 active, stopped"));
  REQUIRE(contains(rendered, "admin_commands=enabled"));
  REQUIRE(contains(rendered, "test_mode=enabled"));
  REQUIRE(contains(rendered, "appearances=dry_run"));
  REQUIRE(contains(rendered, "voice_input=disabled"));

  const sanguinius::HealthService oversized_metadata{
      {std::string(4'000, 'v'),
       "revision\ninjected=" + std::string(4'000, 'r')},
      {.admin_commands_enabled = true, .test_mode = true},
      {.appearances_mode = sanguinius::AppearanceMode::live}};
  const auto bounded = sanguinius::render_health(oversized_metadata.snapshot(
      {.capacity = 64, .queued = 1, .active = 1, .accepting = true},
      {.capacity = 64, .accepting = true}, true));
  REQUIRE(bounded.size() <= sanguinius::maximum_health_message_size);
  REQUIRE_FALSE(contains(bounded, "\ninjected="));
  REQUIRE(contains(bounded, "message_queue=1/64 queued, 1 active, accepting"));
  REQUIRE(contains(bounded, "appearances=live"));
}

TEST_CASE("health types cannot expose secret configuration", "[health]") {
  const sanguinius::HealthService service{
      {"safe-version", "safe-revision"}, {}, {}};
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
