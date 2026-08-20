#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/appearances.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

[[nodiscard]] std::string read(const std::filesystem::path &path) {
  std::ifstream stream{path};
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::filesystem::path root() {
  return std::filesystem::path{__FILE__}
      .parent_path()
      .parent_path()
      .parent_path();
}

[[nodiscard]] sanguinius::AppearancePolicy policy() {
  return sanguinius::parse_appearance_policy(
      read(root() / "config/appearance-policy-v1.json"));
}

} // namespace

TEST_CASE("appearance policy v1 loads exact bounded defaults",
          "[appearance][policy]") {
  const auto value = policy();
  REQUIRE(value.schema_version == 1);
  REQUIRE(value.policy_version == "m9-initial-1");
  REQUIRE(value.activity_window_ms == 600'000);
  REQUIRE(value.human_messages_required == 8);
  REQUIRE(value.active_humans_required == 2);
  REQUIRE(value.activity_maximum_rows == 24);
  REQUIRE(value.activity_maximum_utf8_bytes_per_row == 500);
  REQUIRE(value.activity_maximum_total_utf8_bytes == 12'288);
  REQUIRE(value.score_threshold == 60);
  REQUIRE(value.score_weights.at("relevance_current") == 20);
  REQUIRE(value.score_weights.at("penalty_weak_generic") == -30);
  REQUIRE(value.ai_attempts == 1);
  REQUIRE(value.maximum_output_tokens == 500);
  REQUIRE(value.maximum_unicode_code_points == 500);
  REQUIRE(value.minimum_confidence == 0.8);
  REQUIRE(value.quiet_windows.empty());
  REQUIRE(nlohmann::json::parse(value.canonical_json).at("policy_version") ==
          "m9-initial-1");
}

TEST_CASE("appearance policy rejects unknown fields versions and ranges",
          "[appearance][policy]") {
  auto source =
      nlohmann::json::parse(read(root() / "config/appearance-policy-v1.json"));
  source["unknown"] = true;
  REQUIRE_THROWS(sanguinius::parse_appearance_policy(source.dump()));
  source.erase("unknown");
  source["schema_version"] = 2;
  REQUIRE_THROWS(sanguinius::parse_appearance_policy(source.dump()));
  source["schema_version"] = 1;
  source["ai"]["attempts"] = 2;
  REQUIRE_THROWS(sanguinius::parse_appearance_policy(source.dump()));
  source["ai"]["attempts"] = 1;
  source["ai"]["allowed_tones"] = {"invented"};
  REQUIRE_THROWS(sanguinius::parse_appearance_policy(source.dump()));
}

TEST_CASE("appearance policy rejects unsafe cross-field bounds",
          "[appearance][policy][bounds]") {
  const auto original =
      nlohmann::json::parse(read(root() / "config/appearance-policy-v1.json"));
  const auto rejects = [&](const auto &mutate) {
    auto changed = original;
    mutate(changed);
    REQUIRE_THROWS(sanguinius::parse_appearance_policy(changed.dump()));
  };
  rejects([](auto &value) { value["policy_version"] = "bad/version"; });
  rejects([](auto &value) { value["activity"]["maximum_rows"] = 7; });
  rejects([](auto &value) {
    value["activity"]["maximum_total_utf8_bytes"] = 3'000;
  });
  rejects(
      [](auto &value) { value["activity"]["human_messages_required"] = 24; });
  rejects([](auto &value) { value["activity"]["retention_seconds"] = 300; });
  rejects([](auto &value) { value["scoring"]["alternating_turns"] = 9; });
  rejects([](auto &value) {
    value["hypothetical_budget"]["minimum_gap_seconds"] = 90'000;
  });
  rejects([](auto &value) {
    value["quiet_windows"] = {
        {{"weekday", 1}, {"start_minute", 100}, {"end_minute", 200}},
        {{"weekday", 1}, {"start_minute", 150}, {"end_minute", 250}}};
  });
}

TEST_CASE("configured appearance quiet windows use the configured timezone",
          "[appearance][policy][quiet]") {
  auto source =
      nlohmann::json::parse(read(root() / "config/appearance-policy-v1.json"));
  source["quiet_windows"] = {
      {{"weekday", 1}, {"start_minute", 420}, {"end_minute", 421}}};
  const auto value = sanguinius::parse_appearance_policy(source.dump());
  // 2024-01-01 12:00:00 UTC is Monday 07:00 in America/New_York.
  REQUIRE(sanguinius::appearance_quiet_window_active(value, 1'704'110'400'000,
                                                     "America/New_York"));
  REQUIRE_FALSE(sanguinius::appearance_quiet_window_active(
      value, 1'704'110'460'000, "America/New_York"));
}

TEST_CASE("serious-context matching is category specific and word bounded",
          "[appearance][safety]") {
  const auto value = policy();
  REQUIRE(sanguinius::detect_serious_context(value, "Please pray at church") ==
          "christianity");
  REQUIRE(sanguinius::detect_serious_context(value, "The hospital called") ==
          "death_serious_health");
  REQUIRE_FALSE(sanguinius::detect_serious_context(value, "goodness gracious"));
  REQUIRE(sanguinius::detect_serious_context(value, "my password leaked") ==
          "credentials_security_pii");
}

TEST_CASE("named appearance fixtures produce deterministic gates and scores",
          "[appearance][fixtures]") {
  const auto value = policy();
  const auto cases = nlohmann::json::parse(
      read(root() / "tests/fixtures/appearances/cases.json"));
  for (const auto &fixture : cases) {
    CAPTURE(fixture.at("name").get<std::string>());
    sanguinius::AppearanceCandidate candidate{};
    candidate.candidate_id = "00000000-0000-4000-8000-000000000901";
    candidate.created_at_ms = 1'000;
    candidate.expires_at_ms = fixture.value("stale", false) ? 2'000 : 20'000;
    candidate.type = fixture.value("type", std::string{}) == "anniversary"
                         ? sanguinius::AppearanceCandidateType::anniversary
                         : sanguinius::AppearanceCandidateType::conversation;
    for (int actor = 0; actor < fixture.at("actors").get<int>(); ++actor)
      candidate.actors.emplace_back(static_cast<std::uint64_t>(actor + 1));
    candidate.owner_simulation = fixture.value("owner_simulation", false);
    candidate.alternating_turns = fixture.value("alternating", false);
    candidate.recurrence_matches = fixture.value("recurrence", std::size_t{});
    candidate.chronicle_specificity = fixture.value("specificity", 0);
    candidate.bot_last_meaningful_speaker = fixture.value("bot_last", false);
    candidate.configured_quiet = fixture.value("configured_quiet", false);
    candidate.manual_quiet = fixture.value("manual_quiet", false);
    candidate.source_enabled = fixture.value("source_enabled", true);
    candidate.theme_available = fixture.value("theme_available", true);
    candidate.consented = fixture.value("consented", true);
    if (fixture.contains("serious"))
      candidate.deterministic_serious_category =
          fixture.at("serious").get<std::string>();
    const auto evaluated = sanguinius::evaluate_appearance(
        value, sanguinius::AppearanceMode::dry_run, candidate, 3'000);
    REQUIRE(evaluated.reason == fixture.at("expected").get<std::string>());
    if (evaluated.eligible_for_model)
      REQUIRE(evaluated.score >= 60);
    if (fixture.at("name") == "lively_game_night_banter") {
      REQUIRE(evaluated.score == 75);
      REQUIRE(evaluated.score_components ==
              std::vector<sanguinius::AppearanceScoreComponent>{
                  {"relevance", 20},
                  {"alternating_turns", 5},
                  {"novelty", 15},
                  {"participation", 10},
                  {"recurrence", 10},
                  {"timing", 10},
                  {"expected_interaction_value", 5}});
    }
  }
}

TEST_CASE("appearance timing scoring honors recent and stale speech edges",
          "[appearance][scoring][timing]") {
  const auto value = policy();
  const auto score_points = [](const sanguinius::AppearanceEvaluation &result,
                               const std::string_view name) {
    const auto found =
        std::ranges::find(result.score_components, name,
                          &sanguinius::AppearanceScoreComponent::name);
    return found == result.score_components.end() ? 0 : found->points;
  };
  const auto evaluate = [&](const std::int64_t speech_age,
                            const std::size_t human_messages) {
    sanguinius::AppearanceCandidate candidate{};
    candidate.created_at_ms = 1'000;
    candidate.expires_at_ms = 20'000;
    candidate.type = sanguinius::AppearanceCandidateType::session_started;
    candidate.actors = {1, 2};
    candidate.chronicle_specificity = value.score_weights.at("session_event");
    candidate.bot_speech_age_ms = speech_age;
    candidate.human_messages_since_bot = human_messages;
    return sanguinius::evaluate_appearance(
        value, sanguinius::AppearanceMode::dry_run, candidate, 3'000);
  };

  const auto recent = evaluate(value.recent_speech_ms - 1, 3);
  REQUIRE(score_points(recent, "timing") == 0);
  REQUIRE(score_points(recent, "recent_sanguinius_speech") == -20);

  const auto recent_boundary = evaluate(value.recent_speech_ms, 3);
  REQUIRE(score_points(recent_boundary, "timing") == 0);
  REQUIRE(score_points(recent_boundary, "recent_sanguinius_speech") == -10);

  const auto stale = evaluate(value.stale_speech_ms - 1, 4);
  REQUIRE(score_points(stale, "timing") == 5);
  REQUIRE(score_points(stale, "recent_sanguinius_speech") == -10);

  const auto idle = evaluate(value.stale_speech_ms, 2);
  REQUIRE(score_points(idle, "timing") == 10);
  REQUIRE(score_points(idle, "recent_sanguinius_speech") == 0);
  REQUIRE(idle.eligible_for_model);

  const auto eight_messages = evaluate(value.recent_speech_ms - 1, 8);
  REQUIRE(score_points(eight_messages, "timing") == 10);
  REQUIRE(score_points(eight_messages, "recent_sanguinius_speech") == -20);
}

TEST_CASE("appearance model result is strict bounded and mention safe",
          "[appearance][model]") {
  const auto value = policy();
  const std::vector<std::string> memories{
      "00000000-0000-4000-8000-000000000111"};
  const auto valid =
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"A fine evening for shared victories.","tone":"warm","memory_ids_used":[],"confidence":0.91})";
  REQUIRE(sanguinius::parse_appearance_model_result(value, valid, memories)
              .should_speak);
  const auto serious =
      R"({"serious_context":true,"serious_categories":["christianity"],"should_speak":false,"text":"","tone":"reflective","memory_ids_used":[],"confidence":0.91})";
  REQUIRE(sanguinius::parse_appearance_model_result(value, serious, memories)
              .serious_context);

  const std::vector<std::string> invalid{
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"@everyone behold","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"https://example.test","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"See <#1234567890>","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Our PRIVATE BALANCE is hidden","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"HTTPS://example.test","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"ftp://example.test/archive","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Visit //example.test/path","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Visit example.test/path","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Visit 192.0.2.1/admin","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Visit 192.0.2.1:8080/admin","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Visit [2001:db8::1]/admin","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"Join discord.gg/example","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"hello","tone":"unknown","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"hello","tone":"warm","memory_ids_used":[],"confidence":0.79})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"hello","tone":"warm","memory_ids_used":["00000000-0000-4000-8000-000000000999"],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":false,"text":"not empty","tone":"warm","memory_ids_used":[],"confidence":0.91})",
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":"hello","tone":"warm","memory_ids_used":[],"confidence":0.91,"extra":true})",
      R"({"serious_context":true,"serious_categories":["christianity","christianity","christianity","christianity","christianity","christianity","christianity"],"should_speak":false,"text":"","tone":"reflective","memory_ids_used":[],"confidence":0.91})",
      "not-json"};
  for (const auto &response : invalid) {
    CAPTURE(response);
    REQUIRE_THROWS(
        sanguinius::parse_appearance_model_result(value, response, memories));
  }

  auto excessive = nlohmann::json::parse(valid);
  excessive["text"] = std::string(501, 'x');
  REQUIRE_THROWS(sanguinius::parse_appearance_model_result(
      value, excessive.dump(), memories));

  auto unicode_blank = nlohmann::json::parse(valid);
  unicode_blank["text"] = std::string{"\xC2\xA0"};
  REQUIRE_THROWS(sanguinius::parse_appearance_model_result(
      value, unicode_blank.dump(), memories));

  std::string invalid_utf8 =
      R"({"serious_context":false,"serious_categories":[],"should_speak":true,"text":")";
  invalid_utf8.append("\xC0\xAF", 2);
  invalid_utf8 += R"(","tone":"warm","memory_ids_used":[],"confidence":0.91})";
  REQUIRE_THROWS(
      sanguinius::parse_appearance_model_result(value, invalid_utf8, memories));
}

