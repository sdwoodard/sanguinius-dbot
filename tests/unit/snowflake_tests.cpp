#include "sanguinius/snowflake.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

TEST_CASE("Discord snowflakes round trip the unsigned 64-bit range",
          "[snowflake]") {
  const auto ordinary =
      sanguinius::DiscordSnowflake::parse("123456789012345678");
  REQUIRE(ordinary.value() == 123456789012345678ULL);
  REQUIRE(ordinary.str() == "123456789012345678");

  const auto greater_than_signed =
      sanguinius::DiscordSnowflake::parse("9223372036854775808");
  REQUIRE(greater_than_signed.value() == (std::uint64_t{1} << 63U));
  REQUIRE(greater_than_signed.str() == "9223372036854775808");

  const auto maximum =
      sanguinius::DiscordSnowflake::parse("18446744073709551615");
  REQUIRE(maximum.value() == std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum.str() == "18446744073709551615");
}

TEST_CASE("Discord snowflakes reject noncanonical and overflowing input",
          "[snowflake]") {
  constexpr std::string_view invalid[]{
      "",    "+1",      "-1",
      " 1",  "1 ",      "01",
      "1.0", "1suffix", "18446744073709551616",
  };
  for (const auto value : invalid) {
    REQUIRE_THROWS_AS(sanguinius::DiscordSnowflake::parse(value),
                      std::invalid_argument);
  }

  const auto zero = sanguinius::DiscordSnowflake::parse("0");
  REQUIRE_FALSE(zero.is_set());
  REQUIRE(zero.str() == "0");
}
