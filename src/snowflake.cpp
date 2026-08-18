#include "sanguinius/snowflake.hpp"

#include <charconv>
#include <stdexcept>
#include <system_error>

namespace sanguinius {

DiscordSnowflake DiscordSnowflake::parse(const std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    throw std::invalid_argument{"Invalid Discord snowflake."};
  }

  std::uint64_t parsed{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::invalid_argument{"Invalid Discord snowflake."};
  }
  return DiscordSnowflake{parsed};
}

std::string DiscordSnowflake::str() const { return std::to_string(value_); }

} // namespace sanguinius
