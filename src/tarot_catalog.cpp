#include "sanguinius/tarot_catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sanguinius {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 22> expected_names{
    "The Golden Throne",   "The Angel at the Gate", "The Red Grail",
    "The Broken Wing",     "The Last Standard",     "The Veiled Blade",
    "The Unbroken Wall",   "The Twin Moons",        "The Forge Eternal",
    "The Pilgrim Star",    "The Crownless King",    "The Falling Comet",
    "The Ashen Field",     "The Herald's Horn",     "The Silver Chain",
    "The Open Hand",       "The Final Hour",        "The Lantern in the Void",
    "The Cast Die",        "The Returning Dawn",    "The Sealed Gate",
    "The Living Chronicle"};

constexpr std::array<std::string_view, 22> expected_meanings{
    "Duty, endurance, and the burden of command",
    "Grace, protection, and costly hope",
    "Passion, temptation, and the price of desire",
    "Setback, humility, and recovery",
    "Unity, leadership, and a cause held together",
    "Hidden risk, subtle action, and guarded intent",
    "Resolve, defense, and necessary boundaries",
    "Ambiguity, divided loyalties, and incomplete knowledge",
    "Discipline, making, and transformation through effort",
    "Seeking, patience, and the long road toward meaning",
    "Influence without title and earned authority",
    "Sudden change, danger, and brief opportunity",
    "Consequence, rest, and lessons after conflict",
    "News, recognition, and a call to action",
    "Obligation, trust, and interdependence",
    "Mercy, generosity, and aid freely offered",
    "Deadlines, inevitability, and decisive action",
    "Clarity, truth, and guidance amid uncertainty",
    "Chance, commitment, and accepting risk",
    "Renewal, resilience, and hope after loss",
    "A threshold, a choice, and withheld knowledge",
    "Memory, legacy, and meaning created together"};

constexpr std::array<std::string_view, 22> conventional_names{
    "the fool",       "the magician",   "the high priestess", "the empress",
    "the emperor",    "the hierophant", "the lovers",         "the chariot",
    "strength",       "the hermit",     "wheel of fortune",   "justice",
    "the hanged man", "death",          "temperance",         "the devil",
    "the tower",      "the star",       "the moon",           "the sun",
    "judgement",      "the world"};

constexpr std::string_view expected_deck_checksum{"fnv1a64:86946b9b879fa5ba"};
constexpr std::string_view expected_house_checksum{"fnv1a64:d96dd3b79480f5e9"};

[[nodiscard]] std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

void exact_keys(const Json &value,
                const std::initializer_list<std::string_view> keys,
                const std::string_view subject) {
  if (!value.is_object())
    throw std::invalid_argument{std::string{subject} + " must be an object."};
  std::set<std::string> expected;
  for (const auto key : keys)
    expected.emplace(key);
  std::set<std::string> actual;
  for (const auto &[key, ignored] : value.items()) {
    static_cast<void>(ignored);
    actual.insert(key);
  }
  if (actual != expected)
    throw std::invalid_argument{std::string{subject} +
                                " contains missing or unknown fields."};
}

[[nodiscard]] std::string bounded_string(const Json &value,
                                         const std::string_view field,
                                         const std::size_t minimum,
                                         const std::size_t maximum) {
  if (!value.contains(field) || !value.at(field).is_string())
    throw std::invalid_argument{std::string{field} + " must be a string."};
  auto result = value.at(field).get<std::string>();
  if (result.size() < minimum || result.size() > maximum)
    throw std::invalid_argument{std::string{field} + " is outside bounds."};
  return result;
}

[[nodiscard]] std::string read_catalog(const std::filesystem::path &path) {
  std::ifstream stream{path};
  if (!stream)
    throw std::runtime_error{"Unable to read Tarot catalog: " + path.string()};
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] HouseResolutionAuthority authority(const std::string_view value) {
  if (value == "draw")
    return HouseResolutionAuthority::draw;
  if (value == "public_draw")
    return HouseResolutionAuthority::public_draw;
  if (value == "owner")
    return HouseResolutionAuthority::owner;
  throw std::invalid_argument{"Unknown House resolution authority."};
}

} // namespace

std::string stable_catalog_checksum(const std::string_view canonical) {
  std::uint64_t checksum{14695981039346656037ULL};
  for (const auto character : canonical) {
    checksum ^= static_cast<unsigned char>(character);
    checksum *= 1099511628211ULL;
  }
  std::ostringstream result;
  result << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
         << checksum;
  return result.str();
}

