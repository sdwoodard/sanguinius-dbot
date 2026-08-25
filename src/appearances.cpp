#include "sanguinius/appearances.hpp"

#include "sanguinius/durable_work.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {
using Json = nlohmann::json;

[[nodiscard]] int expected_value(const AppearancePolicy &policy,
                                 const AppearanceCandidateType type) {
  switch (type) {
  case AppearanceCandidateType::conversation:
    return policy.score_weights.at("expected_conversation");
  case AppearanceCandidateType::recurrence:
    return policy.score_weights.at("expected_recurrence");
  case AppearanceCandidateType::anniversary:
    return policy.score_weights.at("expected_anniversary");
  case AppearanceCandidateType::title_awarded:
    return policy.score_weights.at("expected_title_awarded");
  case AppearanceCandidateType::chronicle_entry:
    return policy.score_weights.at("expected_chronicle_entry");
  case AppearanceCandidateType::tarot_event:
    return policy.score_weights.at("expected_tarot_event");
  case AppearanceCandidateType::session_started:
    return policy.score_weights.at("expected_session_started");
  case AppearanceCandidateType::session_completed:
    return policy.score_weights.at("expected_session_completed");
  case AppearanceCandidateType::simulation:
    return policy.score_weights.at("expected_simulation");
  }
  return 0;
}

void score(AppearanceEvaluation &result, std::string name, const int points) {
  result.score_components.push_back({std::move(name), points});
}

void gate(AppearanceEvaluation &result, std::string name, const bool passed) {
  result.gates.push_back({std::move(name), passed});
  if (!passed && result.reason.empty())
    result.reason = result.gates.back().name;
}

[[nodiscard]] bool domain_character(const unsigned char value) {
  return std::isalnum(value) != 0 || value == '-';
}

[[nodiscard]] bool contains_bare_domain(const std::string_view text) {
  for (std::size_t dot = 1; dot + 1 < text.size(); ++dot) {
    if (text[dot] != '.' ||
        !domain_character(static_cast<unsigned char>(text[dot - 1])) ||
        !domain_character(static_cast<unsigned char>(text[dot + 1])))
      continue;
    auto begin = dot;
    while (begin > 0 &&
           domain_character(static_cast<unsigned char>(text[begin - 1])))
      --begin;
    auto end = dot + 1;
    while (end < text.size() &&
           (domain_character(static_cast<unsigned char>(text[end])) ||
            text[end] == '.'))
      ++end;
    const auto host = text.substr(begin, end - begin);
    std::size_t label_begin{};
    std::size_t labels{};
    bool valid = true;
    std::string_view final_label;
    while (label_begin < host.size()) {
      const auto label_end = host.find('.', label_begin);
      const auto label =
          host.substr(label_begin, label_end == std::string_view::npos
                                       ? host.size() - label_begin
                                       : label_end - label_begin);
      if (label.empty() ||
          std::isalnum(static_cast<unsigned char>(label.front())) == 0 ||
          std::isalnum(static_cast<unsigned char>(label.back())) == 0) {
        valid = false;
        break;
      }
      final_label = label;
      ++labels;
      if (label_end == std::string_view::npos)
        break;
      label_begin = label_end + 1;
    }
    const bool alphabetic_tld =
        std::ranges::any_of(final_label, [](const unsigned char value) {
          return std::isalpha(value) != 0;
        });
    if (valid && labels >= 2 && final_label.size() >= 2 && alphabetic_tld)
      return true;
    dot = end - 1;
  }
  return false;
}

[[nodiscard]] bool numeric_address(std::string_view value) {
  while (!value.empty() && value.front() == '.')
    value.remove_prefix(1);
  while (!value.empty() && value.back() == '.')
    value.remove_suffix(1);
  if (value.empty())
    return false;
  const std::string address{value};
  in_addr ipv4{};
  in6_addr ipv6{};
  return inet_pton(AF_INET, address.c_str(), &ipv4) == 1 ||
         inet_pton(AF_INET6, address.c_str(), &ipv6) == 1;
}

[[nodiscard]] bool contains_numeric_address(const std::string_view text) {
  std::size_t index{};
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (std::isxdigit(byte) == 0) {
      ++index;
      continue;
    }
    auto end = index + 1;
    while (end < text.size()) {
      const auto current = static_cast<unsigned char>(text[end]);
      if (std::isxdigit(current) == 0 && current != ':' && current != '.')
        break;
      ++end;
    }
    const auto token = text.substr(index, end - index);
    if ((token.find('.') != std::string_view::npos ||
         token.find(':') != std::string_view::npos) &&
        numeric_address(token))
      return true;
    const auto port_separator = token.rfind(':');
    if (port_separator != std::string_view::npos &&
        token.substr(0, port_separator).find('.') != std::string_view::npos) {
      const auto port = token.substr(port_separator + 1);
      const bool decimal_port =
          !port.empty() && std::ranges::all_of(port, [](const char value) {
            return value >= '0' && value <= '9';
          });
      if (decimal_port && numeric_address(token.substr(0, port_separator)))
        return true;
    }
    index = end;
  }
  return false;
}

[[nodiscard]] bool unicode_whitespace(const std::uint32_t value) noexcept {
  return (value >= 0x0009U && value <= 0x000DU) || value == 0x0020U ||
         value == 0x0085U || value == 0x00A0U || value == 0x1680U ||
         value == 0x180EU || (value >= 0x2000U && value <= 0x200DU) ||
         value == 0x2028U || value == 0x2029U || value == 0x202FU ||
         value == 0x205FU || value == 0x2060U || value == 0x3000U ||
         value == 0xFEFFU;
}

