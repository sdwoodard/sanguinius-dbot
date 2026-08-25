#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view emperor_tarot_catalog_version{
    "emperor-tarot-v1"};
inline constexpr std::string_view tarot_house_catalog_version{
    "tarot-house-v1"};

struct TarotCardDefinition {
  std::int64_t ordinal{};
  std::string slug;
  std::string name;
  std::string meaning;
  std::string theme_tag;
  std::string safety_prompt;
  std::vector<std::string> flavor_variants;

  [[nodiscard]] bool operator==(const TarotCardDefinition &) const = default;
};

struct TarotDeckCatalog {
  std::string version;
  std::string canonical_json;
  std::string checksum;
  std::vector<TarotCardDefinition> cards;
};

enum class HouseResolutionAuthority { draw, public_draw, owner };

struct HouseChoiceDefinition {
  std::string slug;
  std::string label;
  std::int64_t profit_numerator{};
  std::int64_t profit_denominator{};
};

struct HouseTemplateDefinition {
  std::string slug;
  std::string name;
  std::string proposition;
  std::vector<HouseChoiceDefinition> choices;
  std::vector<std::int64_t> allowed_stakes;
  std::int64_t eligibility_balance_below{};
  std::int64_t outcome_window_ms{};
  std::int64_t terminal_cooldown_ms{};
  std::int64_t recovery_reward{};
  HouseResolutionAuthority authority{HouseResolutionAuthority::draw};
  bool scheduled{};
  bool recovery{};
};

struct TarotHouseCatalog {
  std::string version;
  std::string canonical_json;
  std::string checksum;
  std::vector<HouseTemplateDefinition> templates;
};

[[nodiscard]] TarotDeckCatalog parse_tarot_deck_catalog(std::string_view json);
[[nodiscard]] TarotHouseCatalog
parse_tarot_house_catalog(std::string_view json, std::int64_t profit_cap);
[[nodiscard]] TarotDeckCatalog
load_tarot_deck_catalog(const std::filesystem::path &path);
[[nodiscard]] TarotHouseCatalog
load_tarot_house_catalog(const std::filesystem::path &path,
                         std::int64_t profit_cap);

[[nodiscard]] std::string stable_catalog_checksum(std::string_view canonical);
[[nodiscard]] const HouseTemplateDefinition &
house_template(const TarotHouseCatalog &catalog, std::string_view slug);

} // namespace sanguinius
