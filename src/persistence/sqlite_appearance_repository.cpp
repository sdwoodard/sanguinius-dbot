#include "sanguinius/persistence/sqlite_appearance_repository.hpp"

#include "sqlite_durable_work_writes.hpp"

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;
using CandidateSource = std::pair<std::string, std::string>;

[[nodiscard]] std::optional<std::int64_t>
optional_json_integer(const Json &value, const std::string_view key) {
  const auto &field = value.at(key);
  return field.is_null()
             ? std::nullopt
             : std::optional<std::int64_t>{field.get<std::int64_t>()};
}

[[nodiscard]] std::string bounded_utf8(std::string value,
                                       const std::size_t maximum) {
  if (!valid_utf8(value))
    return {};
  if (value.size() <= maximum)
    return value;
  value.resize(maximum);
  while (!value.empty() && !valid_utf8(value))
    value.pop_back();
  return value;
}

[[nodiscard]] std::string candidate_json(const AppearanceCandidate &value) {
  auto memory_references = Json::array();
  for (const auto &memory : value.memory_context)
    memory_references.push_back(
        {{"memory_id", memory.memory_id}, {"revision", memory.revision}});
  Json json{{"excerpts", value.excerpts},
            {"memory_ids", value.supplied_memory_ids},
            {"memory_references", std::move(memory_references)},
            {"safe_summary", value.safe_summary},
            {"source_enabled", value.source_enabled},
            {"correct_scope", value.correct_scope},
            {"manual_quiet", value.manual_quiet},
            {"configured_quiet", value.configured_quiet},
            {"bot_last", value.bot_last_meaningful_speaker},
            {"operational", value.operational},
            {"degraded", value.degraded},
            {"exact_duplicate", value.exact_duplicate},
            {"budget_available", value.budget_available},
            {"gap_available", value.gap_available},
            {"messages_after_previous", value.messages_after_previous},
            {"theme_available", value.theme_available},
            {"memory_available", value.memory_available},
            {"consented", value.consented},
            {"visible", value.visible},
            {"alternating_turns", value.alternating_turns},
            {"chronicle_specificity", value.chronicle_specificity},
            {"novelty_age_ms", value.novelty_age_ms},
            {"recurrence_matches", value.recurrence_matches},
            {"bot_speech_age_ms", value.bot_speech_age_ms},
            {"human_messages_since_bot", value.human_messages_since_bot},
            {"human_message_count", value.human_message_count},
            {"repetition_age_ms", value.repetition_age_ms},
            {"uncertainty_penalty", value.uncertainty_penalty},
            {"serious_category", value.deterministic_serious_category}};
  return json.dump();
}

[[nodiscard]] AppearancePolicy
load_policy_snapshot(SqliteConnection &connection,
                     const std::string_view policy_version) {
  auto query = connection.prepare(
      "SELECT canonical_json FROM appearance_policy_snapshot "
      "WHERE policy_version=?");
  query.bind(1, policy_version);
  if (!query.step())
    throw std::runtime_error{"Stored appearance policy is unavailable."};
  auto policy = parse_appearance_policy(query.column_text(0));
  if (policy.policy_version != policy_version)
    throw std::runtime_error{"Stored appearance policy identity is invalid."};
  return policy;
}

[[nodiscard]] AppearanceCandidate read_candidate(SqliteConnection &connection,
                                                 const std::string_view id) {
  auto query = connection.prepare("SELECT "
                                  "candidate_id,policy_version,candidate_type,"
                                  "context_json,theme_key,owner_simulation,"
                                  "created_at_ms,expires_at_ms FROM "
                                  "appearance_candidate WHERE candidate_id=?");
  query.bind(1, id);
  if (!query.step())
    throw std::runtime_error{"Appearance candidate is missing."};
  const auto type = parse_appearance_candidate_type(query.column_text(2));
  if (!type)
    throw std::runtime_error{"Stored appearance candidate type is invalid."};
  const auto json = Json::parse(query.column_text(3));
  AppearanceCandidate result{
      .candidate_id = query.column_text(0),
      .policy_version = query.column_text(1),
      .type = *type,
      .created_at_ms = query.column_int64(6),
      .expires_at_ms = query.column_int64(7),
      .actors = {},
      .excerpts = json.at("excerpts").get<std::vector<std::string>>(),
      .source_context = {},
      .supplied_memory_ids =
          json.at("memory_ids").get<std::vector<std::string>>(),
      .memory_context = {},
      .safe_summary = json.value("safe_summary", std::string{}),
      .theme_key = query.column_is_null(4)
                       ? std::nullopt
                       : std::optional<std::string>{query.column_text(4)},
      .owner_simulation = query.column_int64(5) != 0,
      .source_enabled = json.at("source_enabled").get<bool>(),
      .correct_scope = json.at("correct_scope").get<bool>(),
      .manual_quiet = json.at("manual_quiet").get<bool>(),
      .configured_quiet = json.at("configured_quiet").get<bool>(),
      .bot_last_meaningful_speaker = json.at("bot_last").get<bool>(),
      .operational = json.at("operational").get<bool>(),
      .degraded = json.at("degraded").get<bool>(),
      .exact_duplicate = json.at("exact_duplicate").get<bool>(),
      .budget_available = json.at("budget_available").get<bool>(),
      .gap_available = json.at("gap_available").get<bool>(),
      .messages_after_previous = json.at("messages_after_previous").get<bool>(),
      .theme_available = json.at("theme_available").get<bool>(),
      .memory_available = json.at("memory_available").get<bool>(),
      .consented = json.at("consented").get<bool>(),
      .visible = json.at("visible").get<bool>(),
      .alternating_turns = json.at("alternating_turns").get<bool>(),
      .chronicle_specificity = json.at("chronicle_specificity").get<int>(),
      .novelty_age_ms = optional_json_integer(json, "novelty_age_ms"),
      .recurrence_matches = json.at("recurrence_matches").get<std::size_t>(),
      .bot_speech_age_ms = optional_json_integer(json, "bot_speech_age_ms"),
      .human_messages_since_bot =
          json.at("human_messages_since_bot").get<std::size_t>(),
      .human_message_count = json.at("human_message_count").get<std::int64_t>(),
      .repetition_age_ms = optional_json_integer(json, "repetition_age_ms"),
      .uncertainty_penalty = json.at("uncertainty_penalty").get<int>(),
      .deterministic_serious_category =
          json.at("serious_category").is_null()
              ? std::nullopt
              : std::optional<std::string>{
                    json.at("serious_category").get<std::string>()}};
  auto actors =
      connection.prepare("SELECT user_id FROM appearance_candidate_actor WHERE "
                         "candidate_id=? ORDER BY user_id");
  actors.bind(1, id);
  while (actors.step())
    result.actors.push_back(DiscordSnowflake::parse(actors.column_text(0)));
  const auto load_memory = [&](const std::string &memory_id,
                               const std::size_t revision) {
    auto memory = connection.prepare(
        "SELECT m.text,(m.revision=? AND m.status='confirmed' AND "
        "m.visibility='shared' AND m.sensitivity='ordinary' AND NOT EXISTS("
        "SELECT 1 FROM memory_subject s JOIN user_preference p ON "
        "p.user_id=s.subject_id WHERE s.memory_id=m.memory_id AND "
        "s.subject_type='user' AND (p.chronicle_opt_in=0 OR "
        "p.memory_callback_opt_in=0 OR p.appearance_callback_opt_in=0))) "
        "FROM memory m WHERE m.memory_id=?");
    memory.bind(1, static_cast<std::int64_t>(revision));
    memory.bind(2, memory_id);
    if (!memory.step()) {
      result.consented = false;
      result.visible = false;
      result.memory_available = false;
      return;
    }
    const bool available = memory.column_int64(1) != 0;
    result.memory_context.push_back(
        {memory_id, revision, available ? memory.column_text(0) : ""});
    if (!available) {
      result.consented = false;
      result.visible = false;
      result.memory_available = false;
    }
  };
  if (json.contains("memory_references")) {
    for (const auto &reference : json.at("memory_references"))
      load_memory(reference.at("memory_id").get<std::string>(),
                  reference.at("revision").get<std::size_t>());
  } else {
    for (const auto &memory_id : result.supplied_memory_ids) {
      auto revision =
          connection.prepare("SELECT revision FROM memory WHERE memory_id=?");
      revision.bind(1, memory_id);
      if (revision.step())
        load_memory(memory_id,
                    static_cast<std::size_t>(revision.column_int64(0)));
      else {
        result.consented = false;
        result.visible = false;
        result.memory_available = false;
      }
    }
  }
  auto chronicle_source = connection.prepare(
      "SELECT e.title,e.body,COALESCE((SELECT group_concat(t.tag,' ') FROM "
      "chronicle_tag t WHERE t.entry_id=e.entry_id),''),(e.status='canon' AND "
      "e.visibility='shared' "
      "AND e.source_guild_id=c.guild_id AND e.source_channel_id=c.channel_id "
      "AND NOT EXISTS(SELECT 1 FROM chronicle_participant cp "
      "JOIN user_preference p ON p.user_id=cp.user_id "
      "WHERE cp.entry_id=e.entry_id AND (p.chronicle_opt_in=0 OR "
      "p.appearance_callback_opt_in=0))) FROM appearance_candidate_source s "
      "JOIN appearance_candidate c ON c.candidate_id=s.candidate_id "
      "JOIN chronicle_entry e ON e.entry_id=s.source_id "
      "WHERE s.candidate_id=? AND s.source_kind='chronicle_entry' LIMIT 1");
  chronicle_source.bind(1, id);
  if (chronicle_source.step()) {
    if (chronicle_source.column_int64(3) != 0) {
      const auto source_policy =
          load_policy_snapshot(connection, result.policy_version);
      result.source_context.push_back(
          bounded_utf8(chronicle_source.column_text(0) + ": " +
                           chronicle_source.column_text(1) + " " +
                           chronicle_source.column_text(2),
                       source_policy.activity_maximum_utf8_bytes_per_row));
    } else {
      result.consented = false;
      result.visible = false;
    }
  }
  return result;
}

[[nodiscard]] std::vector<std::string>
literal_tokens(const std::string_view text) {
  std::set<std::string, std::less<>> unique;
  std::string token;
  const auto flush = [&] {
    if (token.size() >= 4 && unique.size() < 16)
      unique.insert(token);
    token.clear();
  };
  for (const unsigned char byte : text) {
    if (std::isalnum(byte) != 0)
      token.push_back(static_cast<char>(std::tolower(byte)));
    else
      flush();
  }
  flush();
  return {unique.begin(), unique.end()};
}

[[nodiscard]] bool
contains_literal_token(const std::string_view text,
                       const std::vector<std::string> &tokens) {
  std::string normalized{text};
  std::ranges::transform(normalized, normalized.begin(),
                         [](const unsigned char byte) {
                           return static_cast<char>(std::tolower(byte));
                         });
  return std::ranges::any_of(tokens, [&](const auto &token) {
    auto position = normalized.find(token);
    while (position != std::string::npos) {
      const auto end = position + token.size();
      const bool left =
          position == 0 ||
          std::isalnum(static_cast<unsigned char>(normalized[position - 1])) ==
              0;
      const bool right =
          end == normalized.size() ||
          std::isalnum(static_cast<unsigned char>(normalized[end])) == 0;
      if (left && right)
        return true;
      position = normalized.find(token, position + 1);
    }
    return false;
  });
}

