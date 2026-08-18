#include "sanguinius/server_scope_policy.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("server scope policy authorizes configured members and owner",
          "[scope]") {
  const sanguinius::ServerScopePolicy policy{{10, 20, 30}};

  REQUIRE(policy.authorize({10, 20, 40}, sanguinius::RequiredRole::member)
              .allowed());
  REQUIRE(policy.authorize({10, 20, 30}, sanguinius::RequiredRole::owner)
              .allowed());
}

TEST_CASE("server scope policy rejects guild channel and owner mismatches",
          "[scope]") {
  const sanguinius::ServerScopePolicy policy{{10, 20, 30}};

  REQUIRE(policy.authorize({11, 20, 30}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::wrong_guild);
  REQUIRE(policy.authorize({10, 21, 30}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::wrong_channel);
  REQUIRE(policy.authorize({10, 20, 31}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::owner_required);
  REQUIRE(policy.authorize({0, 20, 30}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::wrong_guild);
  REQUIRE(policy.authorize({10, 0, 30}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::wrong_channel);
  REQUIRE(policy.authorize({10, 20, 0}, sanguinius::RequiredRole::owner)
              .rejection == sanguinius::ScopeRejection::owner_required);
}

TEST_CASE("server scope policy requires complete configured IDs", "[scope]") {
  REQUIRE_THROWS_AS(sanguinius::ServerScopePolicy({0, 20, 30}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(sanguinius::ServerScopePolicy({10, 0, 30}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(sanguinius::ServerScopePolicy({10, 20, 0}),
                    std::invalid_argument);
}
