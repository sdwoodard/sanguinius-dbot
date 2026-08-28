#include "sanguinius/ai_generation.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/outbox.hpp"
#include "sanguinius/reliability_test.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Milestone 19 reliability probes are deterministic and provider-free",
          "[m19][reliability]") {
  const sanguinius::ReliabilityTestService service;
  REQUIRE(service.run("text-timeout").find("passed") != std::string::npos);
  REQUIRE(service.run("ai-saturation").find("passed") != std::string::npos);
  REQUIRE(service.run("discord-unknown").find("quarantined") !=
          std::string::npos);
  REQUIRE_THROWS_AS(service.run("invalid"), std::invalid_argument);
}

TEST_CASE("Milestone 19 reliability classifications fail conservatively",
          "[m19][reliability]") {
  const auto timeout = sanguinius::classify_ai_provider_failure(
      sanguinius::AiProviderErrorCategory::timeout, true);
  REQUIRE(timeout.action == sanguinius::AiFailureAccounting::fail_attempt);
  REQUIRE(timeout.result_code == "timeout");

  const auto unknown = sanguinius::classify_discord_delivery_failure(
      sanguinius::DeliveryResult::unknown_outcome, false);
  REQUIRE_FALSE(unknown.retry);
  REQUIRE(unknown.mode == sanguinius::OutboxFailureMode::failed);
  REQUIRE(unknown.error_code == "discord_unknown_outcome_stale");
}

TEST_CASE("Milestone 19 health renders bounded operational readiness",
          "[m19][health]") {
  sanguinius::HealthSnapshot snapshot{};
  snapshot.build = {.version = "2.2.0",
                    .revision = "0123456789abcdef0123456789abcdef01234567",
                    .release_id = "2.2.0-grevision"};
  snapshot.persistence = {.ready = true,
                          .schema_version = 16,
                          .target_schema_version = 16,
                          .sqlite_version = "3.53.4",
                          .instance_id = "redacted"};
  snapshot.discord = {.ready = true,
                      .command_registration =
                          sanguinius::CommandRegistrationState::synchronized,
                      .command_catalog_version = 16};
  snapshot.operations = {.status_available = true,
                         .status_age_seconds = 7,
                         .operation_result = "succeeded",
                         .backup_age_seconds = 61,
                         .backup_schema = 16,
                         .backup_result = "succeeded",
                         .state_disk_warning = false,
                         .cache_disk_warning = true,
                         .backup_disk_warning = false};
  const auto rendered = sanguinius::render_health(snapshot);
  REQUIRE(rendered.find("release=2.2.0-grevision") != std::string::npos);
  REQUIRE(rendered.find("revision=0123456789ab") != std::string::npos);
  REQUIRE(rendered.find("0123456789abcdef0123456789abcdef01234567") ==
          std::string::npos);
  REQUIRE(rendered.find("instance=") == std::string::npos);
  REQUIRE(rendered.find("redacted") == std::string::npos);
  REQUIRE(rendered.find("systemd_readiness=ready") != std::string::npos);
  REQUIRE(rendered.find("operations_status_age_seconds=7") !=
          std::string::npos);
  REQUIRE(rendered.find("backup_age_seconds=61") != std::string::npos);
  REQUIRE(rendered.find("backup_schema=16") != std::string::npos);
  REQUIRE(rendered.find("cache_disk=warning") != std::string::npos);
  REQUIRE(rendered.size() <= sanguinius::maximum_health_message_size);

  const auto unavailable = sanguinius::read_operations_health(
      "/definitely/missing/sanguinius-status.json", "/definitely/missing",
      "/definitely/missing", "/definitely/missing", 1'000);
  REQUIRE_FALSE(unavailable.status_available);
  REQUIRE_FALSE(unavailable.status_age_seconds.has_value());
}