[[nodiscard]] std::size_t
matching_excerpt_count(const std::vector<std::string> &excerpts,
                       const std::string_view source) {
  const auto tokens = literal_tokens(source);
  return static_cast<std::size_t>(
      std::ranges::count_if(excerpts, [&](const auto &excerpt) {
        return contains_literal_token(excerpt, tokens);
      }));
}

void select_literal_memory_context(SqliteConnection &connection,
                                   const AppearancePolicy &policy,
                                   const std::string_view text,
                                   const std::int64_t now_ms,
                                   AppearanceCandidate &candidate) {
  const auto tokens = literal_tokens(text);
  if (tokens.empty() || policy.maximum_memories == 0)
    return;
  auto memories = connection.prepare(
      "SELECT m.memory_id,m.revision,m.text,COALESCE((SELECT "
      "group_concat(s.subject_id,' ') FROM memory_subject s WHERE "
      "s.memory_id=m.memory_id AND s.subject_type='topic'),'') FROM memory m "
      "WHERE "
      "m.status='confirmed' "
      "AND m.visibility='shared' AND m.sensitivity='ordinary' "
      "AND (m.expires_at_ms IS NULL OR m.expires_at_ms>?) AND NOT EXISTS("
      "SELECT 1 FROM memory_subject s JOIN user_preference p ON "
      "p.user_id=s.subject_id "
      "WHERE s.memory_id=m.memory_id AND s.subject_type='user' AND "
      "(p.chronicle_opt_in=0 OR p.memory_callback_opt_in=0 OR "
      "p.appearance_callback_opt_in=0)) "
      "ORDER BY m.confirmed_at_ms DESC,m.memory_id LIMIT 32");
  memories.bind(1, now_ms);
  while (memories.step() &&
         candidate.memory_context.size() < policy.maximum_memories) {
    const auto memory_text = memories.column_text(2);
    const auto match_text = memory_text + " " + memories.column_text(3);
    if (!contains_literal_token(match_text, tokens))
      continue;
    const auto memory_id = memories.column_text(0);
    candidate.supplied_memory_ids.push_back(memory_id);
    candidate.memory_context.push_back(
        {memory_id, static_cast<std::size_t>(memories.column_int64(1)),
         memory_text});
    if (!candidate.deterministic_serious_category)
      candidate.deterministic_serious_category =
          detect_serious_context(policy, match_text);
  }
  if (!candidate.memory_context.empty()) {
    candidate.chronicle_specificity =
        policy.score_weights.at("chronicle_exact");
    for (const auto &memory : candidate.memory_context) {
      auto topics = connection.prepare(
          "SELECT subject_id FROM memory_subject WHERE memory_id=? AND "
          "subject_type='topic' ORDER BY subject_id");
      topics.bind(1, memory.memory_id);
      auto match_text = memory.text;
      while (topics.step()) {
        match_text.push_back(' ');
        match_text += topics.column_text(0);
      }
      candidate.recurrence_matches =
          std::max(candidate.recurrence_matches,
                   matching_excerpt_count(candidate.excerpts, match_text));
    }
  }
}

void apply_theme_history(SqliteConnection &connection,
                         AppearanceCandidate &candidate,
                         const std::int64_t now_ms) {
  if (!candidate.theme_key)
    return;
  auto query = connection.prepare(
      "SELECT max(d.finalized_at_ms) FROM appearance_decision d "
      "JOIN appearance_candidate c ON c.candidate_id=d.candidate_id "
      "WHERE d.action='hypothetical' AND c.theme_key=?");
  query.bind(1, *candidate.theme_key);
  if (!query.step())
    throw std::runtime_error{"Unable to inspect appearance theme history."};
  if (!query.column_is_null(0)) {
    const auto age = std::max<std::int64_t>(0, now_ms - query.column_int64(0));
    candidate.novelty_age_ms = age;
    candidate.repetition_age_ms = age;
  }
}

void apply_source_subject_preferences(
    SqliteConnection &connection, const std::vector<DiscordSnowflake> &subjects,
    const std::int64_t now_ms, AppearanceCandidate &candidate) {
  for (const auto subject : subjects) {
    auto preference = connection.prepare(
        "SELECT chronicle_opt_in,appearance_callback_opt_in,quiet_until_ms "
        "FROM user_preference WHERE user_id=?");
    preference.bind(1, subject.str());
    if (!preference.step() || preference.column_int64(0) == 0 ||
        preference.column_int64(1) == 0) {
      candidate.consented = false;
      continue;
    }
    if (!preference.column_is_null(2) && preference.column_int64(2) > now_ms)
      candidate.manual_quiet = true;
  }
}

void apply_event_source(
    SqliteConnection &connection, const AppearancePolicy &policy,
    const std::string_view event_type, const std::string_view aggregate_id,
    const DiscordSnowflake guild, const DiscordSnowflake channel,
    const std::int64_t now_ms, AppearanceCandidate &candidate,
    const bool include_context) {
  bool available{};
  std::string source_text;
  std::vector<DiscordSnowflake> subjects;
  if (event_type == "chronicle.entry_canonized.v1" ||
      event_type == "chronicle.anniversary_delivered.v1") {
    auto entry = connection.prepare(
        "SELECT title,body,status,visibility,source_guild_id,source_channel_id "
        "FROM chronicle_entry WHERE entry_id=?");
    entry.bind(1, aggregate_id);
    if (entry.step()) {
      available = entry.column_text(2) == "canon" &&
                  entry.column_text(3) == "shared" &&
                  entry.column_text(4) == guild.str() &&
                  entry.column_text(5) == channel.str();
      source_text = entry.column_text(0) + ": " + entry.column_text(1);
    }
    auto participants = connection.prepare(
        "SELECT DISTINCT user_id FROM chronicle_participant WHERE entry_id=? "
        "ORDER BY user_id");
    participants.bind(1, aggregate_id);
    while (participants.step())
      subjects.push_back(DiscordSnowflake::parse(participants.column_text(0)));
    candidate.theme_key = "chronicle:" + std::string{aggregate_id};
  } else if (event_type == "chronicle.session_started.v1" ||
             event_type == "chronicle.session_completed.v1") {
    auto session = connection.prepare("SELECT state,guild_id,channel_id FROM "
                                      "chronicle_session WHERE session_id=?");
    session.bind(1, aggregate_id);
    if (session.step()) {
      const auto state = session.column_text(0);
      available =
          session.column_text(1) == guild.str() &&
          session.column_text(2) == channel.str() &&
          (event_type == "chronicle.session_started.v1" ? state != "abandoned"
                                                        : state == "closed");
    }
    source_text = event_type == "chronicle.session_started.v1"
                      ? "A shared Chronicle session opened."
                      : "A shared Chronicle session was completed.";
    auto participants = connection.prepare(
        "SELECT user_id FROM chronicle_session_participant WHERE session_id=? "
        "ORDER BY user_id");
    participants.bind(1, aggregate_id);
    while (participants.step())
      subjects.push_back(DiscordSnowflake::parse(participants.column_text(0)));
    candidate.theme_key = "session:" + std::string{aggregate_id};
  } else if (event_type == "chronicle.title_awarded.v1") {
    auto title = connection.prepare(
        "SELECT d.title,d.description,g.recipient_user_id,g.state "
        "FROM chronicle_title_grant g JOIN chronicle_title_definition d "
        "ON d.definition_id=g.definition_id WHERE g.grant_id=?");
    title.bind(1, aggregate_id);
    if (title.step()) {
      available = title.column_text(3) == "active";
      source_text = title.column_text(0) + ": " + title.column_text(1);
      subjects.push_back(DiscordSnowflake::parse(title.column_text(2)));
    }
    candidate.theme_key = "title:" + std::string{aggregate_id};
  }
  candidate.visible = candidate.visible && available;
  apply_source_subject_preferences(connection, subjects, now_ms, candidate);
  if (available && !source_text.empty()) {
    if (!candidate.deterministic_serious_category)
      candidate.deterministic_serious_category =
          detect_serious_context(policy, source_text);
    if (include_context)
      candidate.source_context.push_back(bounded_utf8(
          std::move(source_text), policy.activity_maximum_utf8_bytes_per_row));
  }
}

void refresh_event_source(SqliteConnection &connection,
                          const AppearancePolicy &policy,
                          const std::int64_t now_ms,
                          AppearanceCandidate &candidate,
                          const bool include_context = false) {
  auto source = connection.prepare(
      "SELECT o.event_type,o.aggregate_id,o.guild_id,o.channel_id "
      "FROM appearance_candidate_source s JOIN appearance_event_observation o "
      "ON o.source_event_id=s.source_id WHERE s.candidate_id=? "
      "AND s.source_kind='event' LIMIT 1");
  source.bind(1, candidate.candidate_id);
  if (!source.step())
    return;
  if (source.column_is_null(3)) {
    candidate.visible = false;
    return;
  }
  apply_event_source(connection, policy, source.column_text(0),
                     source.column_text(1),
                     DiscordSnowflake::parse(source.column_text(2)),
                     DiscordSnowflake::parse(source.column_text(3)), now_ms,
                     candidate, include_context);
}

void refresh_chronicle_source(SqliteConnection &connection,
                              const AppearancePolicy &policy,
                              const std::int64_t now_ms,
                              AppearanceCandidate &candidate) {
  auto source = connection.prepare(
      "SELECT s.source_id,c.guild_id,c.channel_id FROM "
      "appearance_candidate_source s JOIN appearance_candidate c ON "
      "c.candidate_id=s.candidate_id WHERE s.candidate_id=? AND "
      "s.source_kind='chronicle_entry' LIMIT 1");
  source.bind(1, candidate.candidate_id);
  if (!source.step())
    return;
  apply_event_source(
      connection, policy, "chronicle.entry_canonized.v1", source.column_text(0),
      DiscordSnowflake::parse(source.column_text(1)),
      DiscordSnowflake::parse(source.column_text(2)), now_ms, candidate, false);
}

void update_channel_state(SqliteConnection &connection,
                          const AppearanceMessageObservation &observation) {
  auto ensure = connection.prepare(
      "INSERT OR IGNORE INTO appearance_channel_state("
      "guild_id,channel_id,human_message_count,last_sanguinius_at_ms,"
      "human_message_count_at_last_sanguinius,last_author_is_sanguinius) "
      "VALUES(?,?,0,NULL,0,0)");
  ensure.bind(1, observation.guild_id.str());
  ensure.bind(2, observation.channel_id.str());
  ensure.execute();
  if (observation.author_is_bot) {
    auto bot = connection.prepare(
        "UPDATE appearance_channel_state SET last_sanguinius_at_ms=?,"
        "human_message_count_at_last_sanguinius=human_message_count,"
        "last_author_is_sanguinius=1 WHERE guild_id=? AND channel_id=?");
    bot.bind(1, observation.observed_at_ms);
    bot.bind(2, observation.guild_id.str());
    bot.bind(3, observation.channel_id.str());
    bot.execute();
    return;
  }
  auto human = connection.prepare(
      "UPDATE appearance_channel_state SET "
      "human_message_count=human_message_count+1,last_author_is_sanguinius=0 "
      "WHERE guild_id=? AND channel_id=?");
  human.bind(1, observation.guild_id.str());
  human.bind(2, observation.channel_id.str());
  human.execute();
}

[[nodiscard]] std::int64_t
current_human_message_count(SqliteConnection &connection,
                            const std::string_view guild_id,
                            const std::string_view channel_id) {
  auto state = connection.prepare(
      "SELECT human_message_count FROM appearance_channel_state WHERE "
      "guild_id=? AND channel_id=?");
  state.bind(1, guild_id);
  state.bind(2, channel_id);
  return state.step() ? state.column_int64(0) : 0;
}

