#include "sanguinius/presentation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

[[nodiscard]] std::string
flatten(const sanguinius::InteractionMessage &message) {
  REQUIRE(message.embed.has_value());
  std::string value = message.embed->title + message.embed->description;
  for (const auto &field : message.embed->fields)
    value += field.name + field.value;
  value += message.embed->footer;
  return value;
}

} // namespace

TEST_CASE("member help is catalog-derived and excludes owner controls",
          "[command][help]") {
  const auto message = sanguinius::presentation::help(
      "all",
      sanguinius::FeatureConfiguration{.chronicle_enabled = true,
                                       .tarot_enabled = true,
                                       .vox_enabled = true},
      1'000);
  const auto text = flatten(message);

  REQUIRE(text.find("/help") != std::string::npos);
  REQUIRE(text.find("/repo") != std::string::npos);
  REQUIRE(text.find("/chronicle") != std::string::npos);
  REQUIRE(text.find("/tarot") != std::string::npos);
  REQUIRE(text.find("/vox") != std::string::npos);
  REQUIRE(text.find("/sang-admin") == std::string::npos);
  REQUIRE(text.find("debug") == std::string::npos);
}

TEST_CASE("help topic restricts the displayed family", "[command][help]") {
  const auto text = flatten(sanguinius::presentation::help(
      "tarot",
      sanguinius::FeatureConfiguration{.chronicle_enabled = true,
                                       .tarot_enabled = true,
                                       .vox_enabled = true},
      1'000));
  REQUIRE(text.find("/tarot") != std::string::npos);
  REQUIRE(text.find("/chronicle") == std::string::npos);
  REQUIRE(text.find("/vox") == std::string::npos);
}

TEST_CASE("help topics remain useful across feature flag combinations",
          "[command][help][features]") {
  for (const bool chronicle : {false, true}) {
    for (const bool tarot : {false, true}) {
      for (const bool vox : {false, true}) {
        const sanguinius::FeatureConfiguration features{.chronicle_enabled =
                                                            chronicle,
                                                        .tarot_enabled = tarot,
                                                        .vox_enabled = vox};
        for (const std::string_view topic :
             {"all", "sanguinius", "chronicle", "tarot", "vox"}) {
          const auto text =
              flatten(sanguinius::presentation::help(topic, features, 1'000));
          REQUIRE_FALSE(text.empty());
          REQUIRE(text.find("/sang-admin") == std::string::npos);
          const bool enabled = topic == "all" || topic == "sanguinius" ||
                               (topic == "chronicle" && chronicle) ||
                               (topic == "tarot" && tarot) ||
                               (topic == "vox" && vox);
          if (!enabled)
            REQUIRE(text.find("currently unavailable") != std::string::npos);
        }
      }
    }
  }
  REQUIRE_THROWS_AS(sanguinius::presentation::help("debug", {}, 1'000),
                    std::invalid_argument);
}