TarotDeckCatalog parse_tarot_deck_catalog(const std::string_view source) {
  const auto root = Json::parse(source);
  exact_keys(root, {"schema", "version", "cards"}, "Tarot deck catalog");
  if (root.at("schema") != 1 ||
      root.at("version") != emperor_tarot_catalog_version)
    throw std::invalid_argument{"Unsupported Emperor's Tarot catalog."};
  if (!root.at("cards").is_array() || root.at("cards").size() != 22)
    throw std::invalid_argument{
        "The initial Tarot deck must contain 22 cards."};

  TarotDeckCatalog result;
  result.version = root.at("version").get<std::string>();
  std::set<std::string> slugs;
  std::set<std::string> tags;
  for (std::size_t index{}; index < root.at("cards").size(); ++index) {
    const auto &item = root.at("cards").at(index);
    exact_keys(item,
               {"ordinal", "slug", "name", "meaning", "theme_tag",
                "safety_prompt", "flavor_variants"},
               "Tarot card");
    TarotCardDefinition card;
    card.ordinal = item.at("ordinal").get<std::int64_t>();
    card.slug = bounded_string(item, "slug", 3, 48);
    card.name = bounded_string(item, "name", 3, 80);
    card.meaning = bounded_string(item, "meaning", 10, 160);
    card.theme_tag = bounded_string(item, "theme_tag", 3, 32);
    card.safety_prompt = bounded_string(item, "safety_prompt", 10, 240);
    if (card.ordinal != static_cast<std::int64_t>(index) ||
        card.name != expected_names.at(index) ||
        card.meaning != expected_meanings.at(index))
      throw std::invalid_argument{"The Emperor's Tarot v1 identity changed."};
    if (!slugs.insert(card.slug).second || !tags.insert(card.theme_tag).second)
      throw std::invalid_argument{"Tarot card slugs and tags must be unique."};
    const auto normalized = lower(card.name);
    if (std::ranges::find(conventional_names, normalized) !=
        conventional_names.end())
      throw std::invalid_argument{"Conventional Tarot names are forbidden."};
    const auto &variants = item.at("flavor_variants");
    if (!variants.is_array() || variants.empty() || variants.size() > 4)
      throw std::invalid_argument{"A card needs one to four flavor variants."};
    for (const auto &variant : variants) {
      if (!variant.is_string())
        throw std::invalid_argument{"Tarot flavor variants must be text."};
      auto text = variant.get<std::string>();
      if (text.size() < 10 || text.size() > 240)
        throw std::invalid_argument{"Tarot flavor is outside bounds."};
      card.flavor_variants.push_back(std::move(text));
    }
    result.cards.push_back(std::move(card));
  }
  result.canonical_json = root.dump();
  result.checksum = stable_catalog_checksum(result.canonical_json);
  if (result.checksum != expected_deck_checksum)
    throw std::invalid_argument{
        "The Emperor's Tarot v1 canonical catalog changed."};
  return result;
}