void apply_channel_state(SqliteConnection &connection,
                         const AppearancePolicy &policy,
                         const std::string_view guild_id,
                         const std::string_view channel_id,
                         const std::int64_t now_ms,
                         AppearanceCandidate &candidate) {
  auto state = connection.prepare(
      "SELECT human_message_count,last_sanguinius_at_ms,"
      "human_message_count_at_last_sanguinius,last_author_is_sanguinius "
      "FROM appearance_channel_state WHERE guild_id=? AND channel_id=?");
  state.bind(1, guild_id);
  state.bind(2, channel_id);
  if (state.step()) {
    candidate.human_message_count = state.column_int64(0);
    candidate.bot_last_meaningful_speaker =
        candidate.bot_last_meaningful_speaker || state.column_int64(3) != 0;
    if (!state.column_is_null(1)) {
      candidate.bot_speech_age_ms =
          std::max<std::int64_t>(0, now_ms - state.column_int64(1));
      const auto at_last_bot = state.column_int64(2);
      candidate.human_messages_since_bot =
          static_cast<std::size_t>(std::max<std::int64_t>(
              0, candidate.human_message_count - at_last_bot));
    } else {
      candidate.bot_speech_age_ms.reset();
      candidate.human_messages_since_bot =
          static_cast<std::size_t>(candidate.human_message_count);
    }
  }

  if (candidate.deterministic_serious_category)
    return;
  auto recent = connection.prepare(
      "SELECT serious_category,excerpt FROM appearance_message_activity WHERE "
      "guild_id=? AND "
      "channel_id=? AND author_is_bot=0 AND observed_at_ms>=? AND "
      "observed_at_ms<=? AND policy_version=? "
      "ORDER BY observed_at_ms DESC,message_id DESC LIMIT ?");
  recent.bind(1, guild_id);
  recent.bind(2, channel_id);
  recent.bind(3, std::max<std::int64_t>(0, now_ms - policy.activity_window_ms));
  recent.bind(4, now_ms);
  recent.bind(5, policy.policy_version);
  recent.bind(6, static_cast<std::int64_t>(policy.activity_maximum_rows));
  while (recent.step() && !candidate.deterministic_serious_category) {
    candidate.deterministic_serious_category =
        recent.column_is_null(0)
            ? detect_serious_context(policy, recent.column_text(1))
            : std::optional<std::string>{recent.column_text(0)};
  }
}

void refresh_channel_activity(SqliteConnection &connection,
                              const AppearancePolicy &policy,
                              const std::int64_t now_ms,
                              AppearanceCandidate &candidate) {
  auto candidate_scope = connection.prepare(
      "SELECT guild_id,channel_id FROM appearance_candidate WHERE "
      "candidate_id=?");
  candidate_scope.bind(1, candidate.candidate_id);
  if (!candidate_scope.step()) {
    candidate.correct_scope = false;
    return;
  }
  const auto guild_id = candidate_scope.column_text(0);
  const auto channel_id = candidate_scope.column_text(1);
  apply_channel_state(connection, policy, guild_id, channel_id, now_ms,
                      candidate);
}

void insert_candidate(SqliteConnection &connection,
                      const AppearancePolicy &policy,
                      const AppearanceCandidate &candidate,
                      const DiscordSnowflake guild,
                      const DiscordSnowflake channel,
                      const std::string_view deduplication_key,
                      const std::vector<CandidateSource> &sources) {
  auto insert = connection.prepare(
      "INSERT INTO "
      "appearance_candidate(candidate_id,candidate_type,guild_id,channel_id,"
      "policy_version,deduplication_key,context_json,theme_key,owner_"
      "simulation,created_at_ms,expires_at_ms,context_expires_at_ms)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(deduplication_key) DO "
      "NOTHING");
  insert.bind(1, candidate.candidate_id);
  insert.bind(2, appearance_candidate_type_name(candidate.type));
  insert.bind(3, guild.str());
  insert.bind(4, channel.str());
  insert.bind(5, policy.policy_version);
  insert.bind(6, deduplication_key);
  insert.bind(7, candidate_json(candidate));
  if (candidate.theme_key)
    insert.bind(8, *candidate.theme_key);
  else
    insert.bind_null(8);
  insert.bind(9, static_cast<std::int64_t>(candidate.owner_simulation));
  insert.bind(10, candidate.created_at_ms);
  insert.bind(11, candidate.expires_at_ms);
  insert.bind(12, candidate.created_at_ms + policy.activity_retention_ms);
  insert.execute();
  if (connection.changes() == 0)
    return;
  std::size_t rank{};
  for (const auto &[source_kind, source_id] : sources) {
    auto row =
        connection.prepare("INSERT INTO "
                           "appearance_candidate_source(candidate_id,source_"
                           "kind,source_id,source_rank) VALUES(?,?,?,?)");
    row.bind(1, candidate.candidate_id);
    row.bind(2, source_kind);
    row.bind(3, source_id);
    row.bind(4, static_cast<std::int64_t>(rank++));
    row.execute();
  }
  for (const auto actor : candidate.actors) {
    auto row = connection.prepare(
        "INSERT OR IGNORE INTO "
        "appearance_candidate_actor(candidate_id,user_id) VALUES(?,?)");
    row.bind(1, candidate.candidate_id);
    row.bind(2, actor.str());
    row.execute();
  }
}

void mark_evaluation_started(SqliteConnection &connection,
                             const std::string_view candidate_id,
                             const std::int64_t now_ms) {
  auto update = connection.prepare(
      "UPDATE appearance_candidate SET evaluation_started_at_ms="
      "CASE WHEN ?<created_at_ms THEN created_at_ms ELSE ? END "
      "WHERE candidate_id=? AND evaluation_started_at_ms IS NULL");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, candidate_id);
  update.execute();
}

[[nodiscard]] Json gate_json(const AppearanceEvaluation &evaluation) {
  auto json = Json::array();
  for (const auto &item : evaluation.gates)
    json.push_back({{"name", item.name}, {"passed", item.passed}});
  return json;
}

[[nodiscard]] Json score_json(const AppearanceEvaluation &evaluation) {
  auto json = Json::array();
  for (const auto &item : evaluation.score_components)
    json.push_back({{"name", item.name}, {"points", item.points}});
  return json;
}

void append_candidate_event(SqliteConnection &connection,
                            const std::string &event_id,
                            const AppearanceCandidate &candidate,
                            const DiscordSnowflake guild,
                            const DiscordSnowflake channel,
                            const std::string_view correlation,
                            const std::int64_t recorded_at_ms) {
  const auto event = EventJournalEntry{
      .event_id = event_id,
      .event_type = "appearance.candidate_created.v1",
      .aggregate_type = "appearance_candidate",
      .aggregate_id = candidate.candidate_id,
      .actor_user_id = candidate.actors.empty()
                           ? std::nullopt
                           : std::optional{candidate.actors.front()},
      .guild_id = guild,
      .channel_id = channel,
      .source_message_id = std::nullopt,
      .occurred_at_ms = candidate.created_at_ms,
      .recorded_at_ms = recorded_at_ms,
      .correlation_id = std::string{correlation},
      .causation_id = std::nullopt,
      .idempotency_key = "appearance.candidate:" + candidate.candidate_id,
      .payload_json = Json{{"candidate_type",
                            appearance_candidate_type_name(candidate.type)}}
                          .dump()};
  static_cast<void>(detail::insert_event_uncommitted(connection, event));
}

[[nodiscard]] std::pair<DiscordSnowflake, DiscordSnowflake>
scope(SqliteConnection &connection) {
  auto query = connection.prepare(
      "SELECT guild_id,primary_channel_id FROM guild_config WHERE singleton=1");
  if (!query.step())
    throw std::runtime_error{"Guild scope is unavailable."};
  return {DiscordSnowflake::parse(query.column_text(0)),
          DiscordSnowflake::parse(query.column_text(1))};
}

[[nodiscard]] std::string derived_uuid(std::string value, const int offset) {
  if (!valid_uuid_v4(value))
    throw std::runtime_error{"Source event ID is invalid."};
  constexpr std::string_view digits{"0123456789abcdef"};
  const auto position = digits.find(value.back());
  if (position == std::string_view::npos)
    throw std::runtime_error{"Source event ID is invalid."};
  value.back() =
      digits[(position + static_cast<std::size_t>(offset)) % digits.size()];
  return value;
}

void apply_budget_gates(SqliteConnection &connection,
                        const AppearancePolicy &policy,
                        AppearanceCandidate &candidate,
                        const std::int64_t now_ms) {
  const auto cutoff =
      std::max<std::int64_t>(0, now_ms - policy.budget_window_ms);
  auto recent =
      connection.prepare("SELECT count(*) FROM appearance_decision "
                         "WHERE action='hypothetical' AND finalized_at_ms>?");
  recent.bind(1, cutoff);
  if (!recent.step())
    throw std::runtime_error{"Unable to inspect appearance budget."};
  candidate.budget_available =
      static_cast<std::size_t>(recent.column_int64(0)) < policy.budget_maximum;
  auto previous = connection.prepare(
      "SELECT finalized_at_ms,human_message_count FROM appearance_decision "
      "WHERE action='hypothetical' AND finalized_at_ms>? "
      "ORDER BY finalized_at_ms DESC,decision_id DESC LIMIT 1");
  previous.bind(1, cutoff);
  if (previous.step()) {
    const auto last = previous.column_int64(0);
    candidate.gap_available = now_ms - last >= policy.minimum_gap_ms;
    const auto prior_count = previous.column_int64(1);
    candidate.messages_after_previous =
        candidate.human_message_count >= prior_count &&
        static_cast<std::size_t>(candidate.human_message_count - prior_count) >=
            policy.human_messages_after_previous;
  }
  if (candidate.theme_key) {
    auto theme = connection.prepare(
        "SELECT 1 FROM appearance_decision d JOIN appearance_candidate c ON "
        "c.candidate_id=d.candidate_id "
        "WHERE d.action='hypothetical' AND c.theme_key=? AND "
        "d.finalized_at_ms>? LIMIT 1");
    theme.bind(1, *candidate.theme_key);
    theme.bind(
        2, std::max<std::int64_t>(0, now_ms - policy.same_theme_cooldown_ms));
    candidate.theme_available = !theme.step();
  }
  for (const auto &memory : candidate.memory_context) {
    auto used = connection.prepare(
        "SELECT 1 FROM appearance_decision_memory dm JOIN appearance_decision "
        "d ON d.decision_id=dm.decision_id "
        "WHERE dm.memory_id=? AND dm.used_by_model=1 "
        "AND d.action='hypothetical' AND "
        "d.finalized_at_ms>? LIMIT 1");
    used.bind(1, memory.memory_id);
    used.bind(
        2, std::max<std::int64_t>(0, now_ms - policy.same_memory_cooldown_ms));
    if (used.step()) {
      candidate.memory_available = false;
      break;
    }
  }
}

