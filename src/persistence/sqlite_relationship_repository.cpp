#include "sanguinius/persistence/sqlite_relationship_repository.hpp"

#include "sqlite_durable_work_writes.hpp"
#include "sqlite_relationship_writes.hpp"

#include "sanguinius/persistence/transaction.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

using Json = nlohmann::json;

struct ProjectionRow {
  RelationshipDimensions dimensions;
  std::size_t interaction_count{};
  std::int64_t last_interaction_at_ms{};
  std::size_t version{};
  std::int64_t updated_at_ms{};

  [[nodiscard]] bool operator==(const ProjectionRow &) const = default;
};

struct ProjectionBuild {
  std::map<std::string, ProjectionRow> rows;
  std::size_t event_count{};
  std::size_t chain_errors{};
};

struct RevisionIdentity {
  std::string id;
  std::size_t revision{};

  [[nodiscard]] bool operator==(const RevisionIdentity &) const = default;
};

struct ChroniclePromptSnapshot {
  std::optional<RevisionIdentity> featured_title;
  std::optional<RevisionIdentity> latest_summary;
  std::optional<RevisionIdentity> open_session;

  [[nodiscard]] bool
  operator==(const ChroniclePromptSnapshot &) const = default;
};

struct ChroniclePromptSelection {
  ChroniclePromptSnapshot snapshot;
  std::optional<std::string> featured_title_text;
  std::optional<std::string> latest_summary_text;
};

[[nodiscard]] ChroniclePromptSelection
load_chronicle_prompt_selection(SqliteConnection &connection,
                                const DiscordSnowflake requester,
                                const DiscordSnowflake guild) {
  ChroniclePromptSelection result;
  auto title = connection.prepare(
      "SELECT g.grant_id,g.revision,d.title FROM chronicle_title_grant g JOIN "
      "chronicle_title_definition d ON d.definition_id=g.definition_id "
      "WHERE g.recipient_user_id=? AND g.state='active' AND g.featured=1");
  title.bind(1, requester.str());
  if (title.step()) {
    result.snapshot.featured_title = RevisionIdentity{
        .id = title.column_text(0),
        .revision = static_cast<std::size_t>(title.column_int64(1))};
    result.featured_title_text = title.column_text(2);
  }

  auto summary = connection.prepare(
      "SELECT e.entry_id,e.revision,e.title || ' — ' || e.body FROM "
      "chronicle_entry e WHERE e.entry_type='session_summary' AND "
      "e.status='canon' AND e.visibility='shared' AND EXISTS (SELECT 1 FROM "
      "chronicle_participant p WHERE p.entry_id=e.entry_id AND p.user_id=?) "
      "ORDER BY e.occurred_at_ms DESC,e.entry_id DESC LIMIT 1");
  summary.bind(1, requester.str());
  if (summary.step()) {
    result.snapshot.latest_summary = RevisionIdentity{
        .id = summary.column_text(0),
        .revision = static_cast<std::size_t>(summary.column_int64(1))};
    result.latest_summary_text = summary.column_text(2);
  }

  auto session = connection.prepare(
      "SELECT session_id,revision FROM chronicle_session WHERE guild_id=? "
      "AND state='open'");
  session.bind(1, guild.str());
  if (session.step()) {
    result.snapshot.open_session = RevisionIdentity{
        .id = session.column_text(0),
        .revision = static_cast<std::size_t>(session.column_int64(1))};
  }
  return result;
}

void bind_revision_identity(SqliteStatement &statement,
                            const std::size_t id_index,
                            const std::size_t revision_index,
                            const std::optional<RevisionIdentity> &identity) {
  if (!identity) {
    statement.bind_null(id_index);
    statement.bind_null(revision_index);
    return;
  }
  statement.bind(id_index, identity->id);
  statement.bind(revision_index, static_cast<std::int64_t>(identity->revision));
}

[[nodiscard]] std::optional<RevisionIdentity>
read_revision_identity(SqliteStatement &statement, const int id_index,
                       const int revision_index) {
  const bool id_is_null = statement.column_is_null(id_index);
  const bool revision_is_null = statement.column_is_null(revision_index);
  if (id_is_null != revision_is_null)
    throw std::runtime_error{"Invalid prompt Chronicle snapshot."};
  if (id_is_null)
    return std::nullopt;
  return RevisionIdentity{.id = statement.column_text(id_index),
                          .revision = static_cast<std::size_t>(
                              statement.column_int64(revision_index))};
}

void require_id(const std::string_view value) {
  if (!valid_uuid_v4(std::string{value})) {
    throw std::invalid_argument{"Invalid relationship identifier."};
  }
}

void require_context(const DiscordSnowflake guild,
                     const DiscordSnowflake channel,
                     const DiscordSnowflake user,
                     const std::string_view correlation_id,
                     const std::int64_t now_ms) {
  if (!guild.is_set() || !channel.is_set() || !user.is_set() || now_ms < 0 ||
      correlation_id.empty() || correlation_id.size() > 160) {
    throw std::invalid_argument{"Invalid relationship request context."};
  }
}

[[nodiscard]] bool valid_code(const std::string_view value) {
  return !value.empty() && value.size() <= 96 &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_' ||
                  character == '.' || character == '-';
         });
}

