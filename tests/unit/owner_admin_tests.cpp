#include "sanguinius/owner_admin.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("owner admin prefix parser accepts only the health operation",
          "[admin]") {
  REQUIRE(sanguinius::parse_admin_operation("!sang-admin health", "!") ==
          sanguinius::AdminOperation::health);
  REQUIRE(sanguinius::parse_admin_operation("!SANG-ADMIN   HEALTH", "!") ==
          sanguinius::AdminOperation::health);
  REQUIRE_FALSE(sanguinius::parse_admin_operation("!sang-admin health now", "!")
                    .has_value());
  REQUIRE_FALSE(
      sanguinius::parse_admin_operation("!sang-admin reset", "!").has_value());
  REQUIRE_FALSE(
      sanguinius::parse_admin_operation("?sang-admin health", "!").has_value());
}

TEST_CASE("owner admin service applies enabled scope and owner boundaries",
          "[admin]") {
  const sanguinius::ServerScopePolicy policy{{10, 20, 30}};
  const sanguinius::HealthService health{
      {"test-version", "test-revision"},
      {.admin_commands_enabled = true, .test_mode = false},
      {},
      {true, 1, 1, "3.53.4", "instance"},
      {.interaction_queue =
           [] {
             return sanguinius::QueueSnapshot{
                 .capacity = 16, .queued = 2, .accepting = true};
           },
       .scheduler_queue = [] { return sanguinius::QueueSnapshot{}; },
       .outbox_queue = [] { return sanguinius::QueueSnapshot{}; },
       .pending_notice_count = [] { return std::size_t{3}; },
       .durable_work = [] { return sanguinius::DurableWorkHealth{}; },
       .tarot = {},
       .wagers = {},
       .house = {},
       .vox = {},
       .vox_narration = {},
       .voice_input = {}}};
  const sanguinius::OwnerAdminService service{
      {.admin_commands_enabled = true, .test_mode = false}, policy, health};

  const auto allowed =
      service.handle({{10, 20, 30}, sanguinius::AdminOperation::health},
                     {.capacity = 64, .active = 1, .accepting = true},
                     {.capacity = 64, .accepting = true});
  REQUIRE(allowed.authorization.allowed());
  REQUIRE(allowed.health.has_value());
  REQUIRE(allowed.health->scope_matched);
  REQUIRE(allowed.health->interaction_queue.capacity == 16);
  REQUIRE(allowed.health->interaction_queue.queued == 2);
  REQUIRE(allowed.health->pending_notice_count == 3);

  const auto wrong_guild = service.handle(
      {{11, 20, 30}, sanguinius::AdminOperation::health}, {}, {});
  REQUIRE(wrong_guild.authorization.status ==
          sanguinius::AdminStatus::rejected);
  REQUIRE(wrong_guild.authorization.rejection ==
          sanguinius::ScopeRejection::wrong_guild);
  REQUIRE_FALSE(wrong_guild.health.has_value());

  const auto wrong_channel = service.handle(
      {{10, 21, 30}, sanguinius::AdminOperation::health}, {}, {});
  REQUIRE(wrong_channel.authorization.rejection ==
          sanguinius::ScopeRejection::wrong_channel);

  const auto non_owner = service.handle(
      {{10, 20, 31}, sanguinius::AdminOperation::health}, {}, {});
  REQUIRE(non_owner.authorization.rejection ==
          sanguinius::ScopeRejection::owner_required);
}

TEST_CASE("owner admin service is disabled independently of owner identity",
          "[admin]") {
  const sanguinius::ServerScopePolicy policy{{10, 20, 30}};
  const sanguinius::HealthService health{
      {"version", "revision"},
      {},
      {},
      {true, 1, 1, "3.53.4", "instance"},
      {.interaction_queue = [] { return sanguinius::QueueSnapshot{}; },
       .scheduler_queue = [] { return sanguinius::QueueSnapshot{}; },
       .outbox_queue = [] { return sanguinius::QueueSnapshot{}; },
       .pending_notice_count = [] { return std::size_t{}; },
       .durable_work = [] { return sanguinius::DurableWorkHealth{}; },
       .tarot = {},
       .wagers = {},
       .house = {},
       .vox = {},
       .vox_narration = {},
       .voice_input = {}}};
  const sanguinius::OwnerAdminService service{{}, policy, health};

  const auto result = service.handle(
      {{10, 20, 30}, sanguinius::AdminOperation::health}, {}, {});
  REQUIRE(result.authorization.status == sanguinius::AdminStatus::disabled);
  REQUIRE_FALSE(result.health.has_value());
}