void refresh_persistent_gates(SqliteConnection &connection,
                              const AppearancePolicy &policy,
                              AppearanceCandidate &candidate,
                              const std::int64_t now_ms) {
  const bool initial_budget = candidate.budget_available;
  const bool initial_gap = candidate.gap_available;
  const bool initial_messages = candidate.messages_after_previous;
  const bool initial_theme = candidate.theme_available;
  const bool initial_memory = candidate.memory_available;
  candidate.budget_available = true;
  candidate.gap_available = true;
  candidate.messages_after_previous = true;
  candidate.theme_available = true;
  candidate.memory_available = true;
  refresh_channel_activity(connection, policy, now_ms, candidate);
  apply_theme_history(connection, candidate, now_ms);
  apply_budget_gates(connection, policy, candidate, now_ms);
  candidate.budget_available = initial_budget && candidate.budget_available;
  candidate.gap_available = initial_gap && candidate.gap_available;
  candidate.messages_after_previous =
      initial_messages && candidate.messages_after_previous;
  candidate.theme_available = initial_theme && candidate.theme_available;
  candidate.memory_available = initial_memory && candidate.memory_available;

  bool current_consent = true;
  bool current_visibility = true;
  bool current_quiet = false;
  for (const auto actor : candidate.actors) {
    auto preference = connection.prepare(
        "SELECT appearance_callback_opt_in,quiet_until_ms FROM user_preference "
        "WHERE user_id=?");
    preference.bind(1, actor.str());
    if (!preference.step() || preference.column_int64(0) == 0) {
      current_consent = false;
      continue;
    }
    if (!preference.column_is_null(1) && preference.column_int64(1) > now_ms)
      current_quiet = true;
  }
  for (const auto &memory : candidate.memory_context) {
    if (!candidate.deterministic_serious_category)
      candidate.deterministic_serious_category =
          detect_serious_context(policy, memory.text);
    auto available = connection.prepare(
        "SELECT 1 FROM memory m WHERE m.memory_id=? AND m.revision=? "
        "AND m.status='confirmed' AND m.visibility='shared' "
        "AND m.sensitivity='ordinary' "
        "AND (m.expires_at_ms IS NULL OR m.expires_at_ms>?) AND NOT EXISTS("
        "SELECT 1 FROM memory_subject s JOIN user_preference p "
        "ON p.user_id=s.subject_id WHERE s.memory_id=m.memory_id "
        "AND s.subject_type='user' AND (p.chronicle_opt_in=0 OR "
        "p.memory_callback_opt_in=0 OR p.appearance_callback_opt_in=0 OR "
        "(p.quiet_until_ms IS NOT NULL AND p.quiet_until_ms>?)))");
    available.bind(1, memory.memory_id);
    available.bind(2, static_cast<std::int64_t>(memory.revision));
    available.bind(3, now_ms);
    available.bind(4, now_ms);
    if (!available.step()) {
      current_consent = false;
      current_visibility = false;
    }
  }
  candidate.consented = candidate.consented && current_consent;
  candidate.visible = candidate.visible && current_visibility;
  candidate.manual_quiet = candidate.manual_quiet || current_quiet;
  refresh_event_source(connection, policy, now_ms, candidate);
  refresh_chronicle_source(connection, policy, now_ms, candidate);
}

void insert_decision_memories(
    SqliteConnection &connection, const std::string_view decision_id,
    const AppearanceCandidate &candidate,
    const std::optional<AppearanceModelResult> &result) {
  std::size_t rank{};
  for (const auto &memory : candidate.memory_context) {
    const bool used = result && std::ranges::find(result->memory_ids_used,
                                                  memory.memory_id) !=
                                    result->memory_ids_used.end();
    auto insert = connection.prepare(
        "INSERT INTO "
        "appearance_decision_memory(decision_id,memory_id,memory_revision,"
        "selection_rank,used_by_model) "
        "VALUES(?,?,?,?,?) ON CONFLICT(decision_id,memory_id) DO UPDATE SET "
        "used_by_model=excluded.used_by_model");
    insert.bind(1, decision_id);
    insert.bind(2, memory.memory_id);
    insert.bind(3, static_cast<std::int64_t>(memory.revision));
    insert.bind(4, static_cast<std::int64_t>(rank++));
    insert.bind(5, static_cast<std::int64_t>(used));
    insert.execute();
  }
}

[[nodiscard]] std::string
final_reason(const AppearanceEvaluation &evaluation,
             const std::string_view model_status,
             const std::optional<AppearanceModelResult> &result) {
  if (!evaluation.eligible_for_model)
    return evaluation.reason;
  if (!result)
    return std::string{model_status};
  if (result->serious_context)
    return "model_serious_context";
  if (!result->should_speak)
    return "model_declined";
  return "hypothetical";
}

[[nodiscard]] std::vector<std::string>
serious_categories(const AppearanceCandidate &candidate,
                   const std::optional<AppearanceModelResult> &result) {
  std::set<std::string, std::less<>> unique;
  if (candidate.deterministic_serious_category)
    unique.insert(*candidate.deterministic_serious_category);
  if (result)
    unique.insert(result->serious_categories.begin(),
                  result->serious_categories.end());
  return {unique.begin(), unique.end()};
}

void append_decision_event(SqliteConnection &connection,
                           const std::string &event_id,
                           const AppearanceCandidate &candidate,
                           const std::string_view decision_id,
                           const std::string_view action,
                           const std::string_view reason,
                           const std::int64_t now_ms) {
  const auto [guild, channel] = scope(connection);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      EventJournalEntry{
          .event_id = event_id,
          .event_type = "appearance.decision_recorded.v1",
          .aggregate_type = "appearance_decision",
          .aggregate_id = std::string{decision_id},
          .actor_user_id = candidate.actors.empty()
                               ? std::nullopt
                               : std::optional{candidate.actors.front()},
          .guild_id = guild,
          .channel_id = channel,
          .source_message_id = std::nullopt,
          .occurred_at_ms = now_ms,
          .recorded_at_ms = now_ms,
          .correlation_id = "appearance:" + candidate.candidate_id,
          .causation_id = std::nullopt,
          .idempotency_key = "appearance.decision:" + std::string{decision_id},
          .payload_json =
              Json{{"action", action}, {"reason", reason}}.dump()}));
}

} // namespace

SqliteAppearanceRepository::SqliteAppearanceRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite appearance context is required."};
}

void SqliteAppearanceRepository::register_policy(const AppearancePolicy &policy,
                                                 const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto insert = connection.prepare(
      "INSERT INTO "
      "appearance_policy_snapshot(policy_version,schema_version,canonical_json,"
      "created_at_ms) "
      "VALUES(?,?,?,?) ON CONFLICT(policy_version) DO NOTHING");
  insert.bind(1, policy.policy_version);
  insert.bind(2, static_cast<std::int64_t>(policy.schema_version));
  insert.bind(3, policy.canonical_json);
  insert.bind(4, now_ms);
  insert.execute();
  auto verify =
      connection.prepare("SELECT schema_version,canonical_json FROM "
                         "appearance_policy_snapshot WHERE policy_version=?");
  verify.bind(1, policy.policy_version);
  if (!verify.step() || verify.column_int64(0) != policy.schema_version ||
      verify.column_text(1) != policy.canonical_json)
    throw std::runtime_error{"Appearance policy version collision."};
  auto schedule = connection.prepare(
      "INSERT INTO "
      "scheduled_job(job_id,job_type,aggregate_type,aggregate_id,payload_json,"
      "due_at_ms,state,attempt_count,max_attempts,idempotency_key,created_at_"
      "ms,updated_at_ms) "
      "VALUES(?,?,?,?,?,?,'pending',0,20,?,?,?) ON CONFLICT(idempotency_key) "
      "DO NOTHING");
  const auto enqueue_recurring = [&](const std::string_view id,
                                     const std::string_view type,
                                     const std::string_view key) {
    schedule.reset();
    schedule.bind(1, id);
    schedule.bind(2, type);
    schedule.bind(3, "appearance_policy");
    schedule.bind(4, policy.policy_version);
    schedule.bind(5, Json{{"payload_version", 1},
                          {"policy_version", policy.policy_version}}
                         .dump());
    schedule.bind(6, now_ms);
    schedule.bind(7, key);
    schedule.bind(8, now_ms);
    schedule.bind(9, now_ms);
    schedule.execute();
    auto refresh =
        connection.prepare("UPDATE scheduled_job SET "
                           "aggregate_id=?,payload_json=?,updated_at_ms=? "
                           "WHERE idempotency_key=? AND job_type=?");
    refresh.bind(1, policy.policy_version);
    refresh.bind(2, Json{{"payload_version", 1},
                         {"policy_version", policy.policy_version}}
                        .dump());
    refresh.bind(3, now_ms);
    refresh.bind(4, key);
    refresh.bind(5, type);
    refresh.execute();
  };
  enqueue_recurring("00000000-0000-4000-8000-000000000701",
                    appearance_scan_job_type, "appearance.scan.recurring.v1");
  enqueue_recurring("00000000-0000-4000-8000-000000000702",
                    appearance_purge_job_type, "appearance.purge.recurring.v1");
  transaction.commit();
}

void SqliteAppearanceRepository::activate_mode(const AppearanceMode mode,
                                               const std::int64_t now_ms) {
  if (now_ms < 0)
    throw std::invalid_argument{"Invalid appearance mode activation time."};
  const auto mode_name = appearance_mode_name(mode);
  if (mode_name != "off" && mode_name != "dry_run")
    throw std::invalid_argument{"Invalid appearance mode."};

  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto current = connection.prepare(
      "SELECT mode FROM appearance_mode_state WHERE singleton=1");
  const auto previous_mode =
      current.step() ? current.column_text(0) : std::string{"off"};
  if (mode == AppearanceMode::off || previous_mode == "off") {
    auto discard = connection.prepare(
        "UPDATE appearance_event_observation SET extraction_result='mode_off',"
        "processed_at_ms=? WHERE processed_at_ms IS NULL");
    discard.bind(1, now_ms);
    discard.execute();
  }
  auto update = connection.prepare(
      "INSERT INTO appearance_mode_state(singleton,mode,updated_at_ms) "
      "VALUES(1,?,?) ON CONFLICT(singleton) DO UPDATE SET "
      "mode=excluded.mode,updated_at_ms=excluded.updated_at_ms");
  update.bind(1, mode_name);
  update.bind(2, now_ms);
  update.execute();
  transaction.commit();
}

AppearancePolicy
SqliteAppearanceRepository::load_policy(const std::string_view policy_version) {
  const std::scoped_lock lock{context_->mutex()};
  return load_policy_snapshot(context_->connection(), policy_version);
}

std::size_t SqliteAppearanceRepository::abandon_prior_instance_attempts(
    const std::string_view instance_id, const std::int64_t now_ms,
    PersistentIdGenerator &ids) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto pending = connection.prepare(
      "SELECT decision_id,candidate_id FROM appearance_decision "
      "WHERE state='model_pending' AND application_instance_id<>? "
      "ORDER BY created_at_ms,decision_id");
  pending.bind(1, instance_id);
  std::vector<std::pair<std::string, std::string>> decisions;
  while (pending.step())
    decisions.emplace_back(pending.column_text(0), pending.column_text(1));
  for (const auto &[decision_id, candidate_id] : decisions) {
    auto update = connection.prepare(
        "UPDATE appearance_decision SET "
        "revision=2,state='final',action='reject',"
        "reason='model_abandoned_restart',model_status='model_abandoned_"
        "restart',"
        "finalized_at_ms=? WHERE decision_id=? AND state='model_pending'");
    update.bind(1, now_ms);
    update.bind(2, decision_id);
    update.execute();
    if (connection.changes() != 0)
      append_decision_event(
          connection, ids.next_id(), read_candidate(connection, candidate_id),
          decision_id, "reject", "model_abandoned_restart", now_ms);
  }
  const auto changed = decisions.size();
  connection.execute(
      "UPDATE appearance_candidate SET evaluation_started_at_ms=NULL "
      "WHERE evaluation_started_at_ms IS NOT NULL AND NOT EXISTS("
      "SELECT 1 FROM appearance_decision d "
      "WHERE d.candidate_id=appearance_candidate.candidate_id)");
  transaction.commit();
  return changed;
}

