#include "sanguinius/appearance_policy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace sanguinius {
namespace {

using Json = nlohmann::json;

void exact_keys(const Json &value,
                const std::set<std::string, std::less<>> &keys,
                const std::string_view where) {
  if (!value.is_object())
    throw std::runtime_error{std::string{where} + " must be an object."};
  for (const auto &[key, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (!keys.contains(key))
      throw std::runtime_error{std::string{where} + " contains unknown field " +
                               key + "."};
  }
  for (const auto &key : keys) {
    if (!value.contains(key))
      throw std::runtime_error{std::string{where} + " is missing field " + key +
                               "."};
  }
}

template <typename T>
T bounded_integer(const Json &value, const std::string_view name,
                  const T minimum, const T maximum) {
  if (!value.is_number_integer() && !value.is_number_unsigned())
    throw std::runtime_error{std::string{name} + " must be an integer."};
  const auto raw = value.get<std::int64_t>();
  if (raw < static_cast<std::int64_t>(minimum) ||
      raw > static_cast<std::int64_t>(maximum))
    throw std::runtime_error{std::string{name} + " is out of range."};
  return static_cast<T>(raw);
}

std::int64_t seconds_to_ms(const Json &value, const std::string_view name,
                           const std::int64_t maximum = 365LL * 24 * 60 * 60) {
  return bounded_integer<std::int64_t>(value, name, 1, maximum) * 1'000;
}

[[nodiscard]] std::string lowercase_ascii(const std::string_view input) {
  std::string result{input};
  std::ranges::transform(result, result.begin(), [](const unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return result;
}

[[nodiscard]] bool word_character(const unsigned char value) {
  return std::isalnum(value) != 0 || value == static_cast<unsigned char>('_');
}

[[nodiscard]] bool word_boundary_match(const std::string &text,
                                       const std::string &phrase) {
  auto position = text.find(phrase);
  while (position != std::string::npos) {
    const auto end = position + phrase.size();
    const bool left =
        position == 0 ||
        !word_character(static_cast<unsigned char>(text[position - 1]));
    const bool right = end == text.size() ||
                       !word_character(static_cast<unsigned char>(text[end]));
    if (left && right)
      return true;
    position = text.find(phrase, position + 1);
  }
  return false;
}

} // namespace

AppearancePolicy parse_appearance_policy(const std::string_view source) {
  if (source.size() > 65'536)
    throw std::runtime_error{"Appearance policy exceeds the persisted bound."};
  Json root;
  try {
    root = Json::parse(source);
  } catch (const std::exception &) {
    throw std::runtime_error{"Appearance policy is not valid JSON."};
  }
  exact_keys(root,
             {"activity", "ai", "candidate_expiry_seconds",
              "generated_preview_retention_seconds", "hypothetical_budget",
              "policy_version", "quiet_windows", "schema_version", "scoring",
              "serious_context"},
             "appearance policy");
  AppearancePolicy result;
  result.schema_version =
      bounded_integer<int>(root.at("schema_version"), "schema_version", 1, 1);
  if (!root.at("policy_version").is_string())
    throw std::runtime_error{"policy_version must be a string."};
  result.policy_version = root.at("policy_version").get<std::string>();
  if (result.policy_version.empty() || result.policy_version.size() > 80 ||
      !std::ranges::all_of(
          result.policy_version, [](const unsigned char value) {
            return std::islower(value) != 0 || std::isdigit(value) != 0 ||
                   value == '.' || value == '_' || value == '-';
          }))
    throw std::runtime_error{"policy_version must contain 1 through 80 "
                             "lowercase identifier characters."};
  const bool tarot_policy = result.policy_version == "m13-tarot-1";

  const auto &activity = root.at("activity");
  exact_keys(activity,
             {"active_humans_required", "human_messages_required",
              "maximum_rows", "maximum_total_utf8_bytes",
              "maximum_utf8_bytes_per_row", "retention_seconds",
              "window_seconds"},
             "activity");
  result.activity_window_ms = seconds_to_ms(activity.at("window_seconds"),
                                            "activity.window_seconds", 86'400);
  result.human_messages_required =
      bounded_integer<std::size_t>(activity.at("human_messages_required"),
                                   "activity.human_messages_required", 1, 24);
  result.active_humans_required =
      bounded_integer<std::size_t>(activity.at("active_humans_required"),
                                   "activity.active_humans_required", 1, 3);
  result.activity_retention_ms = seconds_to_ms(
      activity.at("retention_seconds"), "activity.retention_seconds", 86'400);
  result.activity_maximum_rows = bounded_integer<std::size_t>(
      activity.at("maximum_rows"), "activity.maximum_rows", 8, 24);
  result.activity_maximum_utf8_bytes_per_row = bounded_integer<std::size_t>(
      activity.at("maximum_utf8_bytes_per_row"),
      "activity.maximum_utf8_bytes_per_row", 32, 500);
  result.activity_maximum_total_utf8_bytes = bounded_integer<std::size_t>(
      activity.at("maximum_total_utf8_bytes"),
      "activity.maximum_total_utf8_bytes", 1'024, 12'288);
  if (result.activity_retention_ms < result.activity_window_ms ||
      result.activity_maximum_rows < result.human_messages_required ||
      result.active_humans_required > result.human_messages_required ||
      result.human_messages_required *
              result.activity_maximum_utf8_bytes_per_row >
          result.activity_maximum_total_utf8_bytes ||
      result.human_messages_required *
              result.activity_maximum_utf8_bytes_per_row >
          7'000)
    throw std::runtime_error{
        "appearance activity bounds cannot retain one qualifying candidate."};
  const auto &expiry = root.at("candidate_expiry_seconds");
  std::set<std::string, std::less<>> expiry_keys{
      "anniversary",       "chronicle_entry", "conversation", "recurrence",
      "session_completed", "session_started", "simulation",   "title_awarded"};
  if (tarot_policy)
    expiry_keys.insert("tarot_event");
  exact_keys(expiry, expiry_keys, "candidate_expiry_seconds");
  for (const auto &key : expiry_keys)
    result.candidate_expiry_ms[key] =
        seconds_to_ms(expiry.at(key), "candidate expiry");

  const auto &budget = root.at("hypothetical_budget");
  exact_keys(budget,
             {"human_messages_after_previous", "maximum", "minimum_gap_seconds",
              "same_memory_cooldown_seconds", "same_theme_cooldown_seconds",
              "window_seconds"},
             "hypothetical_budget");
  result.budget_window_ms =
      seconds_to_ms(budget.at("window_seconds"), "budget.window_seconds");
  result.budget_maximum = bounded_integer<std::size_t>(budget.at("maximum"),
                                                       "budget.maximum", 1, 20);
  result.minimum_gap_ms = seconds_to_ms(budget.at("minimum_gap_seconds"),
                                        "budget.minimum_gap_seconds");
  result.human_messages_after_previous = bounded_integer<std::size_t>(
      budget.at("human_messages_after_previous"),
      "budget.human_messages_after_previous", 0, 100);
  result.same_theme_cooldown_ms =
      seconds_to_ms(budget.at("same_theme_cooldown_seconds"),
                    "budget.same_theme_cooldown_seconds");
  result.same_memory_cooldown_ms =
      seconds_to_ms(budget.at("same_memory_cooldown_seconds"),
                    "budget.same_memory_cooldown_seconds");
  if (result.minimum_gap_ms > result.budget_window_ms)
    throw std::runtime_error{
        "appearance minimum gap exceeds its budget window."};

  const auto &scoring = root.at("scoring");
  exact_keys(scoring,
             {"alternating_turns", "alternating_window_seconds",
              "recent_speech_seconds", "stale_speech_seconds", "threshold",
              "weights"},
             "scoring");
  result.score_threshold = bounded_integer<int>(scoring.at("threshold"),
                                                "scoring.threshold", 0, 100);
  result.alternating_turns = bounded_integer<std::size_t>(
      scoring.at("alternating_turns"), "scoring.alternating_turns", 2, 24);
  if (result.alternating_turns > result.human_messages_required)
    throw std::runtime_error{
        "alternating turn count exceeds the qualifying message count."};
  result.alternating_window_ms =
      seconds_to_ms(scoring.at("alternating_window_seconds"),
                    "scoring.alternating_window_seconds");
  result.recent_speech_ms = seconds_to_ms(scoring.at("recent_speech_seconds"),
                                          "scoring.recent_speech_seconds");
  result.stale_speech_ms = seconds_to_ms(scoring.at("stale_speech_seconds"),
                                         "scoring.stale_speech_seconds");
  if (result.recent_speech_ms >= result.stale_speech_ms)
    throw std::runtime_error{
        "recent speech must be shorter than stale speech."};
  const auto &weights = scoring.at("weights");
  std::set<std::string, std::less<>> weight_keys{
      "alternating_turns",
      "chronicle_event",
      "chronicle_exact",
      "expected_anniversary",
      "expected_chronicle_entry",
      "expected_conversation",
      "expected_recurrence",
      "expected_session_completed",
      "expected_session_started",
      "expected_simulation",
      "expected_title_awarded",
      "novelty_7_to_30_days",
      "novelty_unseen",
      "participation_three_plus",
      "participation_two",
      "penalty_recent_speech",
      "penalty_repetition_14_to_30_days",
      "penalty_repetition_7_to_14_days",
      "penalty_stale_speech",
      "penalty_uncertain_generic",
      "penalty_weak_generic",
      "recurrence_multiple",
      "recurrence_one",
      "relevance_current",
      "session_event",
      "timing_four_to_seven_messages",
      "timing_idle_or_eight_messages"};
  if (tarot_policy)
    weight_keys.insert("expected_tarot_event");
  exact_keys(weights, weight_keys, "scoring.weights");
  for (const auto &key : weight_keys)
    result.score_weights[key] =
        bounded_integer<int>(weights.at(key), "scoring weight", -100, 100);
  if (!tarot_policy) {
    result.candidate_expiry_ms["tarot_event"] =
        result.candidate_expiry_ms.at("chronicle_entry");
    result.score_weights["expected_tarot_event"] =
        result.score_weights.at("expected_chronicle_entry");
  }
  for (const auto &key :
       {"penalty_recent_speech", "penalty_repetition_14_to_30_days",
        "penalty_repetition_7_to_14_days", "penalty_stale_speech",
        "penalty_uncertain_generic", "penalty_weak_generic"}) {
    if (result.score_weights.at(key) > 0)
      throw std::runtime_error{
          "appearance penalty weights must not be positive."};
  }

  const auto &ai = root.at("ai");
  exact_keys(ai,
             {"allowed_tones", "attempts", "maximum_memories",
              "maximum_output_tokens", "maximum_public_excerpts",
              "maximum_unicode_code_points", "minimum_confidence"},
             "ai");
  result.maximum_public_excerpts = bounded_integer<std::size_t>(
      ai.at("maximum_public_excerpts"), "ai.maximum_public_excerpts", 1, 8);
  result.maximum_memories = bounded_integer<std::size_t>(
      ai.at("maximum_memories"), "ai.maximum_memories", 0, 3);
  result.ai_attempts =
      bounded_integer<std::size_t>(ai.at("attempts"), "ai.attempts", 1, 1);
  result.maximum_output_tokens = bounded_integer<std::size_t>(
      ai.at("maximum_output_tokens"), "ai.maximum_output_tokens", 1, 500);
  result.maximum_unicode_code_points =
      bounded_integer<std::size_t>(ai.at("maximum_unicode_code_points"),
                                   "ai.maximum_unicode_code_points", 1, 500);
  if (result.maximum_public_excerpts > result.human_messages_required)
    throw std::runtime_error{
        "AI public excerpt count exceeds the qualifying message count."};
  if (!ai.at("minimum_confidence").is_number())
    throw std::runtime_error{"ai.minimum_confidence must be numeric."};
  result.minimum_confidence = ai.at("minimum_confidence").get<double>();
  if (!std::isfinite(result.minimum_confidence) ||
      result.minimum_confidence < 0.0 || result.minimum_confidence > 1.0)
    throw std::runtime_error{"ai.minimum_confidence is out of range."};
  if (!ai.at("allowed_tones").is_array() || ai.at("allowed_tones").empty())
    throw std::runtime_error{"ai.allowed_tones must be a nonempty array."};
  std::set<std::string, std::less<>> unique_tones;
  const std::set<std::string, std::less<>> supported_tones{
      "celebratory", "playful", "reflective", "warm", "wry"};
  for (const auto &tone : ai.at("allowed_tones")) {
    if (!tone.is_string())
      throw std::runtime_error{"ai tone must be a string."};
    const auto value = tone.get<std::string>();
    if (!supported_tones.contains(value) || !unique_tones.insert(value).second)
      throw std::runtime_error{"ai tone is invalid or duplicated."};
    result.allowed_tones.push_back(value);
  }

  result.generated_preview_retention_ms =
      seconds_to_ms(root.at("generated_preview_retention_seconds"),
                    "generated_preview_retention_seconds");
  if (!root.at("quiet_windows").is_array())
    throw std::runtime_error{"quiet_windows must be an array."};
  if (root.at("quiet_windows").size() > 32)
    throw std::runtime_error{"quiet_windows exceeds its collection bound."};
  for (const auto &window : root.at("quiet_windows")) {
    exact_keys(window, {"end_minute", "start_minute", "weekday"},
               "quiet window");
    result.quiet_windows.push_back(
        {bounded_integer<int>(window.at("weekday"), "quiet weekday", 0, 6),
         bounded_integer<int>(window.at("start_minute"), "quiet start", 0,
                              1439),
         bounded_integer<int>(window.at("end_minute"), "quiet end", 1, 1440)});
    if (result.quiet_windows.back().start_minute >=
        result.quiet_windows.back().end_minute)
      throw std::runtime_error{"quiet window must not wrap midnight."};
  }
  for (std::size_t left = 0; left < result.quiet_windows.size(); ++left) {
    for (std::size_t right = left + 1; right < result.quiet_windows.size();
         ++right) {
      const auto &a = result.quiet_windows[left];
      const auto &b = result.quiet_windows[right];
      if (a.weekday == b.weekday && a.start_minute < b.end_minute &&
          b.start_minute < a.end_minute)
        throw std::runtime_error{
            "quiet windows on the same weekday must not overlap."};
    }
  }

  const auto &serious = root.at("serious_context");
  const std::set<std::string, std::less<>> serious_keys{
      "abuse_conflict",           "christianity",
      "credentials_security_pii", "crisis_self_harm_emergency",
      "death_serious_health",     "private_medical_employment_legal_financial"};
  exact_keys(serious, serious_keys, "serious_context");
  for (const auto &category : serious_keys) {
    const auto &terms = serious.at(category);
    if (!terms.is_array() || terms.empty() || terms.size() > 64)
      throw std::runtime_error{
          "serious-context category must be a nonempty array."};
    std::set<std::string, std::less<>> unique;
    for (const auto &term : terms) {
      if (!term.is_string())
        throw std::runtime_error{"serious-context term must be a string."};
      const auto normalized = lowercase_ascii(term.get<std::string>());
      if (normalized.empty() || normalized.size() > 80 ||
          !unique.insert(normalized).second)
        throw std::runtime_error{
            "serious-context term is invalid or duplicated."};
      result.serious_terms[category].push_back(normalized);
    }
  }
  result.canonical_json = root.dump();
  return result;
}

std::optional<std::string>
detect_serious_context(const AppearancePolicy &policy,
                       const std::string_view text) {
  if (!valid_utf8(text))
    return std::string{"invalid_utf8"};
  const auto normalized = lowercase_ascii(text);
  for (const auto &[category, terms] : policy.serious_terms) {
    if (std::ranges::any_of(terms, [&](const auto &term) {
          return word_boundary_match(normalized, term);
        }))
      return category;
  }
  return std::nullopt;
}

bool valid_utf8(const std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t continuation{};
    std::uint32_t value{};
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0U) {
      continuation = 1;
      value = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      continuation = 2;
      value = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      continuation = 3;
      value = lead & 0x07U;
    } else
      return false;
    if (index + continuation >= text.size())
      return false;
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0U) != 0x80U)
        return false;
      value = (value << 6U) | (next & 0x3FU);
    }
    if ((continuation == 1 && value < 0x80U) ||
        (continuation == 2 && value < 0x800U) ||
        (continuation == 3 && value < 0x10000U) || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU))
      return false;
    index += continuation + 1;
  }
  return true;
}

std::size_t unicode_code_points(const std::string_view text) noexcept {
  if (!valid_utf8(text))
    return std::numeric_limits<std::size_t>::max();
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](const char value) {
        return (static_cast<unsigned char>(value) & 0xC0U) != 0x80U;
      }));
}

} // namespace sanguinius
