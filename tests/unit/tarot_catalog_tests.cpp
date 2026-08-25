#include "sanguinius/tarot_catalog.hpp"
#include "sanguinius/tarot_house.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

[[nodiscard]] std::string config_file(const std::string &name) {
  const auto path = std::filesystem::path{__FILE__}
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "config" / name;
  std::ifstream stream{path};
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Emperor's Tarot v1 is the exact curated original 22-card deck",
          "[tarot][catalog]") {
  const auto catalog = sanguinius::parse_tarot_deck_catalog(
      config_file("emperor-tarot-v1.json"));
  REQUIRE(catalog.version == "emperor-tarot-v1");
  REQUIRE(catalog.cards.size() == 22);
  REQUIRE(catalog.cards.front().name == "The Golden Throne");
  REQUIRE(catalog.cards.back().name == "The Living Chronicle");
  REQUIRE(catalog.checksum.starts_with("fnv1a64:"));
  REQUIRE(catalog.checksum == "fnv1a64:86946b9b879fa5ba");
  for (std::size_t index{}; index < catalog.cards.size(); ++index) {
    REQUIRE(catalog.cards[index].ordinal == static_cast<std::int64_t>(index));
    REQUIRE_FALSE(catalog.cards[index].safety_prompt.empty());
    REQUIRE(catalog.cards[index].flavor_variants.size() == 2);
  }
}

TEST_CASE("Tarot catalog rejects conventional names and reversal fields",
          "[tarot][catalog][privacy]") {
  auto source = config_file("emperor-tarot-v1.json");
  const auto original = source.find("The Golden Throne");
  REQUIRE(original != std::string::npos);
  source.replace(original, std::string{"The Golden Throne"}.size(), "The Fool");
  REQUIRE_THROWS_AS(sanguinius::parse_tarot_deck_catalog(source),
                    std::invalid_argument);

  source = config_file("emperor-tarot-v1.json");
  const auto card = source.find("\"ordinal\":0");
  REQUIRE(card != std::string::npos);
  source.insert(card, "\"reversed\":true,");
  REQUIRE_THROWS_AS(sanguinius::parse_tarot_deck_catalog(source),
                    std::invalid_argument);

  source = config_file("emperor-tarot-v1.json");
  const auto flavor = source.find("The Golden Throne asks");
  REQUIRE(flavor != std::string::npos);
  source.replace(flavor, std::string{"The Golden Throne asks"}.size(),
                 "The Golden Throne demands");
  REQUIRE_THROWS_AS(sanguinius::parse_tarot_deck_catalog(source),
                    std::invalid_argument);
}

TEST_CASE("House v1 payout matrix is integral and capped",
          "[tarot][house][catalog]") {
  const auto catalog = sanguinius::parse_tarot_house_catalog(
      config_file("tarot-house-v1.json"), 20);
  REQUIRE(catalog.templates.size() == 4);
  REQUIRE(catalog.checksum == "fnv1a64:d96dd3b79480f5e9");
  REQUIRE(sanguinius::house_template(catalog, "returning-dawn").recovery);
  REQUIRE(sanguinius::house_template(catalog, "last-standard").scheduled);
  REQUIRE_THROWS_AS(sanguinius::parse_tarot_house_catalog(
                        config_file("tarot-house-v1.json"), 19),
                    std::invalid_argument);
  auto changed = config_file("tarot-house-v1.json");
  const auto proposition = changed.find("Complete a new Emperor's Tarot draw");
  REQUIRE(proposition != std::string::npos);
  changed.replace(proposition,
                  std::string{"Complete a new Emperor's Tarot draw"}.size(),
                  "Complete one new Emperor's Tarot draw");
  REQUIRE_THROWS_AS(sanguinius::parse_tarot_house_catalog(changed, 20),
                    std::invalid_argument);
}

TEST_CASE("Friday House schedule preserves New York wall time across DST",
          "[tarot][house][schedule][dst]") {
  using namespace std::chrono;
  const auto millis = [](const sys_time<milliseconds> value) {
    return value.time_since_epoch().count();
  };
  const auto before_spring = sys_days{year{2026} / March / 6} + hours{23};
  const auto after_spring = sys_days{year{2026} / March / 13} + hours{22};
  REQUIRE(sanguinius::next_house_weekly_offer_ms(
              millis(time_point_cast<milliseconds>(before_spring)),
              "America/New_York") ==
          millis(time_point_cast<milliseconds>(after_spring)));
  REQUIRE(after_spring - before_spring == hours{167});

  const auto before_fall = sys_days{year{2026} / October / 30} + hours{22};
  const auto after_fall = sys_days{year{2026} / November / 6} + hours{23};
  REQUIRE(sanguinius::next_house_weekly_offer_ms(
              millis(time_point_cast<milliseconds>(before_fall)),
              "America/New_York") ==
          millis(time_point_cast<milliseconds>(after_fall)));
  REQUIRE(after_fall - before_fall == hours{169});
}

TEST_CASE("Last Standard civil deadlines remain Monday 0015 across DST",
          "[tarot][house][schedule][deadline][dst]") {
  using namespace std::chrono;
  const auto millis = [](const sys_time<milliseconds> value) {
    return value.time_since_epoch().count();
  };

  const auto spring_slot =
      time_point_cast<milliseconds>(sys_days{year{2026} / March / 6} +
                                    hours{23});
  const auto spring =
      sanguinius::house_weekly_boundaries_ms(millis(spring_slot));
  REQUIRE(spring.closes_at_ms ==
          millis(time_point_cast<milliseconds>(
              sys_days{year{2026} / March / 8} + hours{5})));
  REQUIRE(spring.resolution_due_at_ms ==
          millis(time_point_cast<milliseconds>(
              sys_days{year{2026} / March / 9} + hours{4} + minutes{15})));
  REQUIRE(milliseconds{spring.resolution_due_at_ms - millis(spring_slot)} ==
          hours{53} + minutes{15});

  const auto fall_slot =
      time_point_cast<milliseconds>(sys_days{year{2026} / October / 30} +
                                    hours{22});
  const auto fall =
      sanguinius::house_weekly_boundaries_ms(millis(fall_slot));
  REQUIRE(fall.closes_at_ms ==
          millis(time_point_cast<milliseconds>(
              sys_days{year{2026} / November / 1} + hours{4})));
  REQUIRE(fall.resolution_due_at_ms ==
          millis(time_point_cast<milliseconds>(
              sys_days{year{2026} / November / 2} + hours{5} + minutes{15})));
  REQUIRE(milliseconds{fall.resolution_due_at_ms - millis(fall_slot)} ==
          hours{55} + minutes{15});
}

TEST_CASE("House component identifiers are opaque and versioned",
          "[tarot][house][component]") {
  const auto token = std::string{sanguinius::tarot_house_component_prefix} +
                     "10000000-0000-4000-8000-000000000001";
  REQUIRE(sanguinius::parse_tarot_house_component(token) ==
          "10000000-0000-4000-8000-000000000001");
  REQUIRE_FALSE(sanguinius::parse_tarot_house_component("sgh:1:not-a-token"));
  REQUIRE_FALSE(sanguinius::parse_tarot_house_component(
      "sgt:1:10000000-0000-4000-8000-000000000001"));
}
