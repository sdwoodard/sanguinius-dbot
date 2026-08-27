#pragma once

#include "sanguinius/discord_types.hpp"
#include "sanguinius/feature_config.hpp"

#include <cstdint>
#include <string_view>

namespace sanguinius::presentation {

inline constexpr std::uint32_t crimson = 0x8B0000U;
inline constexpr std::uint32_t neutral = 0x667085U;
inline constexpr std::uint32_t success = 0x2E7D32U;
inline constexpr std::uint32_t warning = 0xB26A00U;
inline constexpr std::uint32_t failure = 0xB42318U;
inline constexpr std::string_view disabled_previous_custom_id{
    "sanguinius:pagination:previous-disabled"};
inline constexpr std::string_view disabled_next_custom_id{
    "sanguinius:pagination:next-disabled"};

enum class ErrorKind {
  wrong_scope,
  forbidden,
  malformed,
  stale,
  expired,
  busy,
  feature_disabled,
  provider_degraded,
  duplicate,
};

[[nodiscard]] InteractionMessage help(std::string_view topic,
                                      const FeatureConfiguration &features,
                                      std::int64_t timestamp_ms);
[[nodiscard]] InteractionMessage repository(std::int64_t timestamp_ms);
[[nodiscard]] InteractionMessage error(ErrorKind kind,
                                       std::string_view retry_command = {});
[[nodiscard]] ButtonStyle action_button_style(std::string_view label) noexcept;
void validate(const InteractionMessage &message);

} // namespace sanguinius::presentation