void ensure_user(SqliteConnection &connection,
                 const PreparePromptContextRequest &request) {
  const auto cache =
      [](const std::string &value) -> std::optional<std::string> {
    return value.empty() || value.size() > 128 ? std::nullopt
                                               : std::optional{value};
  };
  const auto display = cache(request.requester_display_name);
  const auto username = cache(request.requester_username);
  auto user = connection.prepare(
      "INSERT INTO discord_user (user_id,display_name_cache,username_cache,"
      "is_bot,first_seen_at_ms,last_seen_at_ms,created_at_ms,updated_at_ms) "
      "VALUES (?,?,?,0,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
      "display_name_cache=COALESCE(excluded.display_name_cache,"
      "discord_user.display_name_cache),"
      "username_cache=COALESCE(excluded.username_cache,discord_user.username_"
      "cache),"
      "last_seen_at_ms=max(discord_user.last_seen_at_ms,excluded.last_seen_at_"
      "ms),"
      "updated_at_ms=max(discord_user.updated_at_ms,excluded.updated_at_ms)");
  user.bind(1, request.requester_user_id.str());
  if (display)
    user.bind(2, *display);
  else
    user.bind_null(2);
  if (username)
    user.bind(3, *username);
  else
    user.bind_null(3);
  user.bind(4, request.now_ms);
  user.bind(5, request.now_ms);
  user.bind(6, request.now_ms);
  user.bind(7, request.now_ms);
  user.execute();
  auto preference = connection.prepare(
      "INSERT INTO user_preference (user_id,updated_at_ms) VALUES (?,?) "
      "ON CONFLICT(user_id) DO NOTHING");
  preference.bind(1, request.requester_user_id.str());
  preference.bind(2, request.now_ms);
  preference.execute();
}

[[nodiscard]] ProjectionRow load_state(SqliteConnection &connection,
                                       const DiscordSnowflake user) {
  auto query = connection.prepare(
      "SELECT familiarity,esteem,mirth,reliability,wariness,interaction_count,"
      "last_interaction_at_ms,projection_version,updated_at_ms "
      "FROM relationship_state WHERE subject_user_id=?");
  query.bind(1, user.str());
  if (!query.step())
    return {};
  ProjectionRow result{
      .dimensions = {.familiarity = static_cast<int>(query.column_int64(0)),
                     .esteem = static_cast<int>(query.column_int64(1)),
                     .mirth = static_cast<int>(query.column_int64(2)),
                     .reliability = static_cast<int>(query.column_int64(3)),
                     .wariness = static_cast<int>(query.column_int64(4))},
      .interaction_count = static_cast<std::size_t>(query.column_int64(5)),
      .last_interaction_at_ms = query.column_int64(6),
      .version = static_cast<std::size_t>(query.column_int64(7)),
      .updated_at_ms = query.column_int64(8),
  };
  if (query.step())
    throw std::runtime_error{"Duplicate relationship projection."};
  return result;
}

[[nodiscard]] bool insert_relationship_event_impl(
    SqliteConnection &connection, const std::string &relationship_event_id,
    const std::string &source_event_id, const std::string_view event_type,
    const std::string_view reason_code, const DiscordSnowflake subject,
    const RelationshipDelta requested, const std::int64_t occurred_at_ms,
    const std::int64_t created_at_ms) {
  auto replay = connection.prepare(
      "SELECT 1 FROM relationship_event WHERE source_event_id=? AND "
      "subject_user_id=?");
  replay.bind(1, source_event_id);
  replay.bind(2, subject.str());
  if (replay.step())
    return false;

  const auto current = load_state(connection, subject);
  const auto next = apply_relationship_delta(current.dimensions, requested);
  const auto delta = applied_relationship_delta(current.dimensions, next);
  const auto version = current.version + 1;
  auto insert = connection.prepare(
      "INSERT INTO relationship_event (relationship_event_id,subject_user_id,"
      "source_event_id,event_type,reason_code,policy_version,reason_version,"
      "subject_version,"
      "delta_familiarity,delta_esteem,delta_mirth,delta_reliability,"
      "delta_wariness,old_familiarity,old_esteem,old_mirth,old_reliability,"
      "old_wariness,new_familiarity,new_esteem,new_mirth,new_reliability,"
      "new_wariness,occurred_at_ms,created_at_ms,is_test) "
      "VALUES (?,?,?,?,?,1,1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0)");
  std::size_t index = 1;
  insert.bind(index++, relationship_event_id);
  insert.bind(index++, subject.str());
  insert.bind(index++, source_event_id);
  insert.bind(index++, event_type);
  insert.bind(index++, reason_code);
  insert.bind(index++, static_cast<std::int64_t>(version));
  for (const int value :
       {delta.familiarity, delta.esteem, delta.mirth, delta.reliability,
        delta.wariness, current.dimensions.familiarity,
        current.dimensions.esteem, current.dimensions.mirth,
        current.dimensions.reliability, current.dimensions.wariness,
        next.familiarity, next.esteem, next.mirth, next.reliability,
        next.wariness}) {
    insert.bind(index++, static_cast<std::int64_t>(value));
  }
  insert.bind(index++, occurred_at_ms);
  insert.bind(index, created_at_ms);
  insert.execute();

  auto projection = connection.prepare(
      "INSERT INTO relationship_state (subject_user_id,familiarity,esteem,"
      "mirth,reliability,wariness,interaction_count,last_interaction_at_ms,"
      "projection_version,updated_at_ms) VALUES (?,?,?,?,?,?,1,?,?,?) "
      "ON CONFLICT(subject_user_id) DO UPDATE SET "
      "familiarity=excluded.familiarity,"
      "esteem=excluded.esteem,mirth=excluded.mirth,reliability=excluded."
      "reliability,"
      "wariness=excluded.wariness,interaction_count=relationship_state."
      "interaction_count+1,last_interaction_at_ms=max(relationship_state."
      "last_interaction_at_ms,excluded.last_interaction_at_ms),"
      "projection_version=excluded.projection_version,"
      "updated_at_ms=max(relationship_state.updated_at_ms,excluded.updated_at_"
      "ms)");
  projection.bind(1, subject.str());
  projection.bind(2, static_cast<std::int64_t>(next.familiarity));
  projection.bind(3, static_cast<std::int64_t>(next.esteem));
  projection.bind(4, static_cast<std::int64_t>(next.mirth));
  projection.bind(5, static_cast<std::int64_t>(next.reliability));
  projection.bind(6, static_cast<std::int64_t>(next.wariness));
  projection.bind(7, occurred_at_ms);
  projection.bind(8, static_cast<std::int64_t>(version));
  projection.bind(9, created_at_ms);
  projection.execute();
  return true;
}

