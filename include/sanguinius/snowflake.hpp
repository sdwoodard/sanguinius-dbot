#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sanguinius {

class DiscordSnowflake {
public:
  constexpr DiscordSnowflake() noexcept = default;
  constexpr DiscordSnowflake(const std::uint64_t value) noexcept
      : value_{value} {}

  [[nodiscard]] static DiscordSnowflake parse(std::string_view text);
  [[nodiscard]] std::string str() const;
  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return value_;
  }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

  auto operator<=>(const DiscordSnowflake &) const = default;

private:
  std::uint64_t value_{};
};

using DiscordId = DiscordSnowflake;

} // namespace sanguinius

template <> struct std::hash<sanguinius::DiscordSnowflake> {
  [[nodiscard]] std::size_t
  operator()(const sanguinius::DiscordSnowflake value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