bool SqliteAppearanceRepository::set_callback_consent(
    const DiscordSnowflake user_id, const bool enabled,
    const std::int64_t now_ms, std::string event_id,
    std::string idempotency_key, std::string correlation_id) {
  if (!user_id.is_set() || now_ms < 0 || !valid_uuid_v4(event_id) ||
      idempotency_key.empty() || idempotency_key.size() > 160 ||
      correlation_id.empty() || correlation_id.size() > 160)
    throw std::invalid_argument{
        "Invalid appearance callback preference request."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT event_type,aggregate_id,payload_json FROM event_journal "
      "WHERE idempotency_key=?");
  replay.bind(1, idempotency_key);
  if (replay.step()) {
    const auto payload = Json::parse(replay.column_text(2));
    const auto event_type = replay.column_text(0);
    if ((event_type != "appearance.callback_consent_changed.v1" &&
         event_type != "appearance.callback_consent_unchanged.v1") ||
        replay.column_text(1) != user_id.str() || !payload.is_object() ||
        payload.size() != 1 || !payload.contains("enabled") ||
        !payload.at("enabled").is_boolean() ||
        payload.at("enabled").get<bool>() != enabled)
      throw std::runtime_error{
          "Appearance callback preference replay conflicts with prior input."};
    transaction.commit();
    return false;
  }
  auto preference = connection.prepare(
      "SELECT appearance_callback_opt_in FROM user_preference WHERE user_id=?");
  preference.bind(1, user_id.str());
  if (!preference.step())
    throw std::runtime_error{"Appearance callback preference is unavailable."};
  const bool changed = (preference.column_int64(0) != 0) != enabled;
  if (changed) {
    auto update = connection.prepare(
        "UPDATE user_preference SET appearance_callback_opt_in=?,"
        "updated_at_ms=? WHERE user_id=?");
    update.bind(1, static_cast<std::int64_t>(enabled));
    update.bind(2, now_ms);
    update.bind(3, user_id.str());
    update.execute();
  }
  const auto [guild, channel] = scope(connection);
  static_cast<void>(detail::insert_event_uncommitted(
      connection,
      EventJournalEntry{
          .event_id = std::move(event_id),
          .event_type = changed ? "appearance.callback_consent_changed.v1"
                                : "appearance.callback_consent_unchanged.v1",
          .aggregate_type = "user_preference",
          .aggregate_id = user_id.str(),
          .actor_user_id = user_id,
          .guild_id = guild,
          .channel_id = channel,
          .source_message_id = std::nullopt,
          .occurred_at_ms = now_ms,
          .recorded_at_ms = now_ms,
          .correlation_id = std::move(correlation_id),
          .causation_id = std::nullopt,
          .idempotency_key = std::move(idempotency_key),
          .payload_json = Json{{"enabled", enabled}}.dump()}));
  transaction.commit();
  return changed;
}