[[nodiscard]] ProjectionBuild build_projection(SqliteConnection &connection) {
  ProjectionBuild result;
  auto events = connection.prepare(
      "SELECT subject_user_id,subject_version,delta_familiarity,delta_esteem,"
      "delta_mirth,delta_reliability,delta_wariness,old_familiarity,old_esteem,"
      "old_mirth,old_reliability,old_wariness,new_familiarity,new_esteem,"
      "new_mirth,new_reliability,new_wariness,occurred_at_ms,created_at_ms "
      "FROM relationship_event ORDER BY subject_user_id,subject_version");
  while (events.step()) {
    ++result.event_count;
    const auto user = events.column_text(0);
    auto &projection = result.rows[user];
    const auto version = static_cast<std::size_t>(events.column_int64(1));
    const RelationshipDimensions old_values{
        static_cast<int>(events.column_int64(7)),
        static_cast<int>(events.column_int64(8)),
        static_cast<int>(events.column_int64(9)),
        static_cast<int>(events.column_int64(10)),
        static_cast<int>(events.column_int64(11))};
    const RelationshipDimensions new_values{
        static_cast<int>(events.column_int64(12)),
        static_cast<int>(events.column_int64(13)),
        static_cast<int>(events.column_int64(14)),
        static_cast<int>(events.column_int64(15)),
        static_cast<int>(events.column_int64(16))};
    const RelationshipDelta stored_delta{
        static_cast<int>(events.column_int64(2)),
        static_cast<int>(events.column_int64(3)),
        static_cast<int>(events.column_int64(4)),
        static_cast<int>(events.column_int64(5)),
        static_cast<int>(events.column_int64(6))};
    if (version != projection.version + 1 ||
        old_values != projection.dimensions ||
        applied_relationship_delta(old_values, new_values) != stored_delta) {
      ++result.chain_errors;
    }
    projection.dimensions = new_values;
    ++projection.interaction_count;
    projection.last_interaction_at_ms =
        std::max(projection.last_interaction_at_ms, events.column_int64(17));
    projection.updated_at_ms =
        std::max(projection.updated_at_ms, events.column_int64(18));
    projection.version = version;
  }
  return result;
}

[[nodiscard]] ProjectionCheckResult
compare_projection(SqliteConnection &connection,
                   const ProjectionBuild &expected) {
  std::size_t mismatches = expected.chain_errors;
  std::size_t count = 0;
  std::set<std::string> seen;
  auto rows = connection.prepare(
      "SELECT subject_user_id,familiarity,esteem,mirth,reliability,wariness,"
      "interaction_count,last_interaction_at_ms,projection_version,updated_at_"
      "ms "
      "FROM relationship_state");
  while (rows.step()) {
    ++count;
    const auto user = rows.column_text(0);
    seen.insert(user);
    const auto found = expected.rows.find(user);
    if (found == expected.rows.end()) {
      ++mismatches;
      continue;
    }
    const ProjectionRow actual{
        .dimensions = {static_cast<int>(rows.column_int64(1)),
                       static_cast<int>(rows.column_int64(2)),
                       static_cast<int>(rows.column_int64(3)),
                       static_cast<int>(rows.column_int64(4)),
                       static_cast<int>(rows.column_int64(5))},
        .interaction_count = static_cast<std::size_t>(rows.column_int64(6)),
        .last_interaction_at_ms = rows.column_int64(7),
        .version = static_cast<std::size_t>(rows.column_int64(8)),
        .updated_at_ms = rows.column_int64(9),
    };
    if (actual != found->second)
      ++mismatches;
  }
  for (const auto &[user, row] : expected.rows) {
    static_cast<void>(row);
    if (!seen.contains(user))
      ++mismatches;
  }
  return {.valid = mismatches == 0,
          .event_count = expected.event_count,
          .projection_count = count,
          .mismatch_count = mismatches};
}

} // namespace

bool detail::insert_relationship_event_uncommitted(
    SqliteConnection &connection, const std::string &relationship_event_id,
    const std::string &source_event_id, const std::string_view event_type,
    const std::string_view reason_code, const DiscordSnowflake subject,
    const RelationshipDelta requested, const std::int64_t occurred_at_ms,
    const std::int64_t created_at_ms) {
  return insert_relationship_event_impl(
      connection, relationship_event_id, source_event_id, event_type,
      reason_code, subject, requested, occurred_at_ms, created_at_ms);
}

SqliteRelationshipRepository::SqliteRelationshipRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite context is required."};
}

