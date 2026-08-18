#include "sanguinius/message_handler.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("prefix commands preserve their parser behavior", "[command]") {
  using enum sanguinius::Command;

  REQUIRE(sanguinius::parse_command("!help", "!") == help);
  REQUIRE(sanguinius::parse_command("!HELP", "!") == help);
  REQUIRE(sanguinius::parse_command("!repo extra", "!") == repo);
  REQUIRE(sanguinius::parse_command("?repo", "!") == none);
  REQUIRE(sanguinius::parse_command("!join", "!") == none);
  REQUIRE(sanguinius::parse_command("!gpt question", "!") == none);
  REQUIRE(sanguinius::parse_command("ordinary message", "!") == none);
}