[[nodiscard]] bool unicode_blank(const std::string_view text) noexcept {
  std::size_t index{};
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::uint32_t value{};
    std::size_t width{};
    if (lead <= 0x7FU) {
      value = lead;
      width = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      value = lead & 0x1FU;
      width = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      value = lead & 0x0FU;
      width = 3;
    } else {
      value = lead & 0x07U;
      width = 4;
    }
    for (std::size_t offset = 1; offset < width; ++offset)
      value = (value << 6U) |
              (static_cast<unsigned char>(text[index + offset]) & 0x3FU);
    if (!unicode_whitespace(value))
      return false;
    index += width;
  }
  return true;
}

[[nodiscard]] bool unsafe_generated_text(const std::string_view text) {
  if (text.find_first_of("\r\n\v\f") != std::string_view::npos ||
      text.find("\xC2\x85") != std::string_view::npos ||
      text.find("\xE2\x80\xA8") != std::string_view::npos ||
      text.find("\xE2\x80\xA9") != std::string_view::npos)
    return true;
  std::string normalized{text};
  std::ranges::transform(normalized, normalized.begin(),
                         [](const unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
  constexpr std::string_view forbidden[]{"<@",
                                         "<#",
                                         "@everyone",
                                         "@here",
                                         "mailto:",
                                         "file:",
                                         "data:",
                                         "javascript:",
                                         "www.",
                                         "discord.gg",
                                         "discord.com/invite",
                                         "discordapp.com/invite",
                                         "memory_id",
                                         "candidate_id",
                                         "appearance_callback",
                                         "hidden relationship",
                                         "relationship score",
                                         "private balance",
                                         "sealed wager",
                                         "pending notice",
                                         "internal scoring",
                                         "system prompt"};
  if (std::ranges::any_of(forbidden, [&](const auto phrase) {
        return normalized.find(phrase) != std::string::npos;
      }))
    return true;
  if (normalized.find("://") != std::string::npos ||
      normalized.find("//") != std::string::npos ||
      contains_bare_domain(normalized) || contains_numeric_address(normalized))
    return true;
  static const std::string uuid_pattern{"xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"};
  static_cast<void>(uuid_pattern);
  for (std::size_t i = 0; i + 36 <= text.size(); ++i) {
    if (text[i + 8] == '-' && text[i + 13] == '-' && text[i + 18] == '-' &&
        text[i + 23] == '-')
      return true;
  }
  return false;
}

[[nodiscard]] std::string bounded_response(std::string value,
                                           const std::size_t maximum = 1'900) {
  if (value.size() <= maximum)
    return value;
  constexpr std::string_view suffix{"\n[output truncated]"};
  value.resize(maximum - suffix.size());
  while (!value.empty() && !valid_utf8(value))
    value.pop_back();
  value += suffix;
  return value;
}

} // namespace

std::string_view
appearance_candidate_type_name(const AppearanceCandidateType type) noexcept {
  switch (type) {
  case AppearanceCandidateType::conversation:
    return "conversation";
  case AppearanceCandidateType::recurrence:
    return "recurrence";
  case AppearanceCandidateType::chronicle_entry:
    return "chronicle_entry";
  case AppearanceCandidateType::session_started:
    return "session_started";
  case AppearanceCandidateType::session_completed:
    return "session_completed";
  case AppearanceCandidateType::title_awarded:
    return "title_awarded";
  case AppearanceCandidateType::anniversary:
    return "anniversary";
  case AppearanceCandidateType::tarot_event:
    return "tarot_event";
  case AppearanceCandidateType::simulation:
    return "simulation";
  }
  return "unknown";
}

std::optional<AppearanceCandidateType>
parse_appearance_candidate_type(const std::string_view value) noexcept {
  if (value == "conversation")
    return AppearanceCandidateType::conversation;
  if (value == "recurrence")
    return AppearanceCandidateType::recurrence;
  if (value == "chronicle_entry")
    return AppearanceCandidateType::chronicle_entry;
  if (value == "session_started")
    return AppearanceCandidateType::session_started;
  if (value == "session_completed")
    return AppearanceCandidateType::session_completed;
  if (value == "title_awarded")
    return AppearanceCandidateType::title_awarded;
  if (value == "anniversary")
    return AppearanceCandidateType::anniversary;
  if (value == "tarot_event")
    return AppearanceCandidateType::tarot_event;
  if (value == "simulation")
    return AppearanceCandidateType::simulation;
  return std::nullopt;
}

std::string_view appearance_feedback_action_name(
    const AppearanceFeedbackAction action) noexcept {
  switch (action) {
  case AppearanceFeedbackAction::more:
    return "more";
  case AppearanceFeedbackAction::less:
    return "less";
  case AppearanceFeedbackAction::not_relevant:
    return "not_relevant";
  case AppearanceFeedbackAction::quiet_tonight:
    return "quiet_tonight";
  }
  return "unknown";
}

std::optional<AppearanceFeedbackAction>
parse_appearance_feedback_action(const std::string_view value) noexcept {
  if (value == "more")
    return AppearanceFeedbackAction::more;
  if (value == "less")
    return AppearanceFeedbackAction::less;
  if (value == "not_relevant")
    return AppearanceFeedbackAction::not_relevant;
  if (value == "quiet_tonight")
    return AppearanceFeedbackAction::quiet_tonight;
  return std::nullopt;
}

AppearanceEvaluation evaluate_appearance(const AppearancePolicy &policy,
                                         const AppearanceMode mode,
                                         const AppearanceCandidate &candidate,
                                         const std::int64_t now_ms) {
  AppearanceEvaluation result;
  gate(result, "source_enabled", candidate.source_enabled);
  gate(result, "scope", candidate.correct_scope);
  gate(result, "appearance_mode", mode != AppearanceMode::off);
  gate(result, "mode_epoch", candidate.mode_epoch_valid);
  gate(result, "candidate_fresh",
       now_ms >= candidate.created_at_ms && now_ms < candidate.expires_at_ms);
  gate(result, "active_humans",
       candidate.owner_simulation ||
           candidate.actors.size() >= policy.active_humans_required);
  gate(result, "participant_quiet", !candidate.manual_quiet);
  gate(result, "configured_quiet_window", !candidate.configured_quiet);
  gate(result, "global_kill_switch", !candidate.globally_disabled);
  gate(result, "global_quiet", !candidate.global_quiet);
  gate(result, "bot_last_meaningful_speaker",
       !candidate.bot_last_meaningful_speaker);
  gate(result, "operational", candidate.operational && !candidate.degraded);
  gate(result, "exact_duplicate", !candidate.exact_duplicate);
  gate(result, "daily_budget", candidate.budget_available);
  gate(result, "minimum_gap", candidate.gap_available);
  gate(result, "human_messages_after_previous",
       candidate.messages_after_previous);
  gate(result, "theme_cooldown", candidate.theme_available);
  gate(result, "memory_cooldown", candidate.memory_available);
  gate(result, "serious_context",
       !candidate.deterministic_serious_category.has_value());
  gate(result, "callback_consent", candidate.consented);
  gate(result, "visibility", candidate.visible);

  score(result, "relevance", policy.score_weights.at("relevance_current"));
  if (candidate.alternating_turns)
    score(result, "alternating_turns",
          policy.score_weights.at("alternating_turns"));
  if (candidate.chronicle_specificity != 0)
    score(result, "chronicle_specificity", candidate.chronicle_specificity);
  int novelty{};
  if (!candidate.novelty_age_ms ||
      *candidate.novelty_age_ms > 30LL * 24 * 60 * 60 * 1'000)
    novelty = policy.score_weights.at("novelty_unseen");
  else if (*candidate.novelty_age_ms >= 7LL * 24 * 60 * 60 * 1'000)
    novelty = policy.score_weights.at("novelty_7_to_30_days");
  score(result, "novelty", novelty);
  score(result, "participation",
        candidate.actors.size() >= 3
            ? policy.score_weights.at("participation_three_plus")
            : (candidate.actors.size() >= 2
                   ? policy.score_weights.at("participation_two")
                   : 0));
  score(result, "recurrence",
        candidate.recurrence_matches > 1
            ? policy.score_weights.at("recurrence_multiple")
            : (candidate.recurrence_matches == 1
                   ? policy.score_weights.at("recurrence_one")
                   : 0));
  const int timing =
      (!candidate.bot_speech_age_ms ||
       *candidate.bot_speech_age_ms >= policy.stale_speech_ms ||
       candidate.human_messages_since_bot >= 8)
          ? policy.score_weights.at("timing_idle_or_eight_messages")
          : (candidate.human_messages_since_bot >= 4
                 ? policy.score_weights.at("timing_four_to_seven_messages")
                 : 0);
  score(result, "timing", timing);
  score(result, "expected_interaction_value",
        expected_value(policy, candidate.type));
  if (candidate.bot_speech_age_ms &&
      *candidate.bot_speech_age_ms < policy.recent_speech_ms)
    score(result, "recent_sanguinius_speech",
          policy.score_weights.at("penalty_recent_speech"));
  else if (candidate.bot_speech_age_ms &&
           *candidate.bot_speech_age_ms < policy.stale_speech_ms)
    score(result, "recent_sanguinius_speech",
          policy.score_weights.at("penalty_stale_speech"));
  if (candidate.repetition_age_ms &&
      *candidate.repetition_age_ms >= 7LL * 24 * 60 * 60 * 1'000 &&
      *candidate.repetition_age_ms < 14LL * 24 * 60 * 60 * 1'000)
    score(result, "repetition",
          policy.score_weights.at("penalty_repetition_7_to_14_days"));
  else if (candidate.repetition_age_ms &&
           *candidate.repetition_age_ms >= 14LL * 24 * 60 * 60 * 1'000 &&
           *candidate.repetition_age_ms < 30LL * 24 * 60 * 60 * 1'000)
    score(result, "repetition",
          policy.score_weights.at("penalty_repetition_14_to_30_days"));
  if (candidate.uncertainty_penalty != 0)
    score(result, "uncertainty", candidate.uncertainty_penalty);
  int total{};
  for (const auto &component : result.score_components)
    total += component.points;
  result.score = std::clamp(total, 0, 100);
  if (result.reason.empty() && result.score < policy.score_threshold)
    result.reason = "score_below_threshold";
  result.eligible_for_model = result.reason.empty();
  if (result.eligible_for_model)
    result.reason = "model_pending";
  return result;
}

AppearanceModelResult parse_appearance_model_result(
    const AppearancePolicy &policy, const std::string_view source,
    const std::vector<std::string> &supplied_memory_ids) {
  Json value;
  try {
    value = Json::parse(source);
  } catch (...) {
    throw std::runtime_error{"model_invalid_json"};
  }
  const std::set<std::string, std::less<>> keys{"confidence",
                                                "memory_ids_used",
                                                "serious_categories",
                                                "serious_context",
                                                "should_speak",
                                                "text",
                                                "tone"};
  if (!value.is_object() || value.size() != keys.size())
    throw std::runtime_error{"model_invalid_schema"};
  for (const auto &[key, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (!keys.contains(key))
      throw std::runtime_error{"model_invalid_schema"};
  }
  for (const auto &key : keys)
    if (!value.contains(key))
      throw std::runtime_error{"model_invalid_schema"};
  if (!value.at("serious_context").is_boolean() ||
      !value.at("should_speak").is_boolean() || !value.at("text").is_string() ||
      !value.at("tone").is_string() || !value.at("confidence").is_number() ||
      !value.at("serious_categories").is_array() ||
      !value.at("memory_ids_used").is_array())
    throw std::runtime_error{"model_invalid_schema"};
  AppearanceModelResult result{
      .serious_context = value.at("serious_context").get<bool>(),
      .serious_categories =
          value.at("serious_categories").get<std::vector<std::string>>(),
      .should_speak = value.at("should_speak").get<bool>(),
      .text = value.at("text").get<std::string>(),
      .tone = value.at("tone").get<std::string>(),
      .memory_ids_used =
          value.at("memory_ids_used").get<std::vector<std::string>>(),
      .confidence = value.at("confidence").get<double>()};
  constexpr std::size_t maximum_serious_categories{6};
  if (result.serious_categories.size() > maximum_serious_categories)
    throw std::runtime_error{"model_invalid_schema"};
  const std::set<std::string, std::less<>> serious_categories{
      "abuse_conflict",           "christianity",
      "credentials_security_pii", "crisis_self_harm_emergency",
      "death_serious_health",     "private_medical_employment_legal_financial"};
  if (std::ranges::any_of(result.serious_categories, [&](const auto &category) {
        return !serious_categories.contains(category);
      }))
    throw std::runtime_error{"model_unknown_serious_category"};
  if (result.serious_context != !result.serious_categories.empty())
    throw std::runtime_error{"model_serious_inconsistent"};
  if (std::ranges::find(policy.allowed_tones, result.tone) ==
      policy.allowed_tones.end())
    throw std::runtime_error{"model_unknown_tone"};
  if (!std::isfinite(result.confidence) || result.confidence < 0.0 ||
      result.confidence > 1.0)
    throw std::runtime_error{"model_invalid_confidence"};
  if (result.confidence < policy.minimum_confidence)
    throw std::runtime_error{"model_low_confidence"};
  if (!valid_utf8(result.text) ||
      unicode_code_points(result.text) > policy.maximum_unicode_code_points)
    throw std::runtime_error{"model_unsafe_text"};
  if (detect_serious_context(policy, result.text))
    throw std::runtime_error{"model_unsafe_text"};
  const bool blank = unicode_blank(result.text);
  if ((result.should_speak && (result.text.empty() || blank)) ||
      (!result.should_speak && !result.text.empty()))
    throw std::runtime_error{"model_text_inconsistent"};
  if (unsafe_generated_text(result.text))
    throw std::runtime_error{"model_unsafe_text"};
  std::set<std::string, std::less<>> seen;
  for (const auto &memory : result.memory_ids_used) {
    if (std::ranges::find(supplied_memory_ids, memory) ==
            supplied_memory_ids.end() ||
        !seen.insert(memory).second)
      throw std::runtime_error{"model_hallucinated_memory"};
  }
  return result;
}

bool validate_appearance_model_result(
    const AppearancePolicy &policy, const AppearanceModelResult &result,
    const std::vector<std::string> &supplied_memory_ids) noexcept {
  try {
    const auto encoded = Json{{"serious_context", result.serious_context},
                              {"serious_categories",
                               result.serious_categories},
                              {"should_speak", result.should_speak},
                              {"text", result.text},
                              {"tone", result.tone},
                              {"memory_ids_used", result.memory_ids_used},
                              {"confidence", result.confidence}}
                             .dump();
    static_cast<void>(
        parse_appearance_model_result(policy, encoded, supplied_memory_ids));
    return true;
  } catch (...) {
    return false;
  }
}

AiRequest appearance_ai_request(const AppearancePolicy &policy,
                                const AppearanceCandidate &candidate,
                                const std::string_view persona) {
  Json excerpts = Json::array();
  const auto first =
      candidate.excerpts.size() > policy.maximum_public_excerpts
          ? candidate.excerpts.size() - policy.maximum_public_excerpts
          : 0;
  for (std::size_t index = first; index < candidate.excerpts.size(); ++index)
    excerpts.push_back(candidate.excerpts[index]);
  Json context{
      {"candidate_type", appearance_candidate_type_name(candidate.type)},
      {"public_excerpts", std::move(excerpts)},
      {"approved_source_context", candidate.source_context},
      {"available_memories", Json::array()}};
  for (const auto &memory : candidate.memory_context) {
    context["available_memories"].push_back(
        {{"memory_id", memory.memory_id}, {"text", memory.text}});
  }
  const Json schema{
      {"type", "object"},
      {"additionalProperties", false},
      {"required",
       {"serious_context", "serious_categories", "should_speak", "text", "tone",
        "memory_ids_used", "confidence"}},
      {"properties",
       {{"serious_context", {{"type", "boolean"}}},
        {"serious_categories",
         {{"type", "array"},
          {"maxItems", 6},
          {"items",
           {{"type", "string"},
            {"enum",
             {"crisis_self_harm_emergency", "death_serious_health",
              "abuse_conflict", "private_medical_employment_legal_financial",
              "credentials_security_pii", "christianity"}}}}}},
        {"should_speak", {{"type", "boolean"}}},
        {"text",
         {{"type", "string"},
          {"maxLength", policy.maximum_unicode_code_points}}},
        {"tone", {{"type", "string"}, {"enum", policy.allowed_tones}}},
        {"memory_ids_used",
         {{"type", "array"},
          {"maxItems", policy.maximum_memories},
          {"items", {{"type", "string"}}}}},
        {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}}};
  return AiRequest{
      .instructions =
          "Immutable character persona:\n" + std::string{persona} +
          "\nClassify conservatively. Suppress serious or private context. "
          "Return a short optional public preview in that persona without "
          "changing or contradicting it. Never mention internal IDs, private "
          "features, links, channels, or Discord users.",
      .conversation = {{"user", context.dump()}},
      .max_output_tokens = policy.maximum_output_tokens,
      .json_schema = AiRequest::JsonSchema{.name = "appearance_dry_run",
                                           .schema = schema.dump(),
                                           .strict = true}};
}

bool appearance_quiet_window_active(const AppearancePolicy &policy,
                                    const std::int64_t now_ms,
                                    const std::string_view timezone) {
  using namespace std::chrono;
  const auto *zone = locate_zone(std::string{timezone});
  const sys_time<milliseconds> now{milliseconds{now_ms}};
  const auto local = zoned_time{zone, now}.get_local_time();
  const auto local_day = floor<days>(local);
  const auto weekday_value = static_cast<int>(weekday{local_day}.c_encoding());
  const auto minute =
      static_cast<int>(duration_cast<minutes>(local - local_day).count());
  return std::ranges::any_of(policy.quiet_windows, [&](const auto &window) {
    return window.weekday == weekday_value && minute >= window.start_minute &&
           minute < window.end_minute;
  });
}

std::optional<std::int64_t>
appearance_quiet_deadline(const std::int64_t now_ms,
                          const std::string_view timezone,
                          const std::string_view kind,
                          const std::string_view local_time) {
  using namespace std::chrono;
  if (now_ms < 0)
    return std::nullopt;
  if (kind == "duration")
    return now_ms + duration_cast<milliseconds>(hours{2}).count();

  const auto *zone = locate_zone(std::string{timezone});
  const sys_time<milliseconds> now{milliseconds{now_ms}};
  const auto local_now = zoned_time{zone, now}.get_local_time();
  const auto local_day = floor<days>(local_now);
  if (kind == "tonight") {
    const auto deadline = zone->to_sys(local_day + days{1} + hours{10},
                                       choose::earliest);
    return duration_cast<milliseconds>(deadline.time_since_epoch()).count();
  }
  if (kind != "until" || local_time.size() != 5 || local_time[2] != ':' ||
      !std::isdigit(static_cast<unsigned char>(local_time[0])) ||
      !std::isdigit(static_cast<unsigned char>(local_time[1])) ||
      !std::isdigit(static_cast<unsigned char>(local_time[3])) ||
      !std::isdigit(static_cast<unsigned char>(local_time[4])))
    return std::nullopt;
  const int hour = (local_time[0] - '0') * 10 + (local_time[1] - '0');
  const int minute = (local_time[3] - '0') * 10 + (local_time[4] - '0');
  if (hour > 23 || minute > 59)
    return std::nullopt;

  for (int day_offset = 0; day_offset <= 1; ++day_offset) {
    const auto target = local_day + days{day_offset} + hours{hour} +
                        minutes{minute};
    const auto info = zone->get_info(target);
    if (info.result == local_info::nonexistent) {
      if (target > local_now)
        return std::nullopt;
      continue;
    }
    std::array<sys_time<milliseconds>, 2> possibilities{
        floor<milliseconds>(zone->to_sys(target, choose::earliest)),
        floor<milliseconds>(zone->to_sys(target, choose::latest))};
    std::ranges::sort(possibilities);
    for (const auto candidate : possibilities) {
      if (candidate <= now)
        continue;
      if (candidate - now > hours{24})
        return std::nullopt;
      return duration_cast<milliseconds>(candidate.time_since_epoch()).count();
    }
  }
  return std::nullopt;
}

AppearanceService::AppearanceService(
    AppearanceRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, AppearancePolicy policy,
    const AppearanceMode mode, std::string instance_id, AiClient *ai_client,
    AiWorkService *ai_work, Diagnostics &diagnostics, std::string persona,
    std::string timezone, AppearanceRuntimeStateProvider runtime_state,
    std::function<void()> outbox_wake)
    : repository_{repository}, clock_{clock}, ids_{ids},
      policy_{std::move(policy)}, mode_{mode},
      instance_id_{std::move(instance_id)}, ai_client_{ai_client},
      ai_work_{ai_work}, diagnostics_{diagnostics},
      persona_{std::move(persona)}, timezone_{std::move(timezone)},
      runtime_state_{std::move(runtime_state)},
      outbox_wake_{std::move(outbox_wake)} {
  if (instance_id_.empty())
    throw std::invalid_argument{"Appearance instance ID is required."};
  static_cast<void>(std::chrono::locate_zone(timezone_));
  if (!runtime_state_)
    runtime_state_ = [] { return AppearanceRuntimeState{}; };
}

void AppearanceService::start() {
  const auto started_at_ms = now_ms();
  repository_.register_policy(policy_, started_at_ms);
  repository_.activate_mode(mode_, started_at_ms);
  static_cast<void>(repository_.abandon_prior_instance_attempts(
      instance_id_, started_at_ms, ids_));
}

void AppearanceService::observe_message(
    const AppearanceMessageObservation &observation) {
  if (mode_ == AppearanceMode::off)
    return;
  if (auto candidate = repository_.observe_message(
          policy_, observation, ids_.next_id(), ids_.next_id()))
    evaluate(std::move(*candidate));
}

std::string
AppearanceService::simulate(const AppearanceSimulationRequest &request) {
  if (mode_ != AppearanceMode::dry_run)
    throw std::runtime_error{"Appearance simulation requires dry_run mode."};
  auto prepared = request;
  prepared.candidate_id = ids_.next_id();
  prepared.event_id = ids_.next_id();
  auto candidate = repository_.simulate(policy_, prepared);
  const auto reference = candidate.candidate_id;
  evaluate(std::move(candidate));
  return reference;
}

void AppearanceService::evaluate(AppearanceCandidate candidate) {
  const auto effective_policy =
      candidate.policy_version.empty() ||
              candidate.policy_version == policy_.policy_version
          ? policy_
          : repository_.load_policy(candidate.policy_version);
  decorate_runtime(candidate, effective_policy);
  auto evaluation =
      evaluate_appearance(effective_policy, mode_, candidate, now_ms());
  const auto decision_id = ids_.next_id();
  auto delivery = delivery_ids();
  if (!evaluation.eligible_for_model || ai_client_ == nullptr ||
      ai_work_ == nullptr) {
    const auto status =
        evaluation.eligible_for_model ? "model_unavailable" : "not_requested";
    static_cast<void>(repository_.record_final(
        effective_policy, mode_, candidate, evaluation, decision_id,
        ids_.next_id(), instance_id_, status, std::nullopt, delivery,
        now_ms()));
    return;
  }
  const auto decision_event_id = ids_.next_id();
  const auto prepared_at_ms = now_ms();
  if (!repository_.prepare_model(effective_policy, mode_, candidate, evaluation,
                                 decision_id, decision_event_id, instance_id_,
                                 prepared_at_ms))
    return;
  auto task_candidate = candidate;
  const auto submitted = ai_work_->submit(
      [this, candidate = std::move(task_candidate), decision_id,
       decision_event_id, effective_policy, evaluation,
       delivery,
       prepared_at_ms](const std::stop_token stop) mutable {
        std::optional<AppearanceModelResult> result;
        std::string status{"model_error"};
        try {
          result = parse_appearance_model_result(
              effective_policy,
              ai_client_->generate(
                  appearance_ai_request(effective_policy, candidate, persona_),
                  stop),
              candidate.supplied_memory_ids);
          status = result->serious_context
                       ? "model_serious"
                       : (result->should_speak ? "model_accepted"
                                               : "model_declined");
        } catch (const OperationCancelled &) {
          status = "model_cancelled";
        } catch (const AiRefusal &) {
          status = "model_refusal";
        } catch (const AiIncompleteResponse &) {
          status = "model_incomplete";
        } catch (const std::exception &error) {
          const std::string message{error.what()};
          if (message.starts_with("model_") && message.size() <= 96 &&
              std::ranges::all_of(message, [](const unsigned char value) {
                return std::isalnum(value) != 0 || value == '_' ||
                       value == '.' || value == '-';
              })) {
            status = message;
          } else if (message.find("timeout") != std::string::npos ||
                     message.find("timed out") != std::string::npos) {
            status = "model_timeout";
          } else {
            status = "model_failure";
          }
        } catch (...) {
          status = "model_failure";
        }
        complete_model(std::move(candidate), effective_policy, evaluation,
                       decision_id, decision_event_id, std::move(status),
                       std::move(result), std::move(delivery), prepared_at_ms);
      });
  if (submitted != SubmitResult::accepted) {
    complete_model(std::move(candidate), effective_policy, evaluation,
                   decision_id, decision_event_id, "model_queue_saturated",
                   std::nullopt, std::move(delivery), prepared_at_ms);
  }
}

void AppearanceService::complete_model(
    AppearanceCandidate candidate, const AppearancePolicy &policy,
    const AppearanceEvaluation &prepared_evaluation,
    const std::string_view decision_id, std::string event_id,
    std::string model_status, std::optional<AppearanceModelResult> result,
    AppearanceDeliveryIds delivery_ids,
    const std::int64_t prepared_at_ms) noexcept {
  auto completion_time = prepared_at_ms;
  try {
    completion_time = now_ms();
    decorate_runtime(candidate, policy);
    const auto fresh =
        evaluate_appearance(policy, mode_, candidate, completion_time);
    static_cast<void>(repository_.complete_model(
        policy, mode_, candidate, fresh, decision_id, event_id,
        std::move(model_status), std::move(result), delivery_ids,
        completion_time));
    wake_outbox();
    return;
  } catch (...) {
    diagnostics_.emit(
        {DiagnosticSeverity::error,
         "appearance.model_completion",
         "Appearance model completion failed; recording a safe rejection.",
         {}});
  }

  try {
    static_cast<void>(repository_.complete_model(
        policy, mode_, candidate, prepared_evaluation, decision_id,
        std::move(event_id), "model_completion_failure", std::nullopt,
        delivery_ids,
        completion_time));
  } catch (...) {
    diagnostics_.emit(
        {DiagnosticSeverity::error,
         "appearance.model_terminalization",
         "Appearance model rejection could not be persisted; startup "
         "recovery must abandon the pending decision.",
         {}});
  }
}

std::string AppearanceService::preview(const std::string_view reference) {
  const auto decision = repository_.decision(reference);
  if (!decision)
    return "No appearance decision matches that reference.";
  std::ostringstream output;
  output << "Appearance decision "
         << shortened_persistent_id(decision->decision_id)
         << "\nCandidate: " << shortened_persistent_id(decision->candidate_id)
         << " (" << decision->candidate_type
         << ")\nPolicy: " << decision->policy_version
         << "\nSummary: " << decision->safe_summary
         << "\nState: " << decision->state << "\nAction: " << decision->action
         << "\nReason: " << decision->reason << "\nScore: " << decision->score
         << "/100\nModel: " << decision->model_status << "\nGates:";
  for (const auto &item : decision->gates)
    output << "\n- " << item.name << ": " << (item.passed ? "pass" : "reject");
  output << "\nScore components:";
  for (const auto &item : decision->score_components)
    output << "\n- " << item.name << ": " << item.points;
  if (!decision->memory_ids.empty()) {
    output << "\nMemory references:";
    for (const auto &memory : decision->memory_ids)
      output << "\n- " << shortened_persistent_id(memory);
  }
  if (!decision->serious_categories.empty()) {
    output << "\nSerious categories:";
    for (const auto &category : decision->serious_categories)
      output << "\n- " << category;
  }
  const auto base = output.str();
  if (!decision->preview)
    return bounded_response(base);
  constexpr std::string_view label{"\nRetained preview: "};
  const auto available = base.size() + label.size() < 1'900
                             ? 1'900 - base.size() - label.size()
                             : 0;
  auto preview = *decision->preview;
  if (preview.size() > available) {
    constexpr std::string_view marker{" [preview truncated]"};
    const auto text_limit =
        available > marker.size() ? available - marker.size() : 0;
    preview.resize(text_limit);
    while (!preview.empty() && !valid_utf8(preview))
      preview.pop_back();
    if (available >= marker.size())
      preview += marker;
  }
  return bounded_response(base + std::string{label} + preview);
}

std::string AppearanceService::recent() {
  std::ostringstream output;
  output << "Recent appearance decisions";
  for (const auto &item : repository_.recent(10))
    output << "\n- " << item.decision_id << " " << item.action << " "
           << item.reason << " score=" << item.score;
  output << "\nAppearance public-outbox violations: "
         << repository_.public_outbox_violation_count();
  return bounded_response(output.str());
}

std::string AppearanceService::member_status_summary() {
  const auto state = repository_.control_summary(now_ms());
  std::ostringstream output;
  output << "kill_switch="
         << (state.globally_disabled ? "active" : "clear")
         << ", quiet=" << (state.quiet_until_ms ? "active" : "inactive");
  if (state.quiet_until_ms) {
    const std::chrono::sys_time<std::chrono::milliseconds> deadline{
        std::chrono::milliseconds{*state.quiet_until_ms}};
    const std::chrono::zoned_time local{
        std::chrono::locate_zone(timezone_), deadline};
    output << ", quiet_until="
           << std::format("{:%Y-%m-%d %H:%M %Z}", local);
  }
  return bounded_response(output.str());
}

std::string AppearanceService::status_summary() {
  const auto state = repository_.control_summary(now_ms());
  std::ostringstream output;
  output << "configured=" << appearance_mode_name(mode_)
         << ", persisted=" << appearance_mode_name(state.persisted_mode)
         << ", kill_switch="
         << (state.globally_disabled ? "active" : "clear")
         << ", quiet=" << (state.quiet_until_ms ? "active" : "inactive")
         << (state.quiet_until_ms
                 ? ", quiet_until_ms=" + std::to_string(*state.quiet_until_ms)
                 : std::string{})
         << ", reservations=" << state.active_reservations
         << ", outbox_pending=" << state.pending_outbox
         << ", outbox_failed=" << state.failed_outbox
         << ", outbox_ambiguous=" << state.ambiguous_outbox
         << ", model_failures_1h=" << state.recent_model_failures
         << ", delivery_failures_1h=" << state.recent_delivery_failures
         << ", feedback=" << state.feedback_more << "/"
         << state.feedback_less << "/" << state.feedback_not_relevant
         << ", recommendation=" << state.recommendation;
  if (state.last_queued_at_ms)
    output << ", last_queued_ms=" << *state.last_queued_at_ms;
  if (state.last_delivered_at_ms)
    output << ", last_delivered_ms=" << *state.last_delivered_at_ms;
  output << ", theme_review="
         << (state.theme_review_keys.empty() ? "none" : "recommended");
  if (!state.theme_review_keys.empty())
    output << ", theme_review_count=" << state.theme_review_keys.size();
  return bounded_response(output.str());
}

std::string AppearanceService::set_callback_consent(
    const DiscordSnowflake user_id, const bool enabled,
    std::string idempotency_key, std::string correlation_id) {
  const bool changed = repository_.set_callback_consent(
      user_id, enabled, now_ms(), ids_.next_id(), std::move(idempotency_key),
      std::move(correlation_id));
  if (!changed)
    return enabled ? "Appearance callbacks were already enabled."
                   : "Appearance callbacks were already disabled.";
  return enabled ? "Appearance callbacks are enabled."
                 : "Appearance callbacks are disabled.";
}

std::string AppearanceService::set_quiet(
    const DiscordSnowflake actor_user_id,
    const std::optional<std::int64_t> until_ms, std::string reason,
    std::string request_value,
    std::string idempotency_key, std::string correlation_id) {
  const auto result = repository_.set_quiet(AppearanceQuietMutation{
      .actor_user_id = actor_user_id,
      .quiet_until_ms = until_ms,
      .reason = std::move(reason),
      .request_value = std::move(request_value),
      .event_id = ids_.next_id(),
      .idempotency_key = std::move(idempotency_key),
      .correlation_id = std::move(correlation_id),
      .now_ms = now_ms()});
  switch (result) {
  case AppearanceMutationResult::applied:
    return until_ms ? "Server-wide appearance quiet is active."
                    : "Server-wide appearance quiet is cleared.";
  case AppearanceMutationResult::unchanged:
    return until_ms ? "A server-wide hush already extends at least that long."
                    : "Server-wide appearance quiet was already inactive.";
  case AppearanceMutationResult::unauthorized:
    return "Only the latest quiet setter or the owner may end this hush early.";
  default:
    return "That quiet request is invalid.";
  }
}

std::optional<std::int64_t>
AppearanceService::quiet_deadline(const std::string_view kind,
                                  const std::string_view local_time) const {
  return appearance_quiet_deadline(now_ms(), timezone_, kind, local_time);
}

std::string AppearanceService::set_global_disabled(
    const DiscordSnowflake actor_user_id, const bool disabled,
    std::string idempotency_key, std::string correlation_id) {
  const auto result = repository_.set_global_disabled(
      actor_user_id, disabled, now_ms(), ids_.next_id(),
      std::move(idempotency_key), std::move(correlation_id));
  if (result == AppearanceMutationResult::unauthorized)
    return "Only the configured owner may change the appearance kill switch.";
  if (result == AppearanceMutationResult::invalid)
    return "The appearance kill-switch request is invalid.";
  if (result == AppearanceMutationResult::unchanged)
    return disabled ? "Live appearances were already globally disabled."
                    : "The global appearance kill switch was already clear.";
  return disabled ? "Live appearances are globally disabled; unsent work was cancelled."
                  : "The global appearance kill switch is clear. Configured mode is unchanged.";
}

std::string
AppearanceService::feedback(const AppearanceFeedbackMutation &request) {
  auto prepared = request;
  prepared.feedback_id = ids_.next_id();
  prepared.event_id = ids_.next_id();
  prepared.now_ms = now_ms();
  if (prepared.action == AppearanceFeedbackAction::quiet_tonight ||
      prepared.control_id)
    prepared.quiet_until_ms = appearance_quiet_deadline(
        prepared.now_ms, timezone_, "tonight");
  const auto result = repository_.record_feedback(prepared);
  switch (result) {
  case AppearanceMutationResult::quiet_applied:
    return "Feedback recorded privately. Server-wide appearance quiet is active until tomorrow at 10:00 AM.";
  case AppearanceMutationResult::applied:
    return "Your appearance feedback was recorded privately.";
  case AppearanceMutationResult::unchanged:
    return "That feedback was already recorded.";
  case AppearanceMutationResult::conflict:
    return "You already recorded different feedback for that appearance.";
  case AppearanceMutationResult::expired:
    return "That appearance control has expired.";
  case AppearanceMutationResult::not_found:
    return "No eligible delivered appearance matches that reference.";
  default:
    return "That appearance feedback control is invalid.";
  }
}

std::string AppearanceService::trigger_owner_live_safe(
    const AppearanceSimulationRequest &request) {
  if (mode_ != AppearanceMode::live)
    throw std::runtime_error{"Owner live trigger requires live mode."};
  auto prepared = request;
  prepared.fixture = "owner_live_safe";
  prepared.candidate_id = ids_.next_id();
  prepared.event_id = ids_.next_id();
  auto candidate = repository_.simulate(policy_, prepared);
  decorate_runtime(candidate, policy_);
  const auto evaluation = evaluate_appearance(policy_, mode_, candidate, now_ms());
  const auto result = AppearanceModelResult{
      .serious_context = false,
      .serious_categories = {},
      .should_speak = true,
      .text = "[TEST] Sanguinius marks this safe live appearance check.",
      .tone = "warm",
      .memory_ids_used = {},
      .confidence = 1.0};
  const bool created = repository_.record_final(
      policy_, mode_, candidate, evaluation, ids_.next_id(), ids_.next_id(),
      instance_id_, "owner_fixture", result, delivery_ids(), now_ms());
  if (!created)
    return "That owner live appearance was already handled.";
  const auto decision = repository_.decision(candidate.candidate_id);
  if (decision && decision->action == "live_queued") {
    wake_outbox();
    return "Owner live appearance queued: " + candidate.candidate_id;
  }
  return "Owner live appearance suppressed by final gate: " +
         (decision ? decision->reason : std::string{"finalization_unknown"}) +
         ".";
}

std::optional<VerifiedAppearanceDelivery>
AppearanceService::verify_public_delivery(const ContextMessageSnapshot &message) {
  return repository_.verify_public_delivery(message);
}

bool AppearanceService::scan_events() {
  emit_failure_alerts();
  if (mode_ == AppearanceMode::off) {
    repository_.activate_mode(mode_, now_ms());
    return true;
  }
  const auto state = runtime_state_();
  if (!state.operational || state.degraded)
    return false;
  for (auto &candidate :
       repository_.scan_events(policy_, now_ms(), instance_id_))
    evaluate(std::move(candidate));
  return true;
}

void AppearanceService::emit_failure_alerts() noexcept {
  try {
    for (const auto &alert : repository_.claim_failure_alerts(now_ms())) {
      diagnostics_.emit(
          {DiagnosticSeverity::warning,
           "appearance." + alert.category + "_alert",
           "Appearance " + alert.category + " failures reached " +
               std::to_string(alert.occurrences) +
               " in the rolling hour; inspect private health details.",
           {}});
    }
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error,
                       "appearance.alert_evaluation",
                       "Appearance alert thresholds could not be evaluated.",
                       {}});
  }
}

void AppearanceService::purge() { repository_.purge(policy_, now_ms()); }

std::int64_t AppearanceService::purge_interval_ms() const noexcept {
  return std::min(policy_.activity_retention_ms,
                  appearance_maximum_purge_interval_ms);
}

std::int64_t AppearanceService::now_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock_.now().time_since_epoch())
      .count();
}

AppearanceDeliveryIds AppearanceService::delivery_ids() {
  return AppearanceDeliveryIds{
      .reservation_id = ids_.next_id(),
      .outbox_id = ids_.next_id(),
      .feedback_control_ids = {ids_.next_id(), ids_.next_id(), ids_.next_id(),
                               ids_.next_id()}};
}

void AppearanceService::wake_outbox() const {
  if (outbox_wake_)
    outbox_wake_();
}

void AppearanceService::decorate_runtime(AppearanceCandidate &candidate,
                                         const AppearancePolicy &policy) const {
  candidate.configured_quiet =
      candidate.configured_quiet ||
      appearance_quiet_window_active(policy, now_ms(), timezone_);
  const auto state = runtime_state_();
  candidate.operational = candidate.operational && state.operational;
  candidate.degraded = candidate.degraded || state.degraded;
}

} // namespace sanguinius