TEST_CASE("appearance AI context contains only bounded supplied memories",
          "[appearance][model][privacy]") {
  const auto value = policy();
  sanguinius::AppearanceCandidate candidate{};
  candidate.type = sanguinius::AppearanceCandidateType::recurrence;
  candidate.excerpts = {"one", "two"};
  candidate.supplied_memory_ids = {"00000000-0000-4000-8000-000000000111"};
  candidate.memory_context = {{"00000000-0000-4000-8000-000000000111", 2,
                               "A confirmed ordinary shared memory."}};
  const auto request =
      sanguinius::appearance_ai_request(value, candidate, "PERSONA_SENTINEL");
  REQUIRE(request.instructions.find("PERSONA_SENTINEL") != std::string::npos);
  REQUIRE(request.conversation.size() == 1);
  const auto context =
      nlohmann::json::parse(request.conversation.front().content);
  REQUIRE(context.at("public_excerpts").size() == 2);
  REQUIRE(context.at("available_memories").size() == 1);
  REQUIRE(context.at("available_memories")[0].at("memory_id") ==
          candidate.supplied_memory_ids.front());
  REQUIRE(context.at("available_memories")[0].at("text") ==
          candidate.memory_context.front().text);
  REQUIRE(request.max_output_tokens == 500);
  REQUIRE(request.json_schema.has_value());
  REQUIRE(request.json_schema->strict);

  auto limited_source =
      nlohmann::json::parse(read(root() / "config/appearance-policy-v1.json"));
  limited_source["ai"]["maximum_public_excerpts"] = 1;
  const auto limited_policy =
      sanguinius::parse_appearance_policy(limited_source.dump());
  const auto limited_request =
      sanguinius::appearance_ai_request(limited_policy, candidate);
  const auto limited_context =
      nlohmann::json::parse(limited_request.conversation.front().content);
  REQUIRE(limited_context.at("public_excerpts") ==
          nlohmann::json::array({"two"}));
}