std::optional<AppearanceCandidate> SqliteAppearanceRepository::observe_message(
    const AppearancePolicy &policy,
    const AppearanceMessageObservation &observation, std::string candidate_id,
    std::string event_id) {
  if (!valid_uuid_v4(candidate_id) || !valid_uuid_v4(event_id) ||
      !observation.message_id.is_set() ||
      !observation.author_user_id.is_set() || observation.observed_at_ms < 0 ||
      observation.correlation_id.empty())
    throw std::invalid_argument{"Invalid appearance message observation."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto seen = connection.prepare(
      "INSERT INTO appearance_message_seen(message_id,first_observed_at_ms) "
      "VALUES(?,?) ON CONFLICT(message_id) DO NOTHING");
  seen.bind(1, observation.message_id.str());
  seen.bind(2, observation.observed_at_ms);
  seen.execute();
  const bool fresh_message = connection.changes() != 0;

  if (fresh_message) {
    const auto serious_category =
        detect_serious_context(policy, observation.excerpt);
    auto insert =
        connection.prepare("INSERT INTO "
                           "appearance_message_activity(message_id,guild_id,"
                           "channel_id,policy_version,author_user_"
                           "id,author_is_bot,excerpt,serious_category,observed_"
                           "at_ms,expires_at_ms,"
                           "correlation_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    insert.bind(1, observation.message_id.str());
    insert.bind(2, observation.guild_id.str());
    insert.bind(3, observation.channel_id.str());
    insert.bind(4, policy.policy_version);
    insert.bind(5, observation.author_user_id.str());
    insert.bind(6, static_cast<std::int64_t>(observation.author_is_bot));
    insert.bind(7, bounded_utf8(observation.excerpt,
                                policy.activity_maximum_utf8_bytes_per_row));
    if (serious_category)
      insert.bind(8, *serious_category);
    else
      insert.bind_null(8);
    insert.bind(9, observation.observed_at_ms);
    insert.bind(10, observation.observed_at_ms + policy.activity_retention_ms);
    insert.bind(11, observation.correlation_id);
    insert.execute();
    update_channel_state(connection, observation);
  }
  auto purge_old = connection.prepare(
      "DELETE FROM appearance_message_activity WHERE guild_id=? AND "
      "channel_id=? AND expires_at_ms<=?");
  purge_old.bind(1, observation.guild_id.str());
  purge_old.bind(2, observation.channel_id.str());
  purge_old.bind(3, observation.observed_at_ms);
  purge_old.execute();
  auto purge_rows = connection.prepare(
      "DELETE FROM appearance_message_activity WHERE message_id IN (SELECT "
      "message_id FROM appearance_message_activity WHERE guild_id=? AND "
      "channel_id=? ORDER BY observed_at_ms DESC,message_id DESC LIMIT -1 "
      "OFFSET ?)");
  purge_rows.bind(1, observation.guild_id.str());
  purge_rows.bind(2, observation.channel_id.str());
  purge_rows.bind(3, static_cast<std::int64_t>(policy.activity_maximum_rows));
  purge_rows.execute();
  for (;;) {
    auto bytes = connection.prepare(
        "SELECT coalesce(sum(length(CAST(excerpt AS BLOB))),0) FROM "
        "appearance_message_activity WHERE guild_id=? AND channel_id=?");
    bytes.bind(1, observation.guild_id.str());
    bytes.bind(2, observation.channel_id.str());
    if (!bytes.step() || static_cast<std::size_t>(bytes.column_int64(0)) <=
                             policy.activity_maximum_total_utf8_bytes)
      break;
    auto purge_bytes = connection.prepare(
        "DELETE FROM appearance_message_activity WHERE message_id=(SELECT "
        "message_id FROM appearance_message_activity WHERE guild_id=? AND "
        "channel_id=? ORDER BY observed_at_ms,message_id LIMIT 1)");
    purge_bytes.bind(1, observation.guild_id.str());
    purge_bytes.bind(2, observation.channel_id.str());
    purge_bytes.execute();
  }
  if (!fresh_message || observation.author_is_bot) {
    transaction.commit();
    return std::nullopt;
  }

  const auto since = std::max<std::int64_t>(0, observation.observed_at_ms -
                                                   policy.activity_window_ms);
  auto counts =
      connection.prepare("SELECT count(*),count(DISTINCT author_user_id) FROM "
                         "appearance_message_activity "
                         "WHERE author_is_bot=0 AND consumed_candidate_id IS "
                         "NULL AND guild_id=? AND channel_id=? AND "
                         "policy_version=? AND observed_at_ms>=? AND "
                         "observed_at_ms<=?");
  counts.bind(1, observation.guild_id.str());
  counts.bind(2, observation.channel_id.str());
  counts.bind(3, policy.policy_version);
  counts.bind(4, since);
  counts.bind(5, observation.observed_at_ms);
  if (!counts.step())
    throw std::runtime_error{"Unable to count appearance activity."};
  if (static_cast<std::size_t>(counts.column_int64(0)) <
      policy.human_messages_required) {
    transaction.commit();
    return std::nullopt;
  }

  auto rows = connection.prepare(
      "SELECT "
      "message_id,author_user_id,excerpt,observed_at_ms,serious_category FROM "
      "appearance_message_activity "
      "WHERE author_is_bot=0 AND consumed_candidate_id IS NULL AND "
      "guild_id=? AND channel_id=? AND policy_version=? AND observed_at_ms>=? "
      "AND observed_at_ms<=? "
      "ORDER BY observed_at_ms DESC,message_id DESC LIMIT ?");
  rows.bind(1, observation.guild_id.str());
  rows.bind(2, observation.channel_id.str());
  rows.bind(3, policy.policy_version);
  rows.bind(4, since);
  rows.bind(5, observation.observed_at_ms);
  rows.bind(6, static_cast<std::int64_t>(policy.human_messages_required));
  std::vector<std::string> source_ids;
  std::vector<
      std::tuple<std::int64_t, DiscordSnowflake, DiscordSnowflake, std::string>>
      ordered_excerpts;
  std::set<DiscordSnowflake> actor_set;
  std::string combined;
  std::optional<std::string> retained_serious_category;
  while (rows.step()) {
    source_ids.push_back(rows.column_text(0));
    actor_set.insert(DiscordSnowflake::parse(rows.column_text(1)));
    ordered_excerpts.emplace_back(
        rows.column_int64(3), DiscordSnowflake::parse(rows.column_text(0)),
        DiscordSnowflake::parse(rows.column_text(1)), rows.column_text(2));
    combined += rows.column_text(2);
    combined.push_back('\n');
    if (!retained_serious_category && !rows.column_is_null(4))
      retained_serious_category = rows.column_text(4);
  }
  std::ranges::reverse(source_ids);
  std::ranges::sort(ordered_excerpts);
  AppearanceCandidate candidate{};
  candidate.candidate_id = std::move(candidate_id);
  candidate.policy_version = policy.policy_version;
  candidate.type = AppearanceCandidateType::conversation;
  candidate.created_at_ms = observation.observed_at_ms;
  candidate.expires_at_ms = observation.observed_at_ms +
                            policy.candidate_expiry_ms.at("conversation");
  candidate.actors.assign(actor_set.begin(), actor_set.end());
  for (const auto &[ignored_time, ignored_message, ignored_actor, excerpt] :
       ordered_excerpts) {
    static_cast<void>(ignored_time);
    static_cast<void>(ignored_message);
    static_cast<void>(ignored_actor);
    candidate.excerpts.push_back(excerpt);
  }
  for (std::size_t index = 0;
       index + policy.alternating_turns <= ordered_excerpts.size(); ++index) {
    const auto last = index + policy.alternating_turns - 1;
    if (std::get<0>(ordered_excerpts[last]) -
            std::get<0>(ordered_excerpts[index]) >
        policy.alternating_window_ms)
      continue;
    bool alternating = true;
    for (std::size_t turn = index + 1; turn <= last; ++turn) {
      if (std::get<2>(ordered_excerpts[turn - 1]) ==
          std::get<2>(ordered_excerpts[turn])) {
        alternating = false;
        break;
      }
    }
    if (alternating) {
      candidate.alternating_turns = true;
      break;
    }
  }
  candidate.deterministic_serious_category =
      retained_serious_category ? retained_serious_category
                                : detect_serious_context(policy, combined);
  select_literal_memory_context(connection, policy, combined,
                                observation.observed_at_ms, candidate);
  if (!candidate.memory_context.empty())
    candidate.type = AppearanceCandidateType::recurrence;
  std::optional<std::string> chronicle_source_id;
  if (candidate.memory_context.empty()) {
    const auto tokens = literal_tokens(combined);
    auto entries = connection.prepare(
        "SELECT e.entry_id,e.title,e.body,COALESCE((SELECT "
        "group_concat(t.tag,' ') FROM chronicle_tag t WHERE "
        "t.entry_id=e.entry_id),'') FROM chronicle_entry e WHERE "
        "e.status='canon' "
        "AND e.visibility='shared' AND e.source_guild_id=? AND "
        "e.source_channel_id=? AND NOT EXISTS(SELECT 1 FROM "
        "chronicle_participant cp "
        "JOIN user_preference p ON p.user_id=cp.user_id WHERE "
        "cp.entry_id=e.entry_id "
        "AND (p.chronicle_opt_in=0 OR p.appearance_callback_opt_in=0)) "
        "ORDER BY e.occurred_at_ms DESC,e.entry_id LIMIT 32");
    entries.bind(1, observation.guild_id.str());
    entries.bind(2, observation.channel_id.str());
    while (entries.step()) {
      const auto source_text = entries.column_text(1) + " " +
                               entries.column_text(2) + " " +
                               entries.column_text(3);
      if (contains_literal_token(source_text, tokens)) {
        candidate.type = AppearanceCandidateType::recurrence;
        candidate.chronicle_specificity =
            policy.score_weights.at("chronicle_exact");
        candidate.recurrence_matches =
            matching_excerpt_count(candidate.excerpts, source_text);
        chronicle_source_id = entries.column_text(0);
        candidate.theme_key = "chronicle:" + *chronicle_source_id;
        candidate.source_context.push_back(bounded_utf8(
            source_text, policy.activity_maximum_utf8_bytes_per_row));
        if (!candidate.deterministic_serious_category)
          candidate.deterministic_serious_category =
              detect_serious_context(policy, source_text);
        break;
      }
    }
  }
  if (candidate.type == AppearanceCandidateType::recurrence)
    candidate.expires_at_ms = observation.observed_at_ms +
                              policy.candidate_expiry_ms.at("recurrence");
  if (candidate.chronicle_specificity == 0 && candidate.recurrence_matches == 0)
    candidate.uncertainty_penalty = policy.score_weights.at(
        candidate.alternating_turns ? "penalty_uncertain_generic"
                                    : "penalty_weak_generic");
  candidate.consented = true;
  candidate.manual_quiet = false;
  for (const auto actor : candidate.actors) {
    auto pref =
        connection.prepare("SELECT appearance_callback_opt_in,quiet_until_ms "
                           "FROM user_preference WHERE user_id=?");
    pref.bind(1, actor.str());
    if (!pref.step() || pref.column_int64(0) == 0)
      candidate.consented = false;
    // Re-query because step advances; keep the quiet gate conservative below.
    auto quiet = connection.prepare(
        "SELECT quiet_until_ms FROM user_preference WHERE user_id=?");
    quiet.bind(1, actor.str());
    if (quiet.step() && !quiet.column_is_null(0) &&
        quiet.column_int64(0) > observation.observed_at_ms)
      candidate.manual_quiet = true;
  }
  apply_channel_state(connection, policy, observation.guild_id.str(),
                      observation.channel_id.str(), observation.observed_at_ms,
                      candidate);
  if (!candidate.memory_context.empty())
    candidate.theme_key =
        "memory:" + candidate.memory_context.front().memory_id;
  if (!candidate.memory_context.empty()) {
    candidate.safe_summary = "Approved shared-memory recurrence across " +
                             std::to_string(candidate.excerpts.size()) +
                             " bounded public messages from " +
                             std::to_string(candidate.actors.size()) +
                             " humans.";
  } else if (chronicle_source_id) {
    candidate.safe_summary = "Approved shared-Chronicle recurrence across " +
                             std::to_string(candidate.excerpts.size()) +
                             " bounded public messages from " +
                             std::to_string(candidate.actors.size()) +
                             " humans.";
  } else {
    candidate.safe_summary = "Conversation activity across " +
                             std::to_string(candidate.excerpts.size()) +
                             " bounded public messages from " +
                             std::to_string(candidate.actors.size()) +
                             " humans.";
  }
  apply_theme_history(connection, candidate, observation.observed_at_ms);
  apply_budget_gates(connection, policy, candidate, observation.observed_at_ms);
  const auto dedup =
      std::string{appearance_candidate_type_name(candidate.type)} + ":" +
      source_ids.front() + ":" + source_ids.back();
  std::vector<CandidateSource> sources;
  sources.reserve(source_ids.size() + (chronicle_source_id ? 1U : 0U));
  for (const auto &source_id : source_ids)
    sources.emplace_back("message", source_id);
  if (chronicle_source_id)
    sources.emplace_back("chronicle_entry", *chronicle_source_id);
  insert_candidate(connection, policy, candidate, observation.guild_id,
                   observation.channel_id, dedup, sources);
  if (connection.changes() == 0) {
    transaction.commit();
    return std::nullopt;
  }
  mark_evaluation_started(connection, candidate.candidate_id,
                          observation.observed_at_ms);
  auto consume =
      connection.prepare("UPDATE appearance_message_activity SET "
                         "consumed_candidate_id=? WHERE message_id=?");
  for (const auto &source : source_ids) {
    consume.reset();
    consume.bind(1, candidate.candidate_id);
    consume.bind(2, source);
    consume.execute();
  }
  append_candidate_event(connection, event_id, candidate, observation.guild_id,
                         observation.channel_id, observation.correlation_id,
                         observation.observed_at_ms);
  transaction.commit();
  return candidate;
}

AppearanceCandidate SqliteAppearanceRepository::simulate(
    const AppearancePolicy &policy,
    const AppearanceSimulationRequest &request) {
  if (!valid_uuid_v4(request.candidate_id) ||
      !valid_uuid_v4(request.event_id) || request.fixture.empty() ||
      request.idempotency_key.empty() || request.now_ms <= 0)
    throw std::invalid_argument{"Invalid appearance simulation request."};
  const std::set<std::string, std::less<>> fixtures{
      "lively_game_night_banter",
      "one_person_quiet_channel",
      "bot_just_spoke",
      "quiet_hours",
      "manual_quiet",
      "sensitive_serious_conversation",
      "christianity",
      "chronicle_anniversary",
      "repeated_inside_joke_on_cooldown",
      "tarot_settlement",
      "opted_out_participant",
      "stale_candidate",
      "owner_dry_run"};
  if (!fixtures.contains(request.fixture))
    throw std::invalid_argument{"Unknown appearance fixture."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto [guild, channel] = scope(connection);
  AppearanceCandidate candidate{};
  candidate.candidate_id = request.candidate_id;
  candidate.policy_version = policy.policy_version;
  candidate.type = AppearanceCandidateType::simulation;
  candidate.created_at_ms = request.now_ms;
  candidate.expires_at_ms =
      request.now_ms + policy.candidate_expiry_ms.at("simulation");
  candidate.actors = {request.owner_user_id};
  candidate.excerpts = {"A bounded synthetic owner fixture."};
  candidate.safe_summary = "Owner simulation fixture: " + request.fixture + ".";
  candidate.owner_simulation = true;
  candidate.alternating_turns = true;
  candidate.recurrence_matches = 2;
  candidate.human_messages_since_bot = 8;
  candidate.human_message_count =
      current_human_message_count(connection, guild.str(), channel.str());
  auto owner_preference =
      connection.prepare("SELECT appearance_callback_opt_in,quiet_until_ms "
                         "FROM user_preference WHERE user_id=?");
  owner_preference.bind(1, request.owner_user_id.str());
  const bool owner_preference_found = owner_preference.step();
  candidate.consented =
      owner_preference_found && owner_preference.column_int64(0) != 0;
  if (owner_preference_found && !owner_preference.column_is_null(1) &&
      owner_preference.column_int64(1) > request.now_ms)
    candidate.manual_quiet = true;
  if (request.fixture == "one_person_quiet_channel")
    candidate.owner_simulation = false;
  else if (request.fixture == "bot_just_spoke")
    candidate.bot_last_meaningful_speaker = true;
  else if (request.fixture == "quiet_hours")
    candidate.configured_quiet = true;
  else if (request.fixture == "manual_quiet")
    candidate.manual_quiet = true;
  else if (request.fixture == "sensitive_serious_conversation")
    candidate.deterministic_serious_category = "death_serious_health";
  else if (request.fixture == "christianity")
    candidate.deterministic_serious_category = "christianity";
  else if (request.fixture == "chronicle_anniversary") {
    candidate.type = AppearanceCandidateType::anniversary;
    candidate.chronicle_specificity =
        policy.score_weights.at("chronicle_event");
  } else if (request.fixture == "repeated_inside_joke_on_cooldown")
    candidate.theme_available = false;
  else if (request.fixture == "tarot_settlement")
    candidate.source_enabled = false;
  else if (request.fixture == "opted_out_participant")
    candidate.consented = false;
  else if (request.fixture == "stale_candidate") {
    candidate.created_at_ms = request.now_ms == 0 ? 0 : request.now_ms - 1;
    candidate.expires_at_ms = request.now_ms;
  }
  apply_theme_history(connection, candidate, request.now_ms);
  apply_budget_gates(connection, policy, candidate, request.now_ms);
  const auto dedup = "simulation:" + request.idempotency_key;
  std::vector<CandidateSource> simulation_sources;
  simulation_sources.emplace_back("simulation", request.idempotency_key);
  insert_candidate(connection, policy, candidate, guild, channel, dedup,
                   simulation_sources);
  if (connection.changes() == 0) {
    auto existing =
        connection.prepare("SELECT candidate_id FROM appearance_candidate "
                           "WHERE deduplication_key=?");
    existing.bind(1, dedup);
    if (!existing.step())
      throw std::runtime_error{
          "Simulation idempotency conflict is inconsistent."};
    candidate = read_candidate(connection, existing.column_text(0));
  } else {
    append_candidate_event(connection, request.event_id, candidate, guild,
                           channel, request.correlation_id, request.now_ms);
  }
  mark_evaluation_started(connection, candidate.candidate_id, request.now_ms);
  transaction.commit();
  return candidate;
}

std::vector<AppearanceCandidate>
SqliteAppearanceRepository::scan_events(const AppearancePolicy &policy,
                                        const std::int64_t now_ms,
                                        const std::string_view instance_id) {
  static_cast<void>(instance_id);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto [guild, channel] = scope(connection);
  auto events = connection.prepare(
      "SELECT "
      "source_event_id,event_type,aggregate_id,actor_user_id,occurred_at_ms,"
      "guild_id,channel_id FROM appearance_event_observation "
      "WHERE processed_at_ms IS NULL ORDER BY recorded_at_ms,source_event_id "
      "LIMIT 32");
  struct Event {
    std::string id;
    std::string type;
    std::string aggregate;
    std::optional<DiscordSnowflake> actor;
    std::int64_t occurred{};
    DiscordSnowflake guild;
    std::optional<DiscordSnowflake> channel;
  };
  std::vector<Event> pending;
  while (events.step())
    pending.push_back(
        {events.column_text(0), events.column_text(1), events.column_text(2),
         events.column_is_null(3)
             ? std::nullopt
             : std::optional{DiscordSnowflake::parse(events.column_text(3))},
         events.column_int64(4), DiscordSnowflake::parse(events.column_text(5)),
         events.column_is_null(6)
             ? std::nullopt
             : std::optional{DiscordSnowflake::parse(events.column_text(6))}});
  std::vector<AppearanceCandidate> result;
  for (const auto &event : pending) {
    AppearanceCandidateType type;
    std::string expiry_key;
    int specificity{};
    if (event.type == "chronicle.entry_canonized.v1") {
      type = AppearanceCandidateType::chronicle_entry;
      expiry_key = "chronicle_entry";
      specificity = policy.score_weights.at("chronicle_event");
    } else if (event.type == "chronicle.session_started.v1") {
      type = AppearanceCandidateType::session_started;
      expiry_key = "session_started";
      specificity = policy.score_weights.at("session_event");
    } else if (event.type == "chronicle.session_completed.v1") {
      type = AppearanceCandidateType::session_completed;
      expiry_key = "session_completed";
      specificity = policy.score_weights.at("session_event");
    } else if (event.type == "chronicle.title_awarded.v1") {
      type = AppearanceCandidateType::title_awarded;
      expiry_key = "title_awarded";
      specificity = policy.score_weights.at("chronicle_event");
    } else if (event.type == "chronicle.anniversary_delivered.v1") {
      type = AppearanceCandidateType::anniversary;
      expiry_key = "anniversary";
      specificity = policy.score_weights.at("chronicle_event");
    } else {
      continue;
    }
    AppearanceCandidate candidate{};
    candidate.candidate_id = derived_uuid(event.id, 1);
    candidate.policy_version = policy.policy_version;
    candidate.type = type;
    candidate.created_at_ms = event.occurred;
    candidate.expires_at_ms =
        event.occurred + policy.candidate_expiry_ms.at(expiry_key);
    candidate.chronicle_specificity = specificity;
    candidate.safe_summary =
        "Chronicle event candidate (" +
        std::string{appearance_candidate_type_name(candidate.type)} + ").";
    candidate.correct_scope =
        event.guild == guild && event.channel && *event.channel == channel;
    auto active = connection.prepare(
        "SELECT message_id,author_user_id,excerpt,observed_at_ms "
        "FROM appearance_message_activity WHERE author_is_bot=0 "
        "AND guild_id=? AND channel_id=? AND policy_version=? AND "
        "observed_at_ms>=? AND observed_at_ms<=? "
        "ORDER BY observed_at_ms DESC,message_id DESC LIMIT ?");
    active.bind(1, guild.str());
    active.bind(2, channel.str());
    active.bind(3, policy.policy_version);
    active.bind(4,
                std::max<std::int64_t>(0, now_ms - policy.activity_window_ms));
    active.bind(5, now_ms);
    active.bind(6, static_cast<std::int64_t>(policy.activity_maximum_rows));
    std::vector<std::tuple<std::int64_t, DiscordSnowflake, std::string>>
        activity;
    std::set<DiscordSnowflake> active_actors;
    while (active.step()) {
      const auto actor = DiscordSnowflake::parse(active.column_text(1));
      active_actors.insert(actor);
      activity.emplace_back(active.column_int64(3), actor,
                            active.column_text(2));
    }
    candidate.actors.assign(active_actors.begin(), active_actors.end());
    std::ranges::reverse(activity);
    const auto excerpt_start =
        activity.size() > policy.maximum_public_excerpts
            ? activity.size() - policy.maximum_public_excerpts
            : 0;
    for (std::size_t index = excerpt_start; index < activity.size(); ++index) {
      const auto &[ignored_time, ignored_actor, excerpt] = activity[index];
      static_cast<void>(ignored_time);
      static_cast<void>(ignored_actor);
      candidate.excerpts.push_back(excerpt);
    }
    for (std::size_t index = 0;
         index + policy.alternating_turns <= activity.size(); ++index) {
      const auto last_index = index + policy.alternating_turns - 1;
      if (std::get<0>(activity[last_index]) - std::get<0>(activity[index]) >
          policy.alternating_window_ms)
        continue;
      bool alternating = true;
      for (std::size_t turn = index + 1; turn <= last_index; ++turn)
        alternating = alternating && std::get<1>(activity[turn - 1]) !=
                                         std::get<1>(activity[turn]);
      if (alternating) {
        candidate.alternating_turns = true;
        break;
      }
    }
    candidate.consented = true;
    for (const auto actor : candidate.actors) {
      auto preference =
          connection.prepare("SELECT appearance_callback_opt_in,quiet_until_ms "
                             "FROM user_preference WHERE user_id=?");
      preference.bind(1, actor.str());
      if (!preference.step() || preference.column_int64(0) == 0) {
        candidate.consented = false;
      } else if (!preference.column_is_null(1) &&
                 preference.column_int64(1) > now_ms) {
        candidate.manual_quiet = true;
      }
    }
    apply_event_source(connection, policy, event.type, event.aggregate, guild,
                       channel, now_ms, candidate, true);
    apply_channel_state(connection, policy, guild.str(), channel.str(), now_ms,
                        candidate);
    apply_theme_history(connection, candidate, now_ms);
    apply_budget_gates(connection, policy, candidate, now_ms);
    const auto dedup = "event:" + event.id;
    std::vector<CandidateSource> event_sources;
    event_sources.emplace_back("event", event.id);
    insert_candidate(connection, policy, candidate, guild, channel, dedup,
                     event_sources);
    const bool inserted = connection.changes() != 0;
    auto update =
        connection.prepare("UPDATE appearance_event_observation SET "
                           "extraction_result=?,candidate_id=?,processed_at_ms="
                           "? WHERE source_event_id=?");
    update.bind(1, now_ms >= candidate.expires_at_ms ? "expired"
                                                     : "candidate_created");
    if (inserted)
      update.bind(2, candidate.candidate_id);
    else
      update.bind_null(2);
    update.bind(3, now_ms);
    update.bind(4, event.id);
    update.execute();
    if (inserted) {
      append_candidate_event(connection, derived_uuid(event.id, 2), candidate,
                             guild, channel, "appearance:event_scan", now_ms);
      mark_evaluation_started(connection, candidate.candidate_id, now_ms);
      result.push_back(std::move(candidate));
    }
  }
  auto recoverable =
      connection.prepare("SELECT c.candidate_id FROM appearance_candidate c "
                         "WHERE c.evaluation_started_at_ms IS NULL "
                         "AND NOT EXISTS(SELECT 1 FROM appearance_decision d "
                         "WHERE d.candidate_id=c.candidate_id) "
                         "ORDER BY c.created_at_ms,c.candidate_id LIMIT ?");
  recoverable.bind(1, static_cast<std::int64_t>(32 - result.size()));
  std::vector<std::string> recoverable_ids;
  while (recoverable.step())
    recoverable_ids.push_back(recoverable.column_text(0));
  for (const auto &candidate_id : recoverable_ids) {
    mark_evaluation_started(connection, candidate_id, now_ms);
    auto candidate = read_candidate(connection, candidate_id);
    const auto candidate_policy =
        load_policy_snapshot(connection, candidate.policy_version);
    refresh_event_source(connection, candidate_policy, now_ms, candidate, true);
    result.push_back(std::move(candidate));
  }
  transaction.commit();
  return result;
}

bool SqliteAppearanceRepository::record_final(
    const AppearancePolicy &policy, const AppearanceMode mode,
    const AppearanceCandidate &candidate,
    const AppearanceEvaluation &evaluation, std::string decision_id,
    std::string event_id, const std::string_view instance_id,
    std::string model_status, std::optional<AppearanceModelResult> model_result,
    const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  static_cast<void>(evaluation);
  if (!candidate.policy_version.empty() &&
      candidate.policy_version != policy.policy_version)
    throw std::runtime_error{"Appearance candidate policy mismatch."};
  auto final_candidate = candidate;
  refresh_persistent_gates(connection, policy, final_candidate, now_ms);
  const auto final_evaluation =
      evaluate_appearance(policy, mode, final_candidate, now_ms);
  const auto reason =
      final_reason(final_evaluation, model_status, model_result);
  const auto action = reason == "hypothetical" ? "hypothetical" : "reject";
  const auto categories = serious_categories(final_candidate, model_result);
  auto insert = connection.prepare(
      "INSERT INTO "
      "appearance_decision(decision_id,candidate_id,policy_version,application_"
      "instance_id,revision,state,action,reason,gate_json,score_json,score,"
      "human_message_count,model_status,serious_categories_json,created_at_ms,"
      "finalized_at_ms) VALUES(?,?,?,?,1,'final',?,?,?,?,?,?,?,?,?,?) ON "
      "CONFLICT(candidate_id) "
      "DO NOTHING");
  insert.bind(1, decision_id);
  insert.bind(2, candidate.candidate_id);
  insert.bind(3, policy.policy_version);
  insert.bind(4, instance_id);
  insert.bind(5, action);
  insert.bind(6, reason);
  insert.bind(7, gate_json(final_evaluation).dump());
  insert.bind(8, score_json(final_evaluation).dump());
  insert.bind(9, static_cast<std::int64_t>(final_evaluation.score));
  insert.bind(10, final_candidate.human_message_count);
  insert.bind(11, model_status);
  insert.bind(12, Json(categories).dump());
  insert.bind(13, now_ms);
  insert.bind(14, now_ms);
  insert.execute();
  const bool created = connection.changes() != 0;
  if (created) {
    insert_decision_memories(connection, decision_id, final_candidate,
                             model_result);
    if (action == std::string_view{"hypothetical"} && model_result &&
        !model_result->text.empty()) {
      auto preview =
          connection.prepare("INSERT INTO "
                             "appearance_preview(decision_id,preview_text,tone,"
                             "created_at_ms,expires_at_ms) VALUES(?,?,?,?,?)");
      preview.bind(1, decision_id);
      preview.bind(2, model_result->text);
      preview.bind(3, model_result->tone);
      preview.bind(4, now_ms);
      preview.bind(5, now_ms + policy.generated_preview_retention_ms);
      preview.execute();
    }
    append_decision_event(connection, event_id, final_candidate, decision_id,
                          action, reason, now_ms);
  }
  transaction.commit();
  return created;
}

bool SqliteAppearanceRepository::prepare_model(
    const AppearancePolicy &policy, const AppearanceMode mode,
    const AppearanceCandidate &candidate,
    const AppearanceEvaluation &evaluation, std::string decision_id,
    std::string event_id, const std::string_view instance_id,
    const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  static_cast<void>(evaluation);
  if (!candidate.policy_version.empty() &&
      candidate.policy_version != policy.policy_version)
    throw std::runtime_error{"Appearance candidate policy mismatch."};
  auto prepared_candidate = candidate;
  refresh_persistent_gates(connection, policy, prepared_candidate, now_ms);
  const auto prepared_evaluation =
      evaluate_appearance(policy, mode, prepared_candidate, now_ms);
  if (!prepared_evaluation.eligible_for_model) {
    const auto categories =
        serious_categories(prepared_candidate, std::nullopt);
    auto reject = connection.prepare(
        "INSERT INTO "
        "appearance_decision(decision_id,candidate_id,policy_version,"
        "application_instance_id,revision,state,action,reason,gate_json,score_"
        "json,"
        "score,human_message_count,model_status,serious_categories_json,"
        "created_at_ms,finalized_at_ms) "
        "VALUES(?,?,?,?,1,'final','reject',?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(candidate_id) DO NOTHING");
    reject.bind(1, decision_id);
    reject.bind(2, candidate.candidate_id);
    reject.bind(3, policy.policy_version);
    reject.bind(4, instance_id);
    reject.bind(5, prepared_evaluation.reason);
    reject.bind(6, gate_json(prepared_evaluation).dump());
    reject.bind(7, score_json(prepared_evaluation).dump());
    reject.bind(8, static_cast<std::int64_t>(prepared_evaluation.score));
    reject.bind(9, prepared_candidate.human_message_count);
    reject.bind(10, "not_requested_recheck");
    reject.bind(11, Json(categories).dump());
    reject.bind(12, now_ms);
    reject.bind(13, now_ms);
    reject.execute();
    if (connection.changes() != 0) {
      insert_decision_memories(connection, decision_id, prepared_candidate,
                               std::nullopt);
      append_decision_event(connection, event_id, prepared_candidate,
                            decision_id, "reject", prepared_evaluation.reason,
                            now_ms);
    }
    transaction.commit();
    return false;
  }
  auto insert = connection.prepare(
      "INSERT INTO "
      "appearance_decision(decision_id,candidate_id,policy_version,application_"
      "instance_id,revision,state,gate_json,score_json,score,human_message_"
      "count,"
      "model_status,serious_categories_json,created_at_ms)"
      " VALUES(?,?,?,?,1,'model_pending',?,?,?,?,'model_pending',?,?) ON "
      "CONFLICT(candidate_id) DO NOTHING");
  insert.bind(1, decision_id);
  insert.bind(2, candidate.candidate_id);
  insert.bind(3, policy.policy_version);
  insert.bind(4, instance_id);
  insert.bind(5, gate_json(prepared_evaluation).dump());
  insert.bind(6, score_json(prepared_evaluation).dump());
  insert.bind(7, static_cast<std::int64_t>(prepared_evaluation.score));
  insert.bind(8, prepared_candidate.human_message_count);
  const auto prepared_categories =
      serious_categories(prepared_candidate, std::nullopt);
  insert.bind(9, Json(prepared_categories).dump());
  insert.bind(10, now_ms);
  insert.execute();
  const bool created = connection.changes() != 0;
  if (created)
    insert_decision_memories(connection, decision_id, prepared_candidate,
                             std::nullopt);
  transaction.commit();
  return created;
}

bool SqliteAppearanceRepository::complete_model(
    const AppearancePolicy &policy, const AppearanceMode mode,
    const AppearanceCandidate &candidate,
    const AppearanceEvaluation &fresh_evaluation,
    const std::string_view decision_id, std::string event_id,
    std::string model_status, std::optional<AppearanceModelResult> result,
    const std::int64_t now_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  static_cast<void>(fresh_evaluation);
  if (!candidate.policy_version.empty() &&
      candidate.policy_version != policy.policy_version)
    throw std::runtime_error{"Appearance candidate policy mismatch."};
  auto final_candidate = candidate;
  refresh_persistent_gates(connection, policy, final_candidate, now_ms);
  const auto final_evaluation =
      evaluate_appearance(policy, mode, final_candidate, now_ms);
  const auto reason = final_reason(final_evaluation, model_status, result);
  const auto action = reason == "hypothetical" ? "hypothetical" : "reject";
  const auto categories = serious_categories(final_candidate, result);
  auto update = connection.prepare(
      "UPDATE appearance_decision SET "
      "revision=2,state='final',action=?,reason=?,gate_json=?,score_json=?,"
      "score=?,human_message_count=?,model_status=?,serious_categories_json=?,"
      "finalized_at_ms=? "
      "WHERE decision_id=? AND state='model_pending' AND revision=1");
  update.bind(1, action);
  update.bind(2, reason);
  update.bind(3, gate_json(final_evaluation).dump());
  update.bind(4, score_json(final_evaluation).dump());
  update.bind(5, static_cast<std::int64_t>(final_evaluation.score));
  update.bind(6, final_candidate.human_message_count);
  update.bind(7, model_status);
  update.bind(8, Json(categories).dump());
  update.bind(9, now_ms);
  update.bind(10, decision_id);
  update.execute();
  const bool changed = connection.changes() != 0;
  if (changed) {
    insert_decision_memories(connection, decision_id, final_candidate, result);
    if (action == std::string_view{"hypothetical"} && result &&
        !result->text.empty()) {
      auto preview =
          connection.prepare("INSERT INTO "
                             "appearance_preview(decision_id,preview_text,tone,"
                             "created_at_ms,expires_at_ms) VALUES(?,?,?,?,?)");
      preview.bind(1, decision_id);
      preview.bind(2, result->text);
      preview.bind(3, result->tone);
      preview.bind(4, now_ms);
      preview.bind(5, now_ms + policy.generated_preview_retention_ms);
      preview.execute();
    }
    append_decision_event(connection, event_id, final_candidate, decision_id,
                          action, reason, now_ms);
  }
  transaction.commit();
  return changed;
}

std::optional<AppearanceDecisionRecord>
SqliteAppearanceRepository::decision(const std::string_view reference) {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT "
      "d.decision_id,d.candidate_id,c.policy_version,c.candidate_type,"
      "coalesce(json_extract(c.context_json,'$.safe_summary'),''),d.state,"
      "coalesce(d.action,''),coalesce(d.reason,''),"
      "d.score,d.model_status,p.preview_text,d.created_at_ms,d.gate_json,d."
      "score_json "
      ",d.serious_categories_json "
      "FROM appearance_decision d JOIN appearance_candidate c ON "
      "c.candidate_id=d.candidate_id "
      "LEFT JOIN appearance_preview p ON p.decision_id=d.decision_id "
      "WHERE d.decision_id LIKE ? OR d.candidate_id LIKE ? ORDER BY "
      "d.created_at_ms DESC LIMIT 2");
  const auto pattern = std::string{reference} + "%";
  query.bind(1, pattern);
  query.bind(2, pattern);
  if (!query.step())
    return std::nullopt;
  AppearanceDecisionRecord result{
      .decision_id = query.column_text(0),
      .candidate_id = query.column_text(1),
      .policy_version = query.column_text(2),
      .candidate_type = query.column_text(3),
      .safe_summary = query.column_text(4),
      .state = query.column_text(5),
      .action = query.column_text(6),
      .reason = query.column_text(7),
      .score = static_cast<int>(query.column_int64(8)),
      .model_status = query.column_text(9),
      .preview = query.column_is_null(10)
                     ? std::nullopt
                     : std::optional<std::string>{query.column_text(10)},
      .created_at_ms = query.column_int64(11),
      .gates = {},
      .score_components = {},
      .memory_ids = {},
      .serious_categories =
          Json::parse(query.column_text(14)).get<std::vector<std::string>>()};
  for (const auto &item : Json::parse(query.column_text(12)))
    result.gates.push_back(
        {item.at("name").get<std::string>(), item.at("passed").get<bool>()});
  for (const auto &item : Json::parse(query.column_text(13)))
    result.score_components.push_back(
        {item.at("name").get<std::string>(), item.at("points").get<int>()});
  auto memories = context_->connection().prepare(
      "SELECT memory_id FROM appearance_decision_memory WHERE decision_id=? "
      "ORDER BY selection_rank");
  memories.bind(1, result.decision_id);
  while (memories.step())
    result.memory_ids.push_back(memories.column_text(0));
  if (query.step())
    return std::nullopt;
  return result;
}

std::vector<AppearanceDecisionRecord>
SqliteAppearanceRepository::recent(const std::size_t limit) {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT decision_id FROM appearance_decision ORDER BY created_at_ms "
      "DESC,decision_id DESC LIMIT ?");
  query.bind(1, static_cast<std::int64_t>(std::min<std::size_t>(limit, 10)));
  std::vector<std::string> ids;
  while (query.step())
    ids.push_back(query.column_text(0));
  std::vector<AppearanceDecisionRecord> result;
  for (const auto &id : ids) {
    auto row = context_->connection().prepare(
        "SELECT "
        "d.decision_id,d.candidate_id,c.policy_version,c.candidate_type,"
        "coalesce(json_extract(c.context_json,'$.safe_summary'),''),d.state,"
        "coalesce(d.action,''),coalesce(d.reason,''),d.score,d.model_status,"
        "p.preview_text,d.created_at_ms,d.gate_json,d.score_json,d.serious_"
        "categories_json "
        "FROM appearance_decision d JOIN appearance_candidate c ON "
        "c.candidate_id=d.candidate_id LEFT JOIN appearance_preview p ON "
        "p.decision_id=d.decision_id WHERE d.decision_id=?");
    row.bind(1, id);
    if (!row.step())
      throw std::runtime_error{"Recent appearance decision disappeared."};
    AppearanceDecisionRecord item{
        .decision_id = row.column_text(0),
        .candidate_id = row.column_text(1),
        .policy_version = row.column_text(2),
        .candidate_type = row.column_text(3),
        .safe_summary = row.column_text(4),
        .state = row.column_text(5),
        .action = row.column_text(6),
        .reason = row.column_text(7),
        .score = static_cast<int>(row.column_int64(8)),
        .model_status = row.column_text(9),
        .preview = row.column_is_null(10)
                       ? std::nullopt
                       : std::optional<std::string>{row.column_text(10)},
        .created_at_ms = row.column_int64(11),
        .gates = {},
        .score_components = {},
        .memory_ids = {},
        .serious_categories =
            Json::parse(row.column_text(14)).get<std::vector<std::string>>()};
    for (const auto &gate : Json::parse(row.column_text(12)))
      item.gates.push_back(
          {gate.at("name").get<std::string>(), gate.at("passed").get<bool>()});
    for (const auto &score : Json::parse(row.column_text(13)))
      item.score_components.push_back(
          {score.at("name").get<std::string>(), score.at("points").get<int>()});
    auto memory = context_->connection().prepare(
        "SELECT memory_id FROM appearance_decision_memory WHERE decision_id=? "
        "ORDER BY selection_rank");
    memory.bind(1, item.decision_id);
    while (memory.step())
      item.memory_ids.push_back(memory.column_text(0));
    result.push_back(std::move(item));
  }
  return result;
}

std::size_t SqliteAppearanceRepository::public_outbox_violation_count() {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT count(*) FROM outbox_message WHERE kind LIKE 'appearance.%' OR "
      "aggregate_type IN "
      "('appearance','appearance_candidate','appearance_decision') OR "
      "aggregate_id IN (SELECT candidate_id FROM appearance_candidate) OR "
      "aggregate_id IN (SELECT decision_id FROM appearance_decision)");
  if (!query.step())
    throw std::runtime_error{"Unable to inspect appearance outbox invariant."};
  return static_cast<std::size_t>(query.column_int64(0));
}

void SqliteAppearanceRepository::purge(const AppearancePolicy &policy,
                                       const std::int64_t now_ms) {
  static_cast<void>(policy);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto preview = connection.prepare(
      "DELETE FROM appearance_preview WHERE expires_at_ms<=?");
  preview.bind(1, now_ms);
  preview.execute();
  auto activity = connection.prepare(
      "DELETE FROM appearance_message_activity WHERE expires_at_ms<=?");
  activity.bind(1, now_ms);
  activity.execute();
  auto context =
      connection.prepare("UPDATE appearance_candidate SET context_json="
                         "json_set(context_json,'$.excerpts',json('[]')) "
                         "WHERE context_expires_at_ms<=? AND "
                         "json_array_length(context_json,'$.excerpts')>0");
  context.bind(1, now_ms);
  context.execute();
  transaction.commit();
}

} // namespace sanguinius::persistence