TarotHouseCatalog parse_tarot_house_catalog(const std::string_view source,
                                            const std::int64_t profit_cap) {
  if (profit_cap < 1 || profit_cap > 1'000)
    throw std::invalid_argument{"House profit cap is outside safe bounds."};
  const auto root = Json::parse(source);
  exact_keys(root, {"schema", "version", "templates"}, "House catalog");
  if (root.at("schema") != 1 ||
      root.at("version") != tarot_house_catalog_version)
    throw std::invalid_argument{"Unsupported House catalog."};
  if (!root.at("templates").is_array() || root.at("templates").size() != 4)
    throw std::invalid_argument{"House v1 must contain four templates."};
  TarotHouseCatalog result;
  result.version = root.at("version").get<std::string>();
  std::set<std::string> template_slugs;
  for (const auto &item : root.at("templates")) {
    exact_keys(item,
               {"slug", "name", "proposition", "choices", "allowed_stakes",
                "eligibility_balance_below", "outcome_window_ms",
                "terminal_cooldown_ms", "recovery_reward", "authority",
                "scheduled", "recovery"},
               "House template");
    HouseTemplateDefinition entry;
    entry.slug = bounded_string(item, "slug", 3, 48);
    entry.name = bounded_string(item, "name", 3, 80);
    entry.proposition = bounded_string(item, "proposition", 10, 300);
    if (!template_slugs.insert(entry.slug).second)
      throw std::invalid_argument{"Duplicate House template slug."};
    entry.eligibility_balance_below =
        item.at("eligibility_balance_below").get<std::int64_t>();
    entry.outcome_window_ms = item.at("outcome_window_ms").get<std::int64_t>();
    entry.terminal_cooldown_ms =
        item.at("terminal_cooldown_ms").get<std::int64_t>();
    entry.recovery_reward = item.at("recovery_reward").get<std::int64_t>();
    entry.authority = authority(item.at("authority").get<std::string>());
    entry.scheduled = item.at("scheduled").get<bool>();
    entry.recovery = item.at("recovery").get<bool>();
    if (entry.outcome_window_ms <= 0 ||
        entry.outcome_window_ms > 8 * 86'400'000LL ||
        entry.terminal_cooldown_ms < 0 ||
        entry.terminal_cooldown_ms > 31 * 86'400'000LL)
      throw std::invalid_argument{"House deadline is outside safe bounds."};
    const auto &stakes = item.at("allowed_stakes");
    if (!stakes.is_array() || stakes.empty() || stakes.size() > 8)
      throw std::invalid_argument{"House stakes are invalid."};
    for (const auto &stake_json : stakes) {
      const auto stake = stake_json.get<std::int64_t>();
      if (stake < 0 || stake > 100 ||
          (!entry.allowed_stakes.empty() &&
           stake <= entry.allowed_stakes.back()))
        throw std::invalid_argument{
            "House stakes must be ordered and bounded."};
      entry.allowed_stakes.push_back(stake);
    }
    const auto &choices = item.at("choices");
    if (!choices.is_array() || choices.empty() || choices.size() > 4)
      throw std::invalid_argument{"House choices are invalid."};
    std::set<std::string> choice_slugs;
    for (const auto &choice_json : choices) {
      exact_keys(choice_json,
                 {"slug", "label", "profit_numerator", "profit_denominator"},
                 "House choice");
      HouseChoiceDefinition choice;
      choice.slug = bounded_string(choice_json, "slug", 1, 32);
      choice.label = bounded_string(choice_json, "label", 1, 80);
      choice.profit_numerator =
          choice_json.at("profit_numerator").get<std::int64_t>();
      choice.profit_denominator =
          choice_json.at("profit_denominator").get<std::int64_t>();
      if (!choice_slugs.insert(choice.slug).second ||
          choice.profit_numerator < 0 || choice.profit_numerator > 20 ||
          choice.profit_denominator < 1 || choice.profit_denominator > 20)
        throw std::invalid_argument{"House odds are invalid."};
      for (const auto stake : entry.allowed_stakes) {
        if ((stake * choice.profit_numerator) % choice.profit_denominator !=
                0 ||
            (stake * choice.profit_numerator) / choice.profit_denominator >
                profit_cap)
          throw std::invalid_argument{"House odds violate payout policy."};
      }
      entry.choices.push_back(std::move(choice));
    }
    if (entry.recovery != (entry.recovery_reward > 0) ||
        (entry.recovery &&
         entry.allowed_stakes != std::vector<std::int64_t>{0}))
      throw std::invalid_argument{"Recovery template collateral is invalid."};
    result.templates.push_back(std::move(entry));
  }
  const std::array expected{"returning-dawn", "heralds-call", "final-hour",
                            "last-standard"};
  for (std::size_t index{}; index < expected.size(); ++index)
    if (result.templates.at(index).slug != expected.at(index))
      throw std::invalid_argument{"House v1 template identity changed."};
  result.canonical_json = root.dump();
  result.checksum = stable_catalog_checksum(result.canonical_json);
  if (result.checksum != expected_house_checksum)
    throw std::invalid_argument{"The House v1 canonical catalog changed."};
  return result;
}

TarotDeckCatalog load_tarot_deck_catalog(const std::filesystem::path &path) {
  return parse_tarot_deck_catalog(read_catalog(path));
}

TarotHouseCatalog load_tarot_house_catalog(const std::filesystem::path &path,
                                           const std::int64_t profit_cap) {
  return parse_tarot_house_catalog(read_catalog(path), profit_cap);
}

const HouseTemplateDefinition &house_template(const TarotHouseCatalog &catalog,
                                              const std::string_view slug) {
  const auto found = std::ranges::find(catalog.templates, slug,
                                       &HouseTemplateDefinition::slug);
  if (found == catalog.templates.end())
    throw std::invalid_argument{"Unknown House template."};
  return *found;
}

} // namespace sanguinius
