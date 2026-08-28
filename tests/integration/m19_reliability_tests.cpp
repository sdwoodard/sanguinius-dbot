#include "sanguinius/reliability_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

TEST_CASE("Milestone 19 reliability probes use isolated durable workflows",
          "[m19][reliability][persistence]") {
  const auto service = sanguinius::make_isolated_reliability_test_service();
  REQUIRE(service->run("text-timeout").find("SQLite") != std::string::npos);
  REQUIRE(service->run("ai-saturation").find("bounded") != std::string::npos);
  REQUIRE(service->run("discord-unknown").find("quarantined") !=
          std::string::npos);
  REQUIRE_THROWS_AS(service->run("invalid"), std::invalid_argument);
}