PreparedPromptContext SqliteRelationshipRepository::prepare_prompt_context(
    const PreparePromptContextRequest &request) {
  require_id(request.attempt_id);
  require_id(request.application_instance_id);
  require_context(request.guild_id, request.channel_id,
                  request.requester_user_id, request.correlation_id,
                  request.now_ms);
  if (!request.source_message_id.is_set()) {
    throw std::invalid_argument{"Prompt source message must be set."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  ensure_user(connection, request);

  auto duplicate = connection.prepare(
      "SELECT 1 FROM ai_prompt_attempt WHERE guild_id=? AND channel_id=? "
      "AND source_message_id=?");
  duplicate.bind(1, request.guild_id.str());
  duplicate.bind(2, request.channel_id.str());
  duplicate.bind(3, request.source_message_id.str());
  if (duplicate.step()) {
    transaction.commit();
    return {.status = PromptPreparationStatus::duplicate,
            .attempt_id = std::nullopt,
            .relationship_style = {},
            .memories = {},
            .featured_title = std::nullopt,
            .latest_session_summary = std::nullopt,
            .session_open = false};
  }

  auto preferences = connection.prepare(
      "SELECT chronicle_opt_in,memory_callback_opt_in,updated_at_ms "
      "FROM user_preference WHERE user_id=?");
  preferences.bind(1, request.requester_user_id.str());
  if (!preferences.step()) {
    throw std::runtime_error{
        "User preference missing after identity creation."};
  }
  const bool chronicle_opt_in = preferences.column_int64(0) != 0;
  const bool callbacks = chronicle_opt_in && preferences.column_int64(1) != 0;
  const auto preference_updated_at_ms = preferences.column_int64(2);
  const auto state = chronicle_opt_in
                         ? load_state(connection, request.requester_user_id)
                         : ProjectionRow{};
  std::vector<MemoryCandidate> candidates;
  if (callbacks) {
    auto query = connection.prepare(
        "SELECT m.memory_id,m.text,m.created_at_ms,m.revision FROM memory m "
        "WHERE m.status='confirmed' AND m.visibility='shared' "
        "AND m.sensitivity='ordinary' "
        "AND (m.expires_at_ms IS NULL OR m.expires_at_ms>?) "
        "AND (m.last_used_at_ms IS NULL OR m.last_used_at_ms<=?) "
        "AND EXISTS (SELECT 1 FROM memory_subject s WHERE "
        "s.memory_id=m.memory_id "
        "AND s.subject_type='user' AND s.subject_id=?) "
        "AND NOT EXISTS (SELECT 1 FROM memory_subject s WHERE "
        "s.memory_id=m.memory_id AND s.subject_type='user' AND "
        "s.subject_id<>?) "
        "AND NOT EXISTS (SELECT 1 FROM ai_prompt_attempt_memory am JOIN "
        "ai_prompt_attempt a ON a.attempt_id=am.attempt_id WHERE "
        "am.memory_id=m.memory_id AND a.state='prepared') "
        "ORDER BY m.created_at_ms DESC,m.memory_id LIMIT ?");
    query.bind(1, request.now_ms);
    query.bind(2, request.now_ms - memory_callback_cooldown_ms);
    query.bind(3, request.requester_user_id.str());
    query.bind(4, request.requester_user_id.str());
    query.bind(5, static_cast<std::int64_t>(maximum_prompt_memory_candidates));
    while (query.step()) {
      MemoryCandidate candidate{
          .memory_id = query.column_text(0),
          .text = query.column_text(1),
          .tags = {},
          .created_at_ms = query.column_int64(2),
          .revision = static_cast<std::size_t>(query.column_int64(3))};
      auto tags = connection.prepare(
          "SELECT subject_id FROM memory_subject WHERE memory_id=? AND "
          "subject_type='topic' ORDER BY subject_id");
      tags.bind(1, candidate.memory_id);
      while (tags.step())
        candidate.tags.push_back(tags.column_text(0));
      candidates.push_back(std::move(candidate));
    }
  }
  auto selected =
      rank_prompt_memories(std::move(candidates), request.current_request,
                           request.replied_text, request.now_ms);
  std::optional<std::string> featured_title;
  std::optional<std::string> latest_summary;
  bool session_open = false;
  ChroniclePromptSnapshot chronicle_snapshot;
  if (chronicle_opt_in) {
    auto selection = load_chronicle_prompt_selection(
        connection, request.requester_user_id, request.guild_id);
    chronicle_snapshot = std::move(selection.snapshot);
    featured_title = std::move(selection.featured_title_text);
    latest_summary = std::move(selection.latest_summary_text);
    session_open = chronicle_snapshot.open_session.has_value();
  }
  auto attempt = connection.prepare(
      "INSERT INTO ai_prompt_attempt (attempt_id,application_instance_id,"
      "requester_user_id,guild_id,channel_id,source_message_id,state,"
      "correlation_id,preference_updated_at_ms,prepared_at_ms) "
      "VALUES (?,?,?,?,?,?,'prepared',?,?,?)");
  attempt.bind(1, request.attempt_id);
  attempt.bind(2, request.application_instance_id);
  attempt.bind(3, request.requester_user_id.str());
  attempt.bind(4, request.guild_id.str());
  attempt.bind(5, request.channel_id.str());
  attempt.bind(6, request.source_message_id.str());
  attempt.bind(7, request.correlation_id);
  attempt.bind(8, preference_updated_at_ms);
  attempt.bind(9, request.now_ms);
  attempt.execute();
  if (chronicle_opt_in) {
    auto snapshot = connection.prepare(
        "INSERT INTO ai_prompt_attempt_chronicle_context(attempt_id,"
        "featured_title_grant_id,featured_title_revision,"
        "latest_summary_entry_id,latest_summary_revision,open_session_id,"
        "open_session_revision) VALUES (?,?,?,?,?,?,?)");
    snapshot.bind(1, request.attempt_id);
    bind_revision_identity(snapshot, 2, 3, chronicle_snapshot.featured_title);
    bind_revision_identity(snapshot, 4, 5, chronicle_snapshot.latest_summary);
    bind_revision_identity(snapshot, 6, 7, chronicle_snapshot.open_session);
    snapshot.execute();
  }
  for (std::size_t index = 0; index < selected.size(); ++index) {
    auto memory = connection.prepare(
        "INSERT INTO ai_prompt_attempt_memory (attempt_id,memory_id,"
        "memory_revision,rank_position,selected_at_ms) VALUES (?,?,?,?,?)");
    memory.bind(1, request.attempt_id);
    memory.bind(2, selected[index].memory.memory_id);
    memory.bind(3, static_cast<std::int64_t>(selected[index].memory.revision));
    memory.bind(4, static_cast<std::int64_t>(index));
    memory.bind(5, request.now_ms);
    memory.execute();
  }
  transaction.commit();
  return {.status = PromptPreparationStatus::prepared,
          .attempt_id = request.attempt_id,
          .relationship_style = chronicle_opt_in
                                    ? relationship_style_hint(state.dimensions)
                                    : std::string{},
          .memories = std::move(selected),
          .featured_title = std::move(featured_title),
          .latest_session_summary = std::move(latest_summary),
          .session_open = session_open};
}

PromptFinalizationStatus SqliteRelationshipRepository::complete_prompt_attempt(
    const CompletePromptAttemptRequest &request) {
  require_id(request.attempt_id);
  require_id(request.source_event_id);
  require_id(request.relationship_event_id);
  if (request.now_ms < 0)
    throw std::invalid_argument{"Invalid completion time."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto attempt = connection.prepare(
      "SELECT requester_user_id,guild_id,channel_id,source_message_id,state,"
      "correlation_id,preference_updated_at_ms,prepared_at_ms "
      "FROM ai_prompt_attempt "
      "WHERE attempt_id=?");
  attempt.bind(1, request.attempt_id);
  if (!attempt.step())
    return PromptFinalizationStatus::not_found;
  const auto requester = DiscordSnowflake::parse(attempt.column_text(0));
  const auto guild = DiscordSnowflake::parse(attempt.column_text(1));
  const auto channel = DiscordSnowflake::parse(attempt.column_text(2));
  const auto message = DiscordSnowflake::parse(attempt.column_text(3));
  const auto state = attempt.column_text(4);
  const auto correlation_id = attempt.column_text(5);
  const auto preference_snapshot = attempt.column_int64(6);
  const auto completed_at_ms =
      std::max(request.now_ms, attempt.column_int64(7));
  if (state == "succeeded") {
    transaction.commit();
    return PromptFinalizationStatus::unchanged;
  }
  if (state != "prepared") {
    transaction.commit();
    return PromptFinalizationStatus::invalidated;
  }

  auto memory_count_query = connection.prepare(
      "SELECT count(*) FROM ai_prompt_attempt_memory WHERE attempt_id=?");
  memory_count_query.bind(1, request.attempt_id);
  if (!memory_count_query.step())
    throw std::runtime_error{"Memory count failed."};
  const auto memory_count = memory_count_query.column_int64(0);
  auto preference = connection.prepare(
      "SELECT chronicle_opt_in,memory_callback_opt_in,updated_at_ms FROM "
      "user_preference WHERE user_id=?");
  preference.bind(1, requester.str());
  const bool has_preference = preference.step();
  const bool chronicle_opt_in =
      has_preference && preference.column_int64(0) != 0;
  bool valid = true;
  bool chronicle_context_included = false;
  auto chronicle_context = connection.prepare(
      "SELECT featured_title_grant_id,featured_title_revision,"
      "latest_summary_entry_id,latest_summary_revision,open_session_id,"
      "open_session_revision FROM ai_prompt_attempt_chronicle_context "
      "WHERE attempt_id=?");
  chronicle_context.bind(1, request.attempt_id);
  if (chronicle_context.step()) {
    chronicle_context_included = true;
    const ChroniclePromptSnapshot stored{
        .featured_title = read_revision_identity(chronicle_context, 0, 1),
        .latest_summary = read_revision_identity(chronicle_context, 2, 3),
        .open_session = read_revision_identity(chronicle_context, 4, 5),
    };
    valid = chronicle_opt_in &&
            preference.column_int64(2) == preference_snapshot &&
            load_chronicle_prompt_selection(connection, requester, guild)
                    .snapshot == stored;
  }
  if (memory_count > 0) {
    valid = valid && chronicle_opt_in && preference.column_int64(1) != 0 &&
            preference.column_int64(2) == preference_snapshot;
    auto memories = connection.prepare(
        "SELECT count(*) FROM ai_prompt_attempt_memory am JOIN memory m ON "
        "m.memory_id=am.memory_id WHERE am.attempt_id=? AND "
        "m.revision=am.memory_revision AND m.status='confirmed' AND "
        "m.visibility='shared' AND m.sensitivity='ordinary' AND "
        "(m.expires_at_ms IS NULL OR m.expires_at_ms>?) AND "
        "EXISTS (SELECT 1 FROM memory_subject s WHERE s.memory_id=m.memory_id "
        "AND s.subject_type='user' AND s.subject_id=?) AND NOT EXISTS "
        "(SELECT 1 FROM memory_subject s WHERE s.memory_id=m.memory_id AND "
        "s.subject_type='user' AND s.subject_id<>?)");
    memories.bind(1, request.attempt_id);
    memories.bind(2, completed_at_ms);
    memories.bind(3, requester.str());
    memories.bind(4, requester.str());
    if (!memories.step() || memories.column_int64(0) != memory_count)
      valid = false;
  }
  if (!valid) {
    auto invalidate = connection.prepare(
        "UPDATE ai_prompt_attempt SET state='privacy_invalidated',"
        "completed_at_ms=?,failure_code='context_changed' WHERE attempt_id=? "
        "AND state='prepared'");
    invalidate.bind(1, completed_at_ms);
    invalidate.bind(2, request.attempt_id);
    invalidate.execute();
    transaction.commit();
    return PromptFinalizationStatus::invalidated;
  }

  if (memory_count > 0) {
    auto use = connection.prepare(
        "UPDATE memory SET last_used_at_ms=max(COALESCE(last_used_at_ms,0),"
        "confirmed_at_ms,?),"
        "use_count=use_count+1 WHERE memory_id IN (SELECT memory_id FROM "
        "ai_prompt_attempt_memory WHERE attempt_id=?)");
    use.bind(1, completed_at_ms);
    use.bind(2, request.attempt_id);
    use.execute();
  }

  const EventJournalEntry event{
      .event_id = request.source_event_id,
      .event_type = "ai.direct_interaction_succeeded.v1",
      .aggregate_type = "discord_message",
      .aggregate_id = message.str(),
      .actor_user_id = requester,
      .guild_id = guild,
      .channel_id = channel,
      .source_message_id = message,
      .occurred_at_ms = completed_at_ms,
      .recorded_at_ms = completed_at_ms,
      .correlation_id = correlation_id,
      .causation_id = std::nullopt,
      .idempotency_key = "event:ai:direct:" + message.str(),
      .payload_json =
          Json{{"status", "succeeded"}, {"memory_count", memory_count}}.dump(),
  };
  if (!detail::insert_event_uncommitted(connection, event)) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "AI success event idempotency conflict."};
  }
  if (chronicle_context_included && chronicle_opt_in) {
    auto recent = connection.prepare(
        "SELECT 1 FROM relationship_event WHERE subject_user_id=? AND "
        "reason_code='ai.direct' AND delta_familiarity>0 AND occurred_at_ms>? "
        "LIMIT 1");
    recent.bind(1, requester.str());
    recent.bind(2, completed_at_ms - relationship_direct_cooldown_ms);
    const bool cooldown = recent.step();
    static_cast<void>(detail::insert_relationship_event_uncommitted(
        connection, request.relationship_event_id, request.source_event_id,
        "ai.direct_interaction_succeeded.v1",
        cooldown ? "ai.direct_cooldown" : "ai.direct", requester,
        relationship_policy(RelationshipSourceKind::direct_ai, cooldown),
        completed_at_ms, completed_at_ms));
  }
  auto complete = connection.prepare(
      "UPDATE ai_prompt_attempt SET state='succeeded',completed_at_ms=?,"
      "failure_code=NULL WHERE attempt_id=? AND state='prepared'");
  complete.bind(1, completed_at_ms);
  complete.bind(2, request.attempt_id);
  complete.execute();
  transaction.commit();
  return PromptFinalizationStatus::applied;
}

PromptFinalizationStatus SqliteRelationshipRepository::fail_prompt_attempt(
    const FailPromptAttemptRequest &request) {
  require_id(request.attempt_id);
  if ((request.outcome != "model_failed" && request.outcome != "cancelled") ||
      !valid_code(request.error_code) || request.now_ms < 0) {
    throw std::invalid_argument{"Invalid prompt failure."};
  }
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto update = context_->connection().prepare(
      "UPDATE ai_prompt_attempt SET state=?,"
      "completed_at_ms=max(prepared_at_ms,?),failure_code=? "
      "WHERE attempt_id=? AND state='prepared'");
  update.bind(1, request.outcome);
  update.bind(2, request.now_ms);
  update.bind(3, request.error_code);
  update.bind(4, request.attempt_id);
  update.execute();
  if (context_->connection().changes() == 1) {
    transaction.commit();
    return PromptFinalizationStatus::applied;
  }
  auto found = context_->connection().prepare(
      "SELECT state FROM ai_prompt_attempt WHERE attempt_id=?");
  found.bind(1, request.attempt_id);
  const auto result = found.step() ? PromptFinalizationStatus::unchanged
                                   : PromptFinalizationStatus::not_found;
  transaction.commit();
  return result;
}

std::size_t SqliteRelationshipRepository::recover_prompt_attempts(
    const std::string_view instance_id, const std::int64_t now_ms) {
  require_id(instance_id);
  if (now_ms < 0)
    throw std::invalid_argument{"Invalid recovery time."};
  const std::scoped_lock lock{context_->mutex()};
  Transaction transaction{context_->connection(), TransactionMode::immediate};
  auto update = context_->connection().prepare(
      "UPDATE ai_prompt_attempt SET state='abandoned',"
      "completed_at_ms=max(prepared_at_ms,?),"
      "failure_code='process_restart' WHERE state='prepared' AND "
      "application_instance_id<>?");
  update.bind(1, now_ms);
  update.bind(2, instance_id);
  update.execute();
  const auto changed =
      static_cast<std::size_t>(context_->connection().changes());
  transaction.commit();
  return changed;
}

std::size_t SqliteRelationshipRepository::synchronize_chronicle_sources(
    PersistentIdGenerator &ids, const std::int64_t now_ms) {
  if (now_ms < 0)
    throw std::invalid_argument{"Invalid synchronization time."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto sources = connection.prepare(
      "SELECT e.event_id,e.event_type,e.occurred_at_ms,p.user_id FROM "
      "event_journal e JOIN chronicle_entry ce ON ce.entry_id=e.aggregate_id "
      "JOIN chronicle_participant p ON p.entry_id=e.aggregate_id "
      "JOIN discord_user u ON u.user_id=p.user_id JOIN user_preference pref ON "
      "pref.user_id=p.user_id WHERE "
      "e.event_type='chronicle.entry_canonized.v1' "
      "AND ce.entry_type NOT IN ('session_summary','title_award') "
      "AND p.role IN ('source_author','subject') AND u.is_bot=0 "
      "AND pref.chronicle_opt_in=1 AND "
      "COALESCE(json_extract(e.payload_json,'$.test'),0)=0 AND NOT EXISTS "
      "(SELECT 1 FROM relationship_event r WHERE r.source_event_id=e.event_id "
      "AND r.subject_user_id=p.user_id) GROUP BY e.event_id,p.user_id "
      "ORDER BY e.occurred_at_ms,e.event_id,p.user_id");
  std::size_t count = 0;
  while (sources.step()) {
    const auto event_id = sources.column_text(0);
    const auto event_type = sources.column_text(1);
    const auto occurred_at_ms = sources.column_int64(2);
    const auto user = DiscordSnowflake::parse(sources.column_text(3));
    if (detail::insert_relationship_event_uncommitted(
            connection, ids.next_id(), event_id, event_type, "chronicle.canon",
            user, relationship_policy(RelationshipSourceKind::chronicle_canon),
            occurred_at_ms, now_ms)) {
      ++count;
    }
  }
  auto sessions = connection.prepare(
      "SELECT e.event_id,e.event_type,e.occurred_at_ms,p.user_id FROM "
      "event_journal e JOIN chronicle_session_participant p ON "
      "p.session_id=e.aggregate_id JOIN discord_user u ON u.user_id=p.user_id "
      "JOIN user_preference pref ON pref.user_id=p.user_id WHERE "
      "e.event_type='chronicle.session_completed.v1' AND u.is_bot=0 AND "
      "pref.chronicle_opt_in=1 AND NOT EXISTS (SELECT 1 FROM "
      "relationship_event r "
      "WHERE r.source_event_id=e.event_id AND r.subject_user_id=p.user_id) "
      "ORDER BY e.occurred_at_ms,e.event_id,p.user_id");
  while (sessions.step()) {
    if (detail::insert_relationship_event_uncommitted(
            connection, ids.next_id(), sessions.column_text(0),
            sessions.column_text(1), "session.completed",
            DiscordSnowflake::parse(sessions.column_text(3)),
            relationship_policy(RelationshipSourceKind::session_completed),
            sessions.column_int64(2), now_ms))
      ++count;
  }
  auto titles = connection.prepare(
      "SELECT e.event_id,e.event_type,e.occurred_at_ms,"
      "json_extract(e.payload_json,'$.recipient_user_id') FROM event_journal e "
      "JOIN discord_user u ON u.user_id=json_extract(e.payload_json,"
      "'$.recipient_user_id') JOIN user_preference pref ON "
      "pref.user_id=u.user_id "
      "WHERE e.event_type='chronicle.title_awarded.v1' AND u.is_bot=0 AND "
      "pref.chronicle_opt_in=1 AND NOT EXISTS (SELECT 1 FROM "
      "relationship_event r "
      "WHERE r.source_event_id=e.event_id AND r.subject_user_id=u.user_id) "
      "ORDER BY e.occurred_at_ms,e.event_id,u.user_id");
  while (titles.step()) {
    if (detail::insert_relationship_event_uncommitted(
            connection, ids.next_id(), titles.column_text(0),
            titles.column_text(1), "title.awarded",
            DiscordSnowflake::parse(titles.column_text(3)),
            relationship_policy(RelationshipSourceKind::title_awarded),
            titles.column_int64(2), now_ms))
      ++count;
  }
  transaction.commit();
  return count;
}

RelationshipProfile SqliteRelationshipRepository::profile(
    const DiscordSnowflake &viewer, const DiscordSnowflake &target,
    const bool public_view, const std::int64_t now_ms) {
  if (!viewer.is_set() || !target.is_set() || now_ms < 0 ||
      (!public_view && viewer != target)) {
    throw std::invalid_argument{"Invalid relationship profile request."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto user = connection.prepare(
      "SELECT COALESCE(display_name_cache,username_cache,''),is_bot,"
      "chronicle_opt_in,memory_callback_opt_in FROM discord_user u JOIN "
      "user_preference p ON p.user_id=u.user_id WHERE u.user_id=?");
  user.bind(1, target.str());
  if (!user.step())
    return {};
  RelationshipProfile result{
      .found = true,
      .is_bot = user.column_int64(1) != 0,
      .chronicle_opt_in = user.column_int64(2) != 0,
      .memory_callbacks = user.column_int64(3) != 0,
      .user_id = target,
      .display_name = user.column_text(0),
      .dimensions = load_state(connection, target).dimensions,
      .recent_reasons = {},
      .shared_canon_count = 0,
      .visible_canon_titles = {},
      .featured_title = std::nullopt,
      .latest_session_summary = std::nullopt,
      .session_open = false,
  };
  auto count = connection.prepare(
      "SELECT count(DISTINCT e.entry_id) FROM chronicle_entry e JOIN "
      "chronicle_participant p ON p.entry_id=e.entry_id WHERE e.status='canon' "
      "AND e.visibility='shared' AND p.user_id=? AND "
      "p.role IN ('source_author','subject')");
  count.bind(1, target.str());
  if (count.step())
    result.shared_canon_count = static_cast<std::size_t>(count.column_int64(0));
  auto titles = connection.prepare(
      "SELECT DISTINCT e.title,e.occurred_at_ms,e.entry_id FROM "
      "chronicle_entry e "
      "JOIN chronicle_participant p ON p.entry_id=e.entry_id WHERE "
      "e.status='canon' AND p.user_id=? AND p.role IN "
      "('source_author','subject') "
      "AND (e.visibility='shared' OR (?=0 AND EXISTS (SELECT 1 FROM "
      "chronicle_participant vp WHERE vp.entry_id=e.entry_id AND "
      "vp.user_id=?))) "
      "ORDER BY e.occurred_at_ms DESC,e.entry_id DESC LIMIT 3");
  titles.bind(1, target.str());
  titles.bind(2, static_cast<std::int64_t>(public_view));
  titles.bind(3, viewer.str());
  while (titles.step())
    result.visible_canon_titles.push_back(titles.column_text(0));
  if (result.chronicle_opt_in) {
    auto featured = connection.prepare(
        "SELECT d.title FROM chronicle_title_grant g JOIN "
        "chronicle_title_definition d ON d.definition_id=g.definition_id "
        "WHERE g.recipient_user_id=? AND g.state='active' AND g.featured=1");
    featured.bind(1, target.str());
    if (featured.step())
      result.featured_title = featured.column_text(0);
    auto latest_summary = connection.prepare(
        "SELECT e.title || ' — ' || e.body FROM chronicle_entry e JOIN "
        "chronicle_participant p ON p.entry_id=e.entry_id WHERE "
        "e.entry_type='session_summary' AND e.status='canon' AND "
        "e.visibility='shared' AND p.user_id=? ORDER BY e.occurred_at_ms DESC,"
        "e.entry_id DESC LIMIT 1");
    latest_summary.bind(1, target.str());
    if (latest_summary.step())
      result.latest_session_summary = latest_summary.column_text(0);
    auto open_session = connection.prepare(
        "SELECT 1 FROM chronicle_session WHERE state='open' LIMIT 1");
    result.session_open = open_session.step();
  }
  if (!public_view) {
    auto reasons = connection.prepare(
        "SELECT reason_code FROM relationship_event WHERE subject_user_id=? "
        "ORDER BY subject_version DESC LIMIT 3");
    reasons.bind(1, target.str());
    while (reasons.step())
      result.recent_reasons.push_back(reasons.column_text(0));
  }
  return result;
}

PreferenceChangeStatus SqliteRelationshipRepository::set_memory_callbacks(
    const SetMemoryCallbacksRequest &request) {
  require_id(request.event_id);
  require_context(request.guild_id, request.channel_id, request.user_id,
                  request.correlation_id, request.now_ms);
  if (request.idempotency_key.empty() || request.idempotency_key.size() > 160) {
    throw std::invalid_argument{"Invalid callback idempotency key."};
  }
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto replay = connection.prepare(
      "SELECT event_type,actor_user_id,json_extract(payload_json,'$.enabled') "
      "FROM event_journal WHERE idempotency_key=?");
  replay.bind(1, request.idempotency_key);
  if (replay.step()) {
    const bool matches =
        replay.column_text(0) == "chronicle.memory_callbacks_changed.v1" &&
        replay.column_text(1) == request.user_id.str() &&
        (replay.column_int64(2) != 0) == request.enabled;
    if (!matches) {
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Callback preference idempotency conflict."};
    }
    transaction.commit();
    return PreferenceChangeStatus::unchanged;
  }
  auto preference = connection.prepare(
      "SELECT chronicle_opt_in,memory_callback_opt_in FROM user_preference "
      "WHERE user_id=?");
  preference.bind(1, request.user_id.str());
  if (!preference.step())
    throw std::runtime_error{"User preference missing."};
  if (request.enabled && preference.column_int64(0) == 0) {
    transaction.commit();
    return PreferenceChangeStatus::chronicle_opted_out;
  }
  if ((preference.column_int64(1) != 0) == request.enabled) {
    transaction.commit();
    return PreferenceChangeStatus::unchanged;
  }
  auto update = connection.prepare(
      "UPDATE user_preference SET memory_callback_opt_in=?,updated_at_ms=? "
      "WHERE user_id=?");
  update.bind(1, static_cast<std::int64_t>(request.enabled));
  update.bind(2, request.now_ms);
  update.bind(3, request.user_id.str());
  update.execute();
  const EventJournalEntry event{
      .event_id = request.event_id,
      .event_type = "chronicle.memory_callbacks_changed.v1",
      .aggregate_type = "user_preference",
      .aggregate_id = request.user_id.str(),
      .actor_user_id = request.user_id,
      .guild_id = request.guild_id,
      .channel_id = request.channel_id,
      .source_message_id = std::nullopt,
      .occurred_at_ms = request.now_ms,
      .recorded_at_ms = request.now_ms,
      .correlation_id = request.correlation_id,
      .causation_id = std::nullopt,
      .idempotency_key = request.idempotency_key,
      .payload_json = Json{{"enabled", request.enabled}}.dump(),
  };
  if (!detail::insert_event_uncommitted(connection, event)) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Callback preference event conflict."};
  }
  transaction.commit();
  return PreferenceChangeStatus::updated;
}

ProjectionCheckResult SqliteRelationshipRepository::check_projection() {
  const std::scoped_lock lock{context_->mutex()};
  const auto expected = build_projection(context_->connection());
  return compare_projection(context_->connection(), expected);
}

ProjectionCheckResult SqliteRelationshipRepository::rebuild_projection() {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto expected = build_projection(connection);
  if (expected.chain_errors != 0) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Relationship event chain is invalid."};
  }
  connection.execute("DELETE FROM relationship_state");
  for (const auto &[user, row] : expected.rows) {
    auto insert = connection.prepare(
        "INSERT INTO relationship_state (subject_user_id,familiarity,esteem,"
        "mirth,reliability,wariness,interaction_count,last_interaction_at_ms,"
        "projection_version,updated_at_ms) VALUES (?,?,?,?,?,?,?,?,?,?)");
    insert.bind(1, user);
    insert.bind(2, static_cast<std::int64_t>(row.dimensions.familiarity));
    insert.bind(3, static_cast<std::int64_t>(row.dimensions.esteem));
    insert.bind(4, static_cast<std::int64_t>(row.dimensions.mirth));
    insert.bind(5, static_cast<std::int64_t>(row.dimensions.reliability));
    insert.bind(6, static_cast<std::int64_t>(row.dimensions.wariness));
    insert.bind(7, static_cast<std::int64_t>(row.interaction_count));
    insert.bind(8, row.last_interaction_at_ms);
    insert.bind(9, static_cast<std::int64_t>(row.version));
    insert.bind(10, row.updated_at_ms);
    insert.execute();
  }
  const auto result = compare_projection(connection, expected);
  if (!result.valid) {
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Relationship projection rebuild verification failed."};
  }
  transaction.commit();
  return result;
}

} // namespace sanguinius::persistence
