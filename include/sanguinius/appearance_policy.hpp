#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

struct AppearanceQuietWindow {
  int weekday{};
  int start_minute{};
  int end_minute{};
};

struct AppearancePolicy {
  int schema_version{1};
  std::string policy_version;
  std::string canonical_json;
  std::int64_t activity_window_ms{};
  std::size_t human_messages_required{};
  std::size_t active_humans_required{};
  std::int64_t activity_retention_ms{};
  std::size_t activity_maximum_rows{};
  std::size_t activity_maximum_utf8_bytes_per_row{};
  std::size_t activity_maximum_total_utf8_bytes{};
  std::map<std::string, std::int64_t, std::less<>> candidate_expiry_ms;
  std::int64_t budget_window_ms{};
  std::size_t budget_maximum{};
  std::int64_t minimum_gap_ms{};
  std::size_t human_messages_after_previous{};
  std::int64_t same_theme_cooldown_ms{};
  std::int64_t same_memory_cooldown_ms{};
  int score_threshold{};
  std::size_t alternating_turns{};
  std::int64_t alternating_window_ms{};
  std::int64_t recent_speech_ms{};
  std::int64_t stale_speech_ms{};
  std::map<std::string, int, std::less<>> score_weights;
  std::size_t maximum_public_excerpts{};
  std::size_t maximum_memories{};
  std::size_t ai_attempts{};
  std::size_t maximum_output_tokens{};
  std::size_t maximum_unicode_code_points{};
  double minimum_confidence{};
  std::vector<std::string> allowed_tones;
  std::int64_t generated_preview_retention_ms{};
  std::vector<AppearanceQuietWindow> quiet_windows;
  std::map<std::string, std::vector<std::string>, std::less<>> serious_terms;
};

[[nodiscard]] AppearancePolicy parse_appearance_policy(std::string_view json);
[[nodiscard]] std::optional<std::string>
detect_serious_context(const AppearancePolicy &policy, std::string_view text);
[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;
[[nodiscard]] std::size_t unicode_code_points(std::string_view text) noexcept;

} // namespace sanguinius