TEST_CASE("appearance response schema follows versioned AI policy bounds",
          "[appearance][model][policy]") {
  auto source =
      nlohmann::json::parse(read(root() / "config/appearance-policy-v1.json"));
  source["policy_version"] = "m9-schema-bounds";
  source["ai"]["maximum_memories"] = 1;
  source["ai"]["maximum_unicode_code_points"] = 64;
  source["ai"]["allowed_tones"] = {"warm", "reflective"};
  const auto bounded = sanguinius::parse_appearance_policy(source.dump());
  const auto request = sanguinius::appearance_ai_request(
      bounded, sanguinius::AppearanceCandidate{});
  REQUIRE(request.json_schema.has_value());
  const auto schema =
      nlohmann::json::parse(request.json_schema->schema).at("properties");
  REQUIRE(schema.at("text").at("maxLength") == 64);
  REQUIRE(schema.at("memory_ids_used").at("maxItems") == 1);
  REQUIRE(schema.at("tone").at("enum") ==
          nlohmann::json::array({"warm", "reflective"}));
}

TEST_CASE("appearance UTF-8 validation counts Unicode scalars",
          "[appearance]") {
  REQUIRE(sanguinius::valid_utf8("Sanguinius \xF0\x9F\xA9\xB8"));
  REQUIRE(sanguinius::unicode_code_points("a\xF0\x9F\xA9\xB8") == 2);
  REQUIRE_FALSE(sanguinius::valid_utf8(std::string{"\xC0\xAF", 2}));
}
