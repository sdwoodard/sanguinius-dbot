#include "sanguinius/relationships.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace sanguinius {
namespace {

inline constexpr std::size_t maximum_relationship_profile_bytes = 1'900;

[[nodiscard]] std::string bounded_profile(std::string value) {
  if (value.size() <= maximum_relationship_profile_bytes)
    return value;
  constexpr std::string_view marker{"\n[additional Chronicle details omitted]"};
  auto end = maximum_relationship_profile_bytes - marker.size();
  while (end > 0 &&
         (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  value.resize(end);
  value += marker;
  return value;
}

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] int clamp_dimension(const int value) noexcept {
  return std::clamp(value, 0, 100);
}

[[nodiscard]] std::size_t overlap_count(
    const std::vector<std::string> &left,
    const std::unordered_set<std::string> &right,
    const std::size_t maximum) {
  std::size_t result = 0;
  for (const auto &item : left) {
    if (right.contains(item) && ++result == maximum) {
      break;
    }
  }
  return result;
}

[[nodiscard]] std::size_t tag_overlap_count(
    const std::vector<std::string> &tags,
    const std::unordered_set<std::string> &query_tokens,
    const std::size_t maximum) {
  std::size_t result = 0;
  for (const auto &tag : tags) {
    const auto tag_tokens = normalized_relevance_tokens(tag, 32);
    if (!tag_tokens.empty() &&
        std::ranges::all_of(tag_tokens, [&query_tokens](const auto &token) {
          return query_tokens.contains(token);
        }) &&
        ++result == maximum) {
      break;
    }
  }
  return result;
}

[[nodiscard]] std::string_view dimension_phrase(const std::string_view kind,
                                                const QualitativeBand band) {
  if (kind == "familiarity") {
    switch (band) {
    case QualitativeBand::dormant: return "a bond not yet written";
    case QualitativeBand::emerging: return "newly acquainted";
    case QualitativeBand::established: return "familiar company";
    case QualitativeBand::strong: return "a trusted companion";
    case QualitativeBand::storied: return "a storied ally";
    case QualitativeBand::legendary: return "an inner-circle bond";
    }
  }
  if (kind == "esteem") {
    switch (band) {
    case QualitativeBand::dormant: return "deeds not yet weighed";
    case QualitativeBand::emerging: return "deeds beginning to be noticed";
    case QualitativeBand::established: return "respected deeds";
    case QualitativeBand::strong: return "honored deeds";
    case QualitativeBand::storied: return "renowned deeds";
    case QualitativeBand::legendary: return "legendary esteem";
    }
  }
  if (kind == "mirth") {
    switch (band) {
    case QualitativeBand::dormant: return "quiet company";
    case QualitativeBand::emerging: return "the first shared jokes";
    case QualitativeBand::established: return "easy banter";
    case QualitativeBand::strong: return "well-practiced mischief";
    case QualitativeBand::storied: return "notorious shared chaos";
    case QualitativeBand::legendary: return "mythic levels of mischief";
    }
  }
  if (kind == "reliability") {
    switch (band) {
    case QualitativeBand::dormant: return "steadfastness not yet tested";
    case QualitativeBand::emerging: return "early signs of follow-through";
    case QualitativeBand::established: return "proven follow-through";
    case QualitativeBand::strong: return "steadfast reliability";
    case QualitativeBand::storied: return "unyielding reliability";
    case QualitativeBand::legendary: return "reliability beyond question";
    }
  }
  switch (band) {
  case QualitativeBand::dormant: return "no cause for narrative suspicion";
  case QualitativeBand::emerging: return "a faint, playful suspicion";
  case QualitativeBand::established: return "watchful amusement";
  case QualitativeBand::strong: return "a known agent of chaos";
  case QualitativeBand::storied: return "legendary cause for watchfulness";
  case QualitativeBand::legendary: return "destiny itself is watching";
  }
  return "unwritten";
}

[[nodiscard]] std::string safe_reason(const std::string_view reason) {
  if (reason == "chronicle.canon") return "A shared moment entered the Chronicle.";
  if (reason == "ai.direct") return "Sanguinius answered a direct call.";
  if (reason == "ai.direct_cooldown")
    return "A recent conversation continued the bond.";
  if (reason == "tarot.resolved") return "A wager reached its conclusion.";
  if (reason == "tarot.honored") return "A wager was honorably fulfilled.";
  if (reason == "appearance.positive_feedback")
    return "A well-timed appearance earned a smile.";
  if (reason == "title.awarded") return "A title marked a notable deed.";
  if (reason == "session.completed") return "A shared Chronicle session was completed.";
  return "A deliberate shared interaction was recorded.";
}

[[nodiscard]] std::optional<std::string>
string_option(const IncomingInteraction &interaction,
              const std::string_view name) {
  for (const auto &option : interaction.command_options) {
    if (option.name == name) {
      if (const auto *value = std::get_if<std::string>(&option.value)) {
        return *value;
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<DiscordSnowflake>
user_option(const IncomingInteraction &interaction,
            const std::string_view name) {
  for (const auto &option : interaction.command_options) {
    if (option.name == name) {
      if (const auto *value = std::get_if<DiscordId>(&option.value)) {
        return *value;
      }
    }
  }
  return std::nullopt;
}

} // namespace

RelationshipDelta relationship_policy(const RelationshipSourceKind kind,
                                      const bool direct_cooldown_active) noexcept {
  switch (kind) {
  case RelationshipSourceKind::chronicle_canon:
    return {.familiarity = 1, .esteem = 1};
  case RelationshipSourceKind::direct_ai:
    return {.familiarity = direct_cooldown_active ? 0 : 1};
  case RelationshipSourceKind::tarot_resolved:
  case RelationshipSourceKind::tarot_honored:
  case RelationshipSourceKind::appearance_positive_feedback:
    return {};
  case RelationshipSourceKind::session_completed:
    return {.familiarity = 1};
  case RelationshipSourceKind::title_awarded:
    return {.esteem = 1};
  }
  return {};
}

RelationshipDimensions
apply_relationship_delta(const RelationshipDimensions current,
                         const RelationshipDelta requested) noexcept {
  return {.familiarity = clamp_dimension(current.familiarity + requested.familiarity),
          .esteem = clamp_dimension(current.esteem + requested.esteem),
          .mirth = clamp_dimension(current.mirth + requested.mirth),
          .reliability = clamp_dimension(current.reliability + requested.reliability),
          .wariness = clamp_dimension(current.wariness + requested.wariness)};
}

RelationshipDelta
applied_relationship_delta(const RelationshipDimensions old_values,
                           const RelationshipDimensions new_values) noexcept {
  return {.familiarity = new_values.familiarity - old_values.familiarity,
          .esteem = new_values.esteem - old_values.esteem,
          .mirth = new_values.mirth - old_values.mirth,
          .reliability = new_values.reliability - old_values.reliability,
          .wariness = new_values.wariness - old_values.wariness};
}

QualitativeBand qualitative_band(const int value) noexcept {
  if (value <= 0) return QualitativeBand::dormant;
  if (value <= 4) return QualitativeBand::emerging;
  if (value <= 14) return QualitativeBand::established;
  if (value <= 29) return QualitativeBand::strong;
  if (value <= 59) return QualitativeBand::storied;
  return QualitativeBand::legendary;
}

std::string_view qualitative_band_name(const QualitativeBand band) noexcept {
  switch (band) {
  case QualitativeBand::dormant: return "dormant";
  case QualitativeBand::emerging: return "emerging";
  case QualitativeBand::established: return "established";
  case QualitativeBand::strong: return "strong";
  case QualitativeBand::storied: return "storied";
  case QualitativeBand::legendary: return "legendary";
  }
  return "dormant";
}

std::string relationship_style_hint(
    const RelationshipDimensions &dimensions) {
  std::vector<std::string_view> hints;
  if (dimensions.familiarity > 0) {
    hints.push_back(dimension_phrase("familiarity",
                                    qualitative_band(dimensions.familiarity)));
  }
  if (dimensions.esteem > 0) {
    hints.push_back(dimension_phrase("esteem", qualitative_band(dimensions.esteem)));
  }
  if (dimensions.mirth > 0) {
    hints.push_back(dimension_phrase("mirth", qualitative_band(dimensions.mirth)));
  }
  if (hints.empty()) return {};
  std::string result{"Respond with the tone appropriate for "};
  for (std::size_t index = 0; index < hints.size(); ++index) {
    if (index != 0) result += index + 1 == hints.size() ? " and " : ", ";
    result += hints[index];
  }
  result += ". Treat this only as private style guidance; never state or explain it.";
  return result;
}

std::vector<std::string>
normalized_relevance_tokens(const std::string_view text,
                            const std::size_t limit) {
  static const std::unordered_set<std::string> stop_words{
      "the", "and", "that", "this", "with", "from", "have", "what",
      "when", "where", "who", "why", "how", "you", "your", "are",
      "was", "were", "for", "but", "not", "can", "could", "would",
      "should", "about", "into", "just", "then", "than", "them", "they"};
  std::vector<std::string> result;
  std::set<std::string> seen;
  std::string token;
  const auto flush = [&] {
    if (token.size() >= 3 && !stop_words.contains(token) &&
        seen.insert(token).second && result.size() < limit) {
      result.push_back(token);
    }
    token.clear();
  };
  for (const unsigned char byte : text) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
      token.push_back(static_cast<char>(byte));
    } else if (byte >= 'A' && byte <= 'Z') {
      token.push_back(static_cast<char>(std::tolower(byte)));
    } else {
      flush();
      if (result.size() == limit) break;
    }
  }
  flush();
  return result;
}

std::vector<RankedMemory>
rank_prompt_memories(std::vector<MemoryCandidate> candidates,
                     const std::string_view current_request,
                     const std::string_view replied_text,
                     const std::int64_t now_ms) {
  if (candidates.size() > maximum_prompt_memory_candidates) {
    candidates.resize(maximum_prompt_memory_candidates);
  }
  const auto current_tokens = normalized_relevance_tokens(current_request, 32);
  const auto replied_tokens = normalized_relevance_tokens(replied_text, 24);
  const std::unordered_set<std::string> current_set{current_tokens.begin(),
                                                    current_tokens.end()};
  const std::unordered_set<std::string> replied_set{replied_tokens.begin(),
                                                    replied_tokens.end()};
  std::vector<RankedMemory> ranked;
  for (auto &candidate : candidates) {
    const auto memory_tokens = normalized_relevance_tokens(candidate.text, 64);
    const std::unordered_set<std::string> memory_set{memory_tokens.begin(),
                                                     memory_tokens.end()};
    const auto current_text = overlap_count(current_tokens, memory_set, 5);
    const auto replied_text_matches = overlap_count(replied_tokens, memory_set, 5);
    const auto current_tags = tag_overlap_count(candidate.tags, current_set, 5);
    const auto replied_tags = tag_overlap_count(candidate.tags, replied_set, 5);
    const auto signals = current_text + replied_text_matches + current_tags + replied_tags;
    if (signals == 0) continue;
    int recency = 0;
    const auto age = std::max<std::int64_t>(0, now_ms - candidate.created_at_ms);
    if (age <= 30LL * 24 * 60 * 60 * 1'000) recency = 12;
    else if (age <= 90LL * 24 * 60 * 60 * 1'000) recency = 8;
    else if (age <= 365LL * 24 * 60 * 60 * 1'000) recency = 4;
    const int score = static_cast<int>(current_tags * 40 + current_text * 12 +
                                       replied_tags * 16 + replied_text_matches * 4) +
                      8 + recency;
    if (score >= 20) {
      ranked.push_back({.memory = std::move(candidate),
                        .score = score,
                        .tag_matches = current_tags + replied_tags});
    }
  }
  std::ranges::sort(ranked, [](const RankedMemory &left, const RankedMemory &right) {
    if (left.score != right.score) return left.score > right.score;
    if (left.tag_matches != right.tag_matches)
      return left.tag_matches > right.tag_matches;
    if (left.memory.created_at_ms != right.memory.created_at_ms)
      return left.memory.created_at_ms > right.memory.created_at_ms;
    return left.memory.memory_id < right.memory.memory_id;
  });
  std::vector<RankedMemory> selected;
  std::size_t bytes = 0;
  for (auto &item : ranked) {
    if (selected.size() == maximum_prompt_memories) break;
    if (bytes + item.memory.text.size() > maximum_prompt_memory_bytes) continue;
    bytes += item.memory.text.size();
    selected.push_back(std::move(item));
  }
  return selected;
}

RelationshipService::RelationshipService(
    RelationshipRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, const ServerScopeConfiguration scope,
    std::string application_instance_id)
    : repository_{repository}, clock_{clock}, ids_{ids}, scope_{scope},
      application_instance_id_{std::move(application_instance_id)} {}

bool RelationshipService::in_feature_scope(
    const IncomingMessage &message) const noexcept {
  return message.guild_id == scope_.guild_id &&
         message.channel_id == scope_.primary_channel_id;
}

PreparedPromptContext RelationshipService::prepare_prompt(
    const IncomingMessage &message, std::string current_request,
    std::string replied_text) {
  if (!in_feature_scope(message)) return {};
  return repository_.prepare_prompt_context(PreparePromptContextRequest{
      .attempt_id = ids_.next_id(),
      .application_instance_id = application_instance_id_,
      .requester_user_id = message.author_user_id,
      .requester_username = message.author_username,
      .requester_display_name = message.author_display_name,
      .guild_id = message.guild_id,
      .channel_id = message.channel_id,
      .source_message_id = message.message_id,
      .current_request = std::move(current_request),
      .replied_text = std::move(replied_text),
      .correlation_id = message.correlation_id,
      .now_ms = unix_milliseconds(clock_),
  });
}

PromptFinalizationStatus
RelationshipService::complete_prompt(const std::string_view attempt_id) {
  return repository_.complete_prompt_attempt(
      {.attempt_id = std::string{attempt_id},
       .source_event_id = ids_.next_id(),
       .relationship_event_id = ids_.next_id(),
       .now_ms = unix_milliseconds(clock_)});
}

PromptFinalizationStatus
RelationshipService::fail_prompt(const std::string_view attempt_id,
                                 const std::string_view outcome,
                                 const std::string_view error_code) {
  return repository_.fail_prompt_attempt(
      {.attempt_id = std::string{attempt_id},
       .outcome = std::string{outcome},
       .error_code = std::string{error_code},
       .now_ms = unix_milliseconds(clock_)});
}

InteractionMessage
RelationshipService::profile(const IncomingInteraction &interaction) {
  const auto target = user_option(interaction, "user").value_or(interaction.user_id);
  const bool public_view = target != interaction.user_id;
  const auto data = repository_.profile(interaction.user_id, target, public_view,
                                        unix_milliseconds(clock_));
  if (!data.found || data.is_bot || !data.chronicle_opt_in) {
    return text_message(public_view ? "No public Chronicle profile is available."
                                    : "No private Chronicle profile is available.");
  }
  std::ostringstream output;
  if (public_view) {
    output << "**Public Chronicle profile — <@" << target.str() << ">**\n"
           << "Shared canon entries: " << data.shared_canon_count;
    if (data.featured_title) output << "\nFeatured title: **" << *data.featured_title << "**";
    if (data.latest_session_summary) output << "\nLatest chapter: " << *data.latest_session_summary;
    for (const auto &title : data.visible_canon_titles) output << "\n- " << title;
    if (data.visible_canon_titles.empty()) output << "\nNo shared canon headings yet.";
    return text_message(bounded_profile(output.str()));
  }
  output << "**Your Chronicle profile**\n"
         << "Bond: " << dimension_phrase("familiarity", qualitative_band(data.dimensions.familiarity))
         << "\nRegard: " << dimension_phrase("esteem", qualitative_band(data.dimensions.esteem))
         << "\nBanter: " << dimension_phrase("mirth", qualitative_band(data.dimensions.mirth))
         << "\nSteadfastness: " << dimension_phrase("reliability", qualitative_band(data.dimensions.reliability))
         << "\nNarrative caution: " << dimension_phrase("wariness", qualitative_band(data.dimensions.wariness))
         << "\nMemory callbacks: " << (data.memory_callbacks ? "enabled" : "disabled");
  if (data.featured_title) output << "\nFeatured title: **" << *data.featured_title << "**";
  if (data.latest_session_summary) output << "\nLatest chapter: " << *data.latest_session_summary;
  output << "\nChronicle session: " << (data.session_open ? "open" : "closed");
  if (!data.recent_reasons.empty()) {
    output << "\nRecent continuity:";
    for (const auto &reason : data.recent_reasons) output << "\n- " << safe_reason(reason);
  }
  if (!data.visible_canon_titles.empty()) {
    output << "\nVisible Chronicle headings:";
    for (const auto &title : data.visible_canon_titles) output << "\n- " << title;
  }
  return text_message(bounded_profile(output.str()));
}

InteractionMessage RelationshipService::set_memory_callbacks(
    const IncomingInteraction &interaction) {
  const auto mode = string_option(interaction, "mode");
  if (!mode || (*mode != "on" && *mode != "off")) {
    return text_message("Choose memory callbacks `on` or `off`.");
  }
  const auto result = repository_.set_memory_callbacks({
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .user_id = interaction.user_id,
      .enabled = *mode == "on",
      .event_id = ids_.next_id(),
      .correlation_id = interaction.correlation_id,
      .idempotency_key = "chronicle:callbacks:" + interaction.interaction_id.str(),
      .now_ms = unix_milliseconds(clock_),
  });
  if (result == PreferenceChangeStatus::chronicle_opted_out) {
    return text_message("Chronicle participation must be enabled before memory callbacks.");
  }
  if (result == PreferenceChangeStatus::unchanged) {
    return text_message(*mode == "on" ? "Memory callbacks are already enabled."
                                      : "Memory callbacks are already disabled.");
  }
  return text_message(*mode == "on" ? "Memory callbacks are now enabled."
                                    : "Memory callbacks are now disabled.");
}

std::size_t RelationshipService::recover() {
  const auto now_ms = unix_milliseconds(clock_);
  const auto attempts =
      repository_.recover_prompt_attempts(application_instance_id_, now_ms);
  return attempts + repository_.synchronize_chronicle_sources(ids_, now_ms);
}

ProjectionCheckResult RelationshipService::check_projection() {
  return repository_.check_projection();
}

} // namespace sanguinius
