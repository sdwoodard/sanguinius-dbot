#include "sanguinius/ai_responder.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("bot mentions preserve their parser behavior", "[mention]") {
  constexpr sanguinius::DiscordId bot_id = 123;

  REQUIRE(sanguinius::prompt_after_bot_mention("<@123> hello", bot_id) ==
          "hello");
  REQUIRE(sanguinius::prompt_after_bot_mention("  <@!123>: hello", bot_id) ==
          "hello");
  REQUIRE(sanguinius::prompt_after_bot_mention("<@123>", bot_id) == "");
  REQUIRE_FALSE(sanguinius::prompt_after_bot_mention("hello <@123>", bot_id));
  REQUIRE_FALSE(sanguinius::prompt_after_bot_mention("<@456> hello", bot_id));
}
