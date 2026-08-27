#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"

#include "sanguinius/chronicle.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/relationships.hpp"
#include "sqlite_durable_work_writes.hpp"
#include "sqlite_relationship_writes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sanguinius::persistence {
namespace {

struct Observation {
  std::string event_id;
  std::string event_type;
  std::string visibility;
  bool is_test{};
};

struct ExpectedTitle {
  std::string source_event_id;
  std::string user_id;
  std::string title;
  std::int64_t occurred_at_ms{};
};

struct TitleAccumulator {
  std::int64_t win_streak{};
  std::int64_t loss_streak{};
  std::int64_t settled_house{};
};

[[nodiscard]] std::string title_key(const std::string_view user_id,
                                    const std::string_view title) {
  return std::string{user_id} + "\n" + std::string{title};
}

[[nodiscard]] std::unordered_map<std::string, ExpectedTitle>
expected_titles(SqliteConnection &connection) {
  auto query = connection.prepare(
      "SELECT player.source_event_id,player.user_id,player.result,"
      "player.wager_kind,player.occurred_at_ms,player.baseline FROM "
      "tarot_player_event player "
      "JOIN tarot_event_order event_order ON event_order.event_id=player."
      "source_event_id WHERE player.is_test=0 ORDER BY player.user_id,"
      "event_order.sequence_id");
  std::unordered_map<std::string, TitleAccumulator> accumulators;
  std::unordered_map<std::string, ExpectedTitle> expected;
  auto disabled = connection.prepare(
      "SELECT 1 FROM tarot_integration_effect_receipt WHERE "
      "source_event_id=? AND sink_kind='title' AND sink_key=? AND "
      "sink_reference='chronicle_disabled'");
  while (query.step()) {
    const auto event_id = query.column_text(0);
    const auto user_id = query.column_text(1);
    const auto result = query.column_text(2);
    const auto wager_kind = query.column_text(3);
    const auto occurred_at_ms = query.column_int64(4);
    const bool baseline = query.column_int64(5) != 0;
    auto &value = accumulators[user_id];
    const auto add = [&](const std::string_view title) {
      disabled.reset();
      disabled.bind(1, event_id);
      disabled.bind(2, user_id + ":" + std::string{title});
      if (disabled.step())
        return;
      expected.try_emplace(title_key(user_id, title),
                           ExpectedTitle{.source_event_id = event_id,
                                         .user_id = user_id,
                                         .title = std::string{title},
                                         .occurred_at_ms = occurred_at_ms});
    };
    if (!baseline && result == "win" && value.win_streak + 1 == 3)
      add("Favored of the Cast Die");
    if (!baseline && result == "win" && value.loss_streak >= 3)
      add("Bearer of the Returning Dawn");
    if (!baseline && wager_kind == "house" && value.settled_house + 1 == 10)
      add("Keeper of the Last Standard");
    if (result == "win") {
      ++value.win_streak;
      value.loss_streak = 0;
    } else if (result == "loss") {
      ++value.loss_streak;
      value.win_streak = 0;
    }
    if (wager_kind == "house")
      ++value.settled_house;
  }
  return expected;
}

[[nodiscard]] std::string
title_state_for_grant(const std::string_view grant_state) {
  if (grant_state == "active")
    return "approved";
  if (grant_state == "rejected")
    return "rejected";
  if (grant_state == "revoked")
    return "revoked";
  return "proposed";
}

[[nodiscard]] TarotIntegrationProjectionReport
check_title_projection(SqliteConnection &connection) {
  const auto expected = expected_titles(connection);
  std::size_t actual_count{};
  std::size_t mismatches{};
  std::unordered_map<std::string, bool> seen;
  auto actual = connection.prepare(
      "SELECT source.source_event_id,source.user_id,source.title_name,"
      "source.title_definition_id,source.state,grant.state FROM "
      "tarot_title_source source LEFT JOIN chronicle_title_grant grant ON "
      "grant.definition_id=source.title_definition_id");
  while (actual.step()) {
    ++actual_count;
    const auto key = title_key(actual.column_text(1), actual.column_text(2));
    seen[key] = true;
    const auto found = expected.find(key);
    if (found == expected.end() ||
        found->second.source_event_id != actual.column_text(0)) {
      ++mismatches;
      continue;
    }
    if (!actual.column_is_null(3)) {
      if (actual.column_is_null(5) ||
          actual.column_text(4) != title_state_for_grant(actual.column_text(5)))
        ++mismatches;
    } else if (actual.column_text(4) == "suppressed") {
      auto receipt = connection.prepare(
          "SELECT 1 FROM tarot_integration_effect_receipt WHERE "
          "source_event_id=? AND sink_kind='title' AND sink_key=? AND "
          "sink_reference='recipient_opted_out'");
      receipt.bind(1, found->second.source_event_id);
      receipt.bind(2, found->second.user_id + ":" + found->second.title);
      if (!receipt.step())
        ++mismatches;
    } else if (actual.column_text(4) != "proposed") {
      ++mismatches;
    }
  }
  for (const auto &[key, ignored] : expected) {
    static_cast<void>(ignored);
    if (!seen.contains(key))
      ++mismatches;
  }
  return {.valid = mismatches == 0,
          .expected_title_count = expected.size(),
          .title_source_count = actual_count,
          .title_mismatch_count = mismatches};
}

[[nodiscard]] TarotIntegrationReport
inspect_unlocked(SqliteConnection &connection) {
  auto query =
      connection.prepare("SELECT count(*) FILTER(WHERE state='pending'),"
                         "count(*) FILTER(WHERE state='complete'),"
                         "count(*) FILTER(WHERE state='suppressed'),"
                         "count(*) FILTER(WHERE state='failed') FROM "
                         "tarot_integration_observation");
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot integration report failed."};
  auto effects = connection.prepare(
      "SELECT count(*) FROM tarot_integration_effect_receipt");
  if (!effects.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot integration effects report failed."};
  return {.pending = static_cast<std::size_t>(query.column_int64(0)),
          .completed = static_cast<std::size_t>(query.column_int64(1)),
          .suppressed = static_cast<std::size_t>(query.column_int64(2)),
          .failed = static_cast<std::size_t>(query.column_int64(3)),
          .effects = static_cast<std::size_t>(effects.column_int64(0))};
}

[[nodiscard]] std::string next_id(const std::function<std::string()> &factory) {
  if (!factory)
    throw std::invalid_argument{"Integration ID factory is required."};
  auto result = factory();
  if (!valid_uuid_v4(result))
    throw std::invalid_argument{"Integration IDs must be UUIDv4 values."};
  return result;
}

[[nodiscard]] bool effect_exists(SqliteConnection &connection,
                                 const std::string_view event_id,
                                 const std::string_view sink,
                                 const std::string_view key) {
  auto query = connection.prepare(
      "SELECT 1 FROM tarot_integration_effect_receipt WHERE source_event_id=? "
      "AND sink_kind=? AND sink_key=?");
  query.bind(1, event_id);
  query.bind(2, sink);
  query.bind(3, key);
  return query.step();
}

void effect(SqliteConnection &connection, const std::string_view event_id,
            const std::string_view sink, const std::string_view key,
            const std::optional<std::string_view> reference,
            const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO tarot_integration_effect_receipt(source_event_id,"
      "sink_kind,sink_key,sink_reference,created_at_ms) VALUES(?,?,?,?,?)");
  insert.bind(1, event_id);
  insert.bind(2, sink);
  insert.bind(3, key);
  if (reference)
    insert.bind(4, *reference);
  else
    insert.bind_null(4);
  insert.bind(5, now_ms);
  insert.execute();
}

[[nodiscard]] bool callback_consented(SqliteConnection &connection,
                                      const std::string_view user_id) {
  auto query = connection.prepare(
      "SELECT pref.appearance_callback_opt_in FROM discord_user user "
      "JOIN user_preference pref ON pref.user_id=user.user_id "
      "WHERE user.user_id=? AND user.is_bot=0");
  query.bind(1, user_id);
  return query.step() && query.column_int64(0) != 0;
}

[[nodiscard]] bool
all_callback_consented(SqliteConnection &connection,
                       const std::vector<std::string> &participants) {
  return !participants.empty() &&
         std::ranges::all_of(participants, [&](const std::string &participant) {
           return callback_consented(connection, participant);
         });
}

[[nodiscard]] bool
all_chronicle_consented(SqliteConnection &connection,
                        const std::vector<std::string> &participants) {
  if (participants.empty())
    return false;
  return std::ranges::all_of(participants, [&](const std::string &participant) {
    auto query = connection.prepare(
        "SELECT chronicle_opt_in FROM user_preference WHERE user_id=?");
    query.bind(1, participant);
    return query.step() && query.column_int64(0) != 0;
  });
}

void create_appearance(
    SqliteConnection &connection, const Observation &observation,
    const std::vector<std::string> &participants, const std::string_view guild,
    const std::string_view channel, const std::string_view summary,
    const std::int64_t now_ms, const std::function<std::string()> &ids) {
  if (effect_exists(connection, observation.event_id, "appearance",
                    "tarot_event"))
    return;
  if (!all_callback_consented(connection, participants)) {
    effect(connection, observation.event_id, "appearance", "tarot_event",
           std::optional<std::string_view>{"consent_suppressed"}, now_ms);
    return;
  }
  const auto candidate_id = next_id(ids);
  auto insert = connection.prepare(
      "INSERT INTO tarot_appearance_candidate(candidate_id,source_event_id,"
      "candidate_type,actor_user_id,guild_id,channel_id,safe_summary,is_test,"
      "state,created_at_ms,expires_at_ms) "
      "VALUES(?,?,'tarot_event',?,?,?,?,?,'pending',?,?)");
  insert.bind(1, candidate_id);
  insert.bind(2, observation.event_id);
  insert.bind(3, participants.front());
  insert.bind(4, guild);
  insert.bind(5, channel);
  insert.bind(6, summary);
  insert.bind(7, observation.is_test ? 1LL : 0LL);
  insert.bind(8, now_ms);
  insert.bind(9, now_ms + 86'400'000);
  insert.execute();
  auto observation_insert = connection.prepare(
      "INSERT OR IGNORE INTO appearance_event_observation(source_event_id,"
      "event_type,aggregate_type,aggregate_id,guild_id,channel_id,actor_user_"
      "id,"
      "occurred_at_ms,recorded_at_ms,extraction_result,candidate_id,"
      "processed_at_ms) SELECT event_id,event_type,aggregate_type,aggregate_id,"
      "guild_id,channel_id,actor_user_id,occurred_at_ms,recorded_at_ms,NULL,"
      "NULL,NULL FROM event_journal WHERE event_id=?");
  observation_insert.bind(1, observation.event_id);
  observation_insert.execute();
  effect(connection, observation.event_id, "appearance", "tarot_event",
         candidate_id, now_ms);
}

void create_chronicle(SqliteConnection &connection,
                      const Observation &observation,
                      const std::vector<std::string> &participants,
                      const std::string_view actor,
                      const std::string_view title, const std::string_view body,
                      const bool chronicle_enabled, const std::int64_t now_ms,
                      const std::function<std::string()> &ids) {
  if (!chronicle_enabled || observation.is_test ||
      observation.visibility != "public" ||
      effect_exists(connection, observation.event_id, "chronicle", "proposal"))
    return;
  if (!all_chronicle_consented(connection, participants)) {
    effect(connection, observation.event_id, "chronicle", "proposal",
           std::optional<std::string_view>{"consent_suppressed"}, now_ms);
    return;
  }
  const auto proposal_id = next_id(ids);
  const auto approval_id = next_id(ids);
  const auto notice_id = next_id(ids);
  const auto notice_open_token_id = next_id(ids);
  const auto approve_token_id = next_id(ids);
  const auto decline_token_id = next_id(ids);
  const auto submitted_event_id = next_id(ids);
  auto source = connection.prepare(
      "SELECT guild_id,channel_id,occurred_at_ms FROM event_journal WHERE "
      "event_id=? AND channel_id IS NOT NULL");
  source.bind(1, observation.event_id);
  if (!source.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Tarot Chronicle source event is missing."};
  const auto guild_id = source.column_text(0);
  const auto channel_id = source.column_text(1);
  const auto occurred_at_ms = source.column_int64(2);
  auto owner = connection.prepare(
      "SELECT owner_user_id FROM guild_config WHERE guild_id=?");
  owner.bind(1, guild_id);
  if (!owner.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Tarot Chronicle reviewer is missing."};
  const auto owner_user_id = owner.column_text(0);

  auto entry = connection.prepare(
      "INSERT INTO chronicle_entry(entry_id,entry_type,title,body,visibility,"
      "status,occurred_at_ms,created_at_ms,created_by_user_id,submitted_at_ms,"
      "approved_at_ms,approved_by_user_id,retracted_at_ms,retracted_by_user_id,"
      "source_guild_id,source_channel_id,source_message_id,source_author_user_"
      "id,"
      "source_text,source_text_truncated,source_attachment_count,revision,"
      "source_kind,source_event_id) "
      "VALUES(?,'incident',?,?,'shared','proposed',"
      "?,?,?,?,NULL,NULL,NULL,NULL,?,?,NULL,?,?,0,0,2,'tarot_event',?)");
  entry.bind(1, proposal_id);
  entry.bind(2, title);
  entry.bind(3, body);
  entry.bind(4, occurred_at_ms);
  entry.bind(5, now_ms);
  entry.bind(6, actor);
  entry.bind(7, now_ms);
  entry.bind(8, guild_id);
  entry.bind(9, channel_id);
  entry.bind(10, actor);
  entry.bind(11, body);
  entry.bind(12, observation.event_id);
  entry.execute();
  const auto add_participant = [&](const std::string_view user,
                                   const std::string_view role) {
    auto participant = connection.prepare(
        "INSERT OR IGNORE INTO chronicle_participant(entry_id,user_id,role) "
        "VALUES(?,?,?)");
    participant.bind(1, proposal_id);
    participant.bind(2, user);
    participant.bind(3, role);
    participant.execute();
  };
  add_participant(actor, "proposer");
  add_participant(actor, "source_author");
  for (const auto &participant : participants)
    add_participant(participant, "subject");
  auto tag = connection.prepare(
      "INSERT INTO chronicle_tag(entry_id,tag) VALUES(?,'tarot-event')");
  tag.bind(1, proposal_id);
  tag.execute();

  auto staging = connection.prepare(
      "INSERT INTO tarot_chronicle_proposal(proposal_id,source_event_id,"
      "proposer_user_id,title,body,status,created_at_ms) "
      "VALUES(?,?,?,?,?,'submitted',?)");
  staging.bind(1, proposal_id);
  staging.bind(2, observation.event_id);
  staging.bind(3, actor);
  staging.bind(4, title);
  staging.bind(5, body);
  staging.bind(6, now_ms);
  staging.execute();

  const auto expiry_ms = now_ms + 7LL * 86'400'000;
  const auto component = [](const std::string_view token) {
    return std::string{chronicle_component_prefix} + std::string{token};
  };
  const auto notice_payload = nlohmann::json{
      {"title", "Chronicle approval requested"},
      {"body", "**" + std::string{title} + "**\n" + std::string{body} +
                   "\nSource: deterministic public Tarot event. Approve "
                   "only if this should become canon."},
      {"actions",
       nlohmann::json::array(
           {{{"custom_id", component(approve_token_id)}, {"label", "Approve"}},
            {{"custom_id", component(decline_token_id)},
             {"label",
              "Decline"}}})}}.dump();
  auto notice = connection.prepare(
      "INSERT INTO pending_notice(notice_id,target_user_id,notice_type,"
      "payload_json,source_aggregate_type,source_aggregate_id,state,"
      "expires_at_ms,idempotency_key,created_at_ms) "
      "VALUES(?,?,'chronicle_approval',"
      "?,'chronicle_entry',?,'pending',?,?,?)");
  notice.bind(1, notice_id);
  notice.bind(2, owner_user_id);
  notice.bind(3, notice_payload);
  notice.bind(4, proposal_id);
  notice.bind(5, expiry_ms);
  notice.bind(6, "notice:chronicle:tarot:" + observation.event_id);
  notice.bind(7, now_ms);
  notice.execute();
  const auto insert_token = [&](const std::string_view token_id,
                                const std::string_view action,
                                const std::string_view entity_type,
                                const std::string_view entity_id,
                                const std::int64_t revision,
                                const std::string_view purpose) {
    auto token = connection.prepare(
        "INSERT INTO interaction_token(token_id,token_version,interaction_kind,"
        "action,entity_type,entity_id,expected_user_id,guild_id,channel_id,"
        "state,"
        "expires_at_ms,used_at_ms,idempotency_key,created_at_ms,"
        "expected_entity_revision) VALUES(?,1,'button',?,?,?,?,?,?,'active',"
        "?,NULL,?,?,?)");
    token.bind(1, token_id);
    token.bind(2, action);
    token.bind(3, entity_type);
    token.bind(4, entity_id);
    token.bind(5, owner_user_id);
    token.bind(6, guild_id);
    token.bind(7, channel_id);
    token.bind(8, expiry_ms);
    token.bind(9, "token:chronicle:tarot:" + observation.event_id + ":" +
                      std::string{purpose});
    token.bind(10, now_ms);
    token.bind(11, revision);
    token.execute();
  };
  insert_token(notice_open_token_id, "notice.open", "pending_notice", notice_id,
               1, "notice");
  auto approval = connection.prepare(
      "INSERT INTO chronicle_approval(approval_id,entry_id,reviewer_user_id,"
      "approval_role,state,entry_revision,notice_id,requested_at_ms) "
      "VALUES(?,?,?,'owner','pending',2,?,?)");
  approval.bind(1, approval_id);
  approval.bind(2, proposal_id);
  approval.bind(3, owner_user_id);
  approval.bind(4, notice_id);
  approval.bind(5, now_ms);
  approval.execute();
  insert_token(approve_token_id, "chronicle.entry.approve",
               "chronicle_approval", approval_id, 2, "approve");
  insert_token(decline_token_id, "chronicle.entry.decline",
               "chronicle_approval", approval_id, 2, "decline");
  const auto event_inserted = detail::insert_event_uncommitted(
      connection,
      EventJournalEntry{.event_id = submitted_event_id,
                        .event_type = "chronicle.proposal_submitted.v1",
                        .aggregate_type = "chronicle_entry",
                        .aggregate_id = proposal_id,
                        .actor_user_id = DiscordSnowflake::parse(actor),
                        .guild_id = DiscordSnowflake::parse(guild_id),
                        .channel_id = DiscordSnowflake::parse(channel_id),
                        .source_message_id = std::nullopt,
                        .occurred_at_ms = now_ms,
                        .recorded_at_ms = now_ms,
                        .correlation_id = "tarot-integration",
                        .causation_id = observation.event_id,
                        .idempotency_key =
                            "event:chronicle:tarot:" + observation.event_id,
                        .payload_json = nlohmann::json{
                            {"entry_id", proposal_id},
                            {"status", "proposed"},
                            {"visibility", "shared"},
                            {"revision", 2},
                            {"source_kind", "tarot_event"},
                            {"test", false}}.dump()});
  if (!event_inserted)
    throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                        SQLITE_CONSTRAINT,
                        "Tarot Chronicle submission event was not fresh."};
  effect(connection, observation.event_id, "chronicle", "proposal", proposal_id,
         now_ms);
}

void create_relationship(
    SqliteConnection &connection, const Observation &observation,
    const std::string_view actor, const std::string_view reason,
    const RelationshipSourceKind policy, const std::int64_t occurred_at_ms,
    const bool chronicle_enabled, const std::int64_t now_ms,
    const std::function<std::string()> &ids) {
  if (!chronicle_enabled || observation.is_test ||
      effect_exists(connection, observation.event_id, "relationship", actor))
    return;
  const auto inserted = detail::insert_relationship_event_uncommitted(
      connection, next_id(ids), observation.event_id, observation.event_type,
      reason, DiscordSnowflake::parse(actor), relationship_policy(policy),
      occurred_at_ms, now_ms);
  effect(connection, observation.event_id, "relationship", actor,
         inserted ? std::optional<std::string_view>{"applied"}
                  : std::optional<std::string_view>{"replay"},
         now_ms);
}

void propose_title_source(SqliteConnection &connection,
                          const Observation &observation,
                          const std::string_view user_id,
                          const std::string_view title,
                          const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT OR IGNORE INTO tarot_title_source(source_event_id,user_id,"
      "title_name,title_definition_id,state,created_at_ms) "
      "VALUES(?,?,?,NULL,'proposed',?)");
  insert.bind(1, observation.event_id);
  insert.bind(2, user_id);
  insert.bind(3, title);
  insert.bind(4, now_ms);
  insert.execute();
}

struct PlayerPrefix {
  std::int64_t win_streak{};
  std::int64_t loss_streak{};
  std::int64_t settled_house{};
};

[[nodiscard]] PlayerPrefix player_prefix(SqliteConnection &connection,
                                         const std::string_view user_id,
                                         const std::string_view event_id) {
  auto query = connection.prepare(
      "SELECT player.result,player.wager_kind FROM tarot_player_event player "
      "JOIN tarot_event_order player_order ON player_order.event_id=player."
      "source_event_id JOIN tarot_event_order current_order ON current_order."
      "event_id=? WHERE player.user_id=? AND player_order.sequence_id<"
      "current_order.sequence_id ORDER BY player_order.sequence_id");
  query.bind(1, event_id);
  query.bind(2, user_id);
  PlayerPrefix result;
  while (query.step()) {
    const auto event_result = query.column_text(0);
    if (event_result == "win") {
      ++result.win_streak;
      result.loss_streak = 0;
    } else if (event_result == "loss") {
      ++result.loss_streak;
      result.win_streak = 0;
    }
    if (query.column_text(1) == "house")
      ++result.settled_house;
  }
  return result;
}

void rebuild_user_projection(SqliteConnection &connection,
                             const std::string_view user_id,
                             const std::int64_t now_ms) {
  auto query = connection.prepare(
      "SELECT player.source_event_id,player.result,player.wager_kind FROM "
      "tarot_player_event player JOIN tarot_event_order event_order ON "
      "event_order.event_id=player.source_event_id WHERE player.user_id=? "
      "ORDER BY event_order.sequence_id");
  query.bind(1, user_id);
  std::int64_t wins{};
  std::int64_t losses{};
  std::int64_t win_streak{};
  std::int64_t loss_streak{};
  std::int64_t settled_house{};
  std::string last_event;
  while (query.step()) {
    last_event = query.column_text(0);
    const auto result = query.column_text(1);
    if (result == "win") {
      ++wins;
      ++win_streak;
      loss_streak = 0;
    } else if (result == "loss") {
      ++losses;
      ++loss_streak;
      win_streak = 0;
    }
    if (query.column_text(2) == "house")
      ++settled_house;
  }
  if (last_event.empty()) {
    auto remove =
        connection.prepare("DELETE FROM tarot_player_stats WHERE user_id=?");
    remove.bind(1, user_id);
    remove.execute();
    return;
  }
  auto update = connection.prepare(
      "INSERT INTO tarot_player_stats(user_id,wins,losses,current_win_streak,"
      "current_loss_streak,settled_house_wagers,last_event_id,rebuilt_at_ms) "
      "VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
      "wins=excluded.wins,losses=excluded.losses,current_win_streak=excluded."
      "current_win_streak,current_loss_streak=excluded.current_loss_streak,"
      "settled_house_wagers=excluded.settled_house_wagers,last_event_id="
      "excluded."
      "last_event_id,rebuilt_at_ms=excluded.rebuilt_at_ms");
  update.bind(1, user_id);
  update.bind(2, wins);
  update.bind(3, losses);
  update.bind(4, win_streak);
  update.bind(5, loss_streak);
  update.bind(6, settled_house);
  update.bind(7, last_event);
  update.bind(8, now_ms);
  update.execute();
}

void update_player_stats(
    SqliteConnection &connection, const Observation &observation,
    const std::string_view user_id, const std::string_view result,
    const std::string_view wager_kind, const std::int64_t occurred_at_ms,
    const bool chronicle_enabled, const std::int64_t now_ms) {
  if (observation.is_test ||
      effect_exists(connection, observation.event_id, "stats", user_id))
    return;
  const auto prefix = player_prefix(connection, user_id, observation.event_id);
  auto event = connection.prepare(
      "INSERT OR IGNORE INTO tarot_player_event(source_event_id,user_id,result,"
      "wager_kind,is_test,baseline,occurred_at_ms) VALUES(?,?,?,?,0,0,?)");
  event.bind(1, observation.event_id);
  event.bind(2, user_id);
  event.bind(3, result);
  event.bind(4, wager_kind);
  event.bind(5, occurred_at_ms);
  event.execute();
  rebuild_user_projection(connection, user_id, now_ms);
  const auto handle_title = [&](const std::string_view title) {
    if (chronicle_enabled) {
      propose_title_source(connection, observation, user_id, title, now_ms);
      return;
    }
    effect(connection, observation.event_id, "title",
           std::string{user_id} + ":" + std::string{title},
           std::optional<std::string_view>{"chronicle_disabled"}, now_ms);
  };
  if (result == "win" && prefix.win_streak + 1 == 3)
    handle_title("Favored of the Cast Die");
  if (result == "win" && prefix.loss_streak >= 3)
    handle_title("Bearer of the Returning Dawn");
  if (wager_kind == "house" && prefix.settled_house + 1 == 10)
    handle_title("Keeper of the Last Standard");
  effect(connection, observation.event_id, "stats", user_id,
         std::optional<std::string_view>{"applied"}, now_ms);
}

[[nodiscard]] bool
sync_titles(SqliteConnection &connection, const Observation &observation,
            const std::string_view guild_id, const std::string_view channel_id,
            const bool chronicle_enabled, const std::int64_t now_ms,
            const std::function<std::string()> &ids) {
  if (!chronicle_enabled)
    return false;
  auto query = connection.prepare(
      "SELECT user_id,title_name FROM tarot_title_source WHERE "
      "source_event_id=? AND state='proposed' AND title_definition_id IS NULL "
      "ORDER BY user_id,title_name");
  query.bind(1, observation.event_id);
  std::vector<std::pair<std::string, std::string>> titles;
  while (query.step())
    titles.emplace_back(query.column_text(0), query.column_text(1));
  bool created{};
  for (const auto &[user_id, title] : titles) {
    if (effect_exists(connection, observation.event_id, "title",
                      user_id + ":" + title))
      continue;
    auto preference = connection.prepare(
        "SELECT chronicle_opt_in FROM user_preference WHERE user_id=?");
    preference.bind(1, user_id);
    if (!preference.step() || preference.column_int64(0) == 0) {
      auto suppress = connection.prepare(
          "UPDATE tarot_title_source SET state='suppressed' WHERE "
          "source_event_id=? AND user_id=? AND title_name=? AND "
          "state='proposed' AND title_definition_id IS NULL");
      suppress.bind(1, observation.event_id);
      suppress.bind(2, user_id);
      suppress.bind(3, title);
      suppress.execute();
      effect(connection, observation.event_id, "title", user_id + ":" + title,
             std::optional<std::string_view>{"recipient_opted_out"}, now_ms);
      continue;
    }
    auto owner = connection.prepare(
        "SELECT owner_user_id FROM guild_config WHERE guild_id=?");
    owner.bind(1, guild_id);
    if (!owner.step())
      throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                          SQLITE_SCHEMA, "Tarot title owner is missing."};
    const auto definition_id = next_id(ids);
    const auto grant_id = next_id(ids);
    const auto event_id = next_id(ids);
    auto definition = connection.prepare(
        "INSERT INTO chronicle_title_definition(definition_id,title,"
        "description,provenance,session_id,supporting_entry_id,"
        "proposed_by_user_id,created_at_ms) VALUES(?,?,?,'tarot_system',"
        "NULL,NULL,?,?)");
    definition.bind(1, definition_id);
    definition.bind(2, title);
    definition.bind(3, "Proposed by the deterministic Tarot record.");
    definition.bind(4, owner.column_text(0));
    definition.bind(5, now_ms);
    definition.execute();
    auto grant = connection.prepare(
        "INSERT INTO chronicle_title_grant(grant_id,definition_id,"
        "recipient_user_id,state,featured,revision,source_idempotency_key,"
        "proposed_at_ms) VALUES(?,?,?,'proposed',0,1,?,?)");
    grant.bind(1, grant_id);
    grant.bind(2, definition_id);
    grant.bind(3, user_id);
    grant.bind(4, "title:tarot:" + observation.event_id + ":" + user_id + ":" +
                      title);
    grant.bind(5, now_ms);
    grant.execute();
    auto title_event = connection.prepare(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,source_message_id,"
        "occurred_at_ms,recorded_at_ms,correlation_id,causation_id,"
        "idempotency_key,payload_json) VALUES(?,'chronicle.title_proposed.v1',"
        "'chronicle_title',?,?,?,?,NULL,?,?,'tarot-integration',?,?,'{\""
        "provenance\":\"tarot_system\"}')");
    title_event.bind(1, event_id);
    title_event.bind(2, grant_id);
    title_event.bind(3, owner.column_text(0));
    title_event.bind(4, guild_id);
    title_event.bind(5, channel_id);
    title_event.bind(6, now_ms);
    title_event.bind(7, now_ms);
    title_event.bind(8, observation.event_id);
    title_event.bind(9, "event:title:tarot:" + observation.event_id + ":" +
                            user_id + ":" + title);
    title_event.execute();
    auto link = connection.prepare(
        "UPDATE tarot_title_source SET title_definition_id=? WHERE "
        "source_event_id=? AND user_id=? AND title_name=? AND "
        "title_definition_id IS NULL");
    link.bind(1, definition_id);
    link.bind(2, observation.event_id);
    link.bind(3, user_id);
    link.bind(4, title);
    link.execute();
    effect(connection, observation.event_id, "title", user_id + ":" + title,
           grant_id, now_ms);
    created = true;
  }
  return created;
}

void process_draw(SqliteConnection &connection, const Observation &observation,
                  const std::int64_t now_ms,
                  const std::function<std::string()> &ids) {
  auto query = connection.prepare(
      "SELECT draw.user_id,draw.guild_id,draw.channel_id,card.name "
      "FROM tarot_card_draw draw JOIN tarot_card_definition card "
      "ON card.catalog_version=draw.catalog_version AND "
      "card.ordinal=draw.card_ordinal "
      "WHERE draw.event_id=?");
  query.bind(1, observation.event_id);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Observed Tarot draw is missing."};
  if (observation.visibility != "public")
    return;
  const auto actor = query.column_text(0);
  const auto guild = query.column_text(1);
  const auto channel = query.column_text(2);
  const auto card = query.column_text(3);
  create_appearance(connection, observation, {actor}, guild, channel,
                    "A public Emperor's Tarot draw revealed " + card + ".",
                    now_ms, ids);
}

void require_prior_player_observations_complete(
    SqliteConnection &connection, const Observation &observation,
    const std::string_view user_id) {
  if (observation.is_test)
    return;
  auto pending = connection.prepare(
      "SELECT 1 FROM tarot_integration_observation prior JOIN event_journal "
      "event ON event.event_id=prior.source_event_id JOIN tarot_event_order "
      "prior_order ON prior_order.event_id=prior.source_event_id JOIN "
      "tarot_event_order current_order ON current_order.event_id=? WHERE "
      "prior.is_test=0 "
      "AND prior.state IN ('pending','failed','suppressed') "
      "AND prior.event_type IN "
      "('tarot.house_resolved.v1','tarot.house_voided.v1',"
      "'tarot.wager_resolved.v1','tarot.wager_voided.v1') AND "
      "prior_order.sequence_id<current_order.sequence_id "
      "AND (EXISTS(SELECT 1 FROM tarot_house_wager house WHERE "
      "house.terminal_event_id=prior.source_event_id AND house.user_id=?) OR "
      "EXISTS(SELECT 1 FROM tarot_wager peer WHERE "
      "peer.wager_id=event.aggregate_id "
      "AND (peer.creator_user_id=? OR peer.target_user_id=?))) LIMIT 1");
  pending.bind(1, observation.event_id);
  pending.bind(2, user_id);
  pending.bind(3, user_id);
  pending.bind(4, user_id);
  if (pending.step())
    throw std::runtime_error{
        "An earlier Tarot player observation is awaiting integration."};
}

void process_house(SqliteConnection &connection, const Observation &observation,
                   const bool chronicle_enabled, const std::int64_t now_ms,
                   const std::function<std::string()> &ids) {
  auto query = connection.prepare(
      "SELECT "
      "wager.user_id,wager.guild_id,wager.channel_id,wager.template_slug,"
      "wager.result,wager.recovery,event.occurred_at_ms FROM tarot_house_wager "
      "wager "
      "JOIN event_journal event ON event.event_id=? WHERE "
      "wager.terminal_event_id=?");
  query.bind(1, observation.event_id);
  query.bind(2, observation.event_id);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Observed House settlement is missing."};
  const auto actor = query.column_text(0);
  const auto guild = query.column_text(1);
  const auto channel = query.column_text(2);
  const auto template_slug = query.column_text(3);
  const auto result = query.column_text(4);
  const auto recovery = query.column_int64(5) != 0;
  const auto occurred_at = query.column_int64(6);
  require_prior_player_observations_complete(connection, observation, actor);
  update_player_stats(connection, observation, actor, result, "house",
                      occurred_at, chronicle_enabled, now_ms);
  if (!recovery && result != "void") {
    const auto honored = template_slug == "heralds-call" && result == "win";
    create_relationship(connection, observation, actor,
                        honored ? "tarot.honored" : "tarot.resolved",
                        honored ? RelationshipSourceKind::tarot_honored
                                : RelationshipSourceKind::tarot_resolved,
                        occurred_at, chronicle_enabled, now_ms, ids);
  }
  const bool title_created = sync_titles(
      connection, observation, guild, channel, chronicle_enabled, now_ms, ids);
  if (observation.visibility == "public" && !recovery) {
    const auto summary = "A public House augury, " + template_slug +
                         ", settled as a " + result + ".";
    create_appearance(connection, observation, {actor}, guild, channel, summary,
                      now_ms, ids);
    if (template_slug == "last-standard")
      create_chronicle(connection, observation, {actor}, actor,
                       "The Last Standard", summary, chronicle_enabled, now_ms,
                       ids);
    if (title_created)
      create_chronicle(connection, observation, {actor}, actor,
                       "A Tarot title threshold", summary, chronicle_enabled,
                       now_ms, ids);
  }
}

void process_peer(SqliteConnection &connection, const Observation &observation,
                  const bool chronicle_enabled, const std::int64_t now_ms,
                  const std::function<std::string()> &ids) {
  auto query = connection.prepare(
      "SELECT wager.creator_user_id,wager.target_user_id,wager.guild_id,"
      "wager.channel_id,wager.visibility,event.occurred_at_ms,"
      "COALESCE(resolution.authority,'mutual'),COALESCE(resolution.result,'"
      "void'),"
      "EXISTS(SELECT 1 FROM tarot_wager_action action WHERE action.wager_id="
      "wager.wager_id AND action.action='disputed') FROM tarot_wager wager "
      "JOIN event_journal event ON event.event_id=? LEFT JOIN "
      "tarot_wager_resolution resolution "
      "ON resolution.event_id=event.event_id WHERE "
      "wager.wager_id=event.aggregate_id");
  query.bind(1, observation.event_id);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Observed peer settlement is missing."};
  const auto creator = query.column_text(0);
  const auto target = query.column_text(1);
  const auto occurred_at = query.column_int64(5);
  const auto resolution = query.column_text(7);
  require_prior_player_observations_complete(connection, observation, creator);
  require_prior_player_observations_complete(connection, observation, target);
  const bool honored = query.column_text(6) == "mutual" && resolution != "void";
  const bool disputed = query.column_int64(8) != 0;
  update_player_stats(connection, observation, creator,
                      resolution == "creator"  ? "win"
                      : resolution == "target" ? "loss"
                                               : "void",
                      "peer", occurred_at, chronicle_enabled, now_ms);
  update_player_stats(connection, observation, target,
                      resolution == "target"    ? "win"
                      : resolution == "creator" ? "loss"
                                                : "void",
                      "peer", occurred_at, chronicle_enabled, now_ms);
  const bool title_created =
      sync_titles(connection, observation, query.column_text(2),
                  query.column_text(3), chronicle_enabled, now_ms, ids);
  create_relationship(connection, observation, creator,
                      honored ? "tarot.honored" : "tarot.resolved",
                      honored ? RelationshipSourceKind::tarot_honored
                              : RelationshipSourceKind::tarot_resolved,
                      occurred_at, chronicle_enabled, now_ms, ids);
  create_relationship(connection, observation, target,
                      honored ? "tarot.honored" : "tarot.resolved",
                      honored ? RelationshipSourceKind::tarot_honored
                              : RelationshipSourceKind::tarot_resolved,
                      occurred_at, chronicle_enabled, now_ms, ids);
  if (observation.visibility == "public") {
    create_appearance(connection, observation, {creator, target},
                      query.column_text(2), query.column_text(3),
                      "A public peer Fate wager reached settlement.", now_ms,
                      ids);
    if (disputed)
      create_chronicle(connection, observation, {creator, target}, creator,
                       "A disputed Fate wager was settled",
                       "A public disputed peer wager reached settlement.",
                       chronicle_enabled, now_ms, ids);
    else if (title_created)
      create_chronicle(connection, observation, {creator, target}, creator,
                       "A Tarot title threshold",
                       "A public Tarot record reached a title threshold.",
                       chronicle_enabled, now_ms, ids);
  }
}

} // namespace

SqliteTarotIntegrationRepository::SqliteTarotIntegrationRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{
        "SQLite Tarot integration context is required."};
}

void SqliteTarotIntegrationRepository::ensure_schedule(
    const std::int64_t now_ms, std::string job_id) {
  if (!valid_uuid_v4(job_id) || now_ms < 0)
    throw std::invalid_argument{"Tarot integration schedule is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT job_id,job_type,state FROM scheduled_job WHERE idempotency_key="
      "'job:tarot-integration:singleton'");
  if (existing.step()) {
    if (existing.column_text(1) != tarot_integration_job_type)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "Tarot integration schedule version conflicts."};
    const auto state = existing.column_text(2);
    if (state == "dead" || state == "cancelled" || state == "completed") {
      auto rearm = connection.prepare(
          "UPDATE scheduled_job SET "
          "state='pending',attempt_count=0,due_at_ms=?,"
          "lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,"
          "last_error_code=NULL,completed_at_ms=NULL,terminal_at_ms=NULL,"
          "updated_at_ms=max(?,updated_at_ms) WHERE job_id=?");
      rearm.bind(1, now_ms);
      rearm.bind(2, now_ms);
      rearm.bind(3, existing.column_text(0));
      rearm.execute();
    }
    transaction.commit();
    return;
  }
  const ScheduledJobEnqueue job{
      .job_id = std::move(job_id),
      .job_type = std::string{tarot_integration_job_type},
      .aggregate_type = "tarot_integration",
      .aggregate_id = "singleton",
      .due_at_ms = now_ms,
      .max_attempts = 10,
      .idempotency_key = "job:tarot-integration:singleton",
      .created_at_ms = now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(
      connection, job,
      detail::encode_tarot_integration_scan_payload(
          TarotIntegrationScanJobPayload{.schedule_key = "singleton"},
          "tarot-integration-recurring", std::nullopt)));
  transaction.commit();
}

TarotIntegrationReport SqliteTarotIntegrationRepository::scan(
    const std::int64_t now_ms, const std::size_t limit,
    std::function<std::string()> ids,
    const TarotIntegrationSinkPolicy sink_policy) {
  if (limit == 0 || limit > 100)
    throw std::invalid_argument{"Tarot integration batch is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto query = connection.prepare(
      "SELECT observation.source_event_id,observation.event_type,"
      "observation.visibility,observation.is_test FROM "
      "tarot_integration_observation observation JOIN tarot_event_order "
      "event_order ON event_order.event_id=observation.source_event_id WHERE "
      "(observation.state IN ('pending','failed') OR (observation.state="
      "'suppressed' AND observation.is_test=0 AND COALESCE(observation."
      "last_error,'')<>'integration_disabled')) AND (observation.is_test=0 "
      "OR observation.attempts<100) AND observation.next_attempt_at_ms<=? "
      "ORDER BY event_order.sequence_id LIMIT ?");
  query.bind(1, now_ms);
  query.bind(2, static_cast<std::int64_t>(limit));
  std::vector<Observation> work;
  while (query.step())
    work.push_back({query.column_text(0), query.column_text(1),
                    query.column_text(2), query.column_int64(3) != 0});
  for (const auto &observation : work) {
    try {
      Transaction transaction{connection, TransactionMode::immediate};
      if (observation.event_type == "tarot.draw_created.v1")
        process_draw(connection, observation, now_ms, ids);
      else if (observation.event_type == "tarot.house_resolved.v1" ||
               observation.event_type == "tarot.house_voided.v1")
        process_house(connection, observation, sink_policy.chronicle_enabled,
                      now_ms, ids);
      else if (observation.event_type == "tarot.wager_resolved.v1" ||
               observation.event_type == "tarot.wager_voided.v1")
        process_peer(connection, observation, sink_policy.chronicle_enabled,
                     now_ms, ids);
      auto complete = connection.prepare(
          "UPDATE tarot_integration_observation SET state='complete',"
          "attempts=attempts+1,last_error=NULL,processed_at_ms=? "
          "WHERE source_event_id=? AND state IN "
          "('pending','failed','suppressed')");
      complete.bind(1, now_ms);
      complete.bind(2, observation.event_id);
      complete.execute();
      transaction.commit();
    } catch (const std::exception &error) {
      Transaction failure{connection, TransactionMode::immediate};
      auto update = connection.prepare(
          "UPDATE tarot_integration_observation SET state=CASE WHEN "
          "is_test=1 AND attempts>=99 "
          "THEN 'suppressed' ELSE 'failed' END,"
          "attempts=attempts+1,next_attempt_at_ms=?,last_error=?,processed_at_"
          "ms=NULL "
          "WHERE source_event_id=?");
      update.bind(1, now_ms + 60'000);
      const auto message = std::string{error.what()}.substr(0, 500);
      update.bind(2, message.empty() ? "integration failure" : message);
      update.bind(3, observation.event_id);
      update.execute();
      failure.commit();
    }
  }
  return inspect_unlocked(connection);
}

std::size_t
SqliteTarotIntegrationRepository::suppress_disabled(const std::int64_t now_ms,
                                                    const std::size_t limit) {
  if (now_ms < 0 || limit == 0 || limit > 50)
    throw std::invalid_argument{"Disabled Tarot integration batch is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto update = connection.prepare(
      "UPDATE tarot_integration_observation SET state='suppressed',"
      "attempts=attempts+1,next_attempt_at_ms=?,"
      "last_error='integration_disabled',processed_at_ms=max(created_at_ms,?) "
      "WHERE source_event_id IN (SELECT source_event_id FROM "
      "tarot_integration_observation WHERE (state IN "
      "('pending','processing','failed') OR (state='suppressed' AND "
      "is_test=0)) AND COALESCE(last_error,'')<>'integration_disabled' "
      "ORDER BY created_at_ms,source_event_id LIMIT ?)");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, static_cast<std::int64_t>(limit));
  update.execute();
  const auto suppressed = static_cast<std::size_t>(connection.changes());
  transaction.commit();
  return suppressed;
}

bool SqliteTarotIntegrationRepository::retry(
    const std::string_view source_event_id, const std::int64_t now_ms) {
  std::scoped_lock lock{context_->mutex()};
  auto update = context_->connection().prepare(
      "UPDATE tarot_integration_observation SET state='pending',"
      "attempts=0,next_attempt_at_ms=?,last_error=NULL,processed_at_ms=NULL "
      "WHERE source_event_id=? AND is_test=1 AND state IN "
      "('failed','suppressed')");
  update.bind(1, now_ms);
  update.bind(2, source_event_id);
  update.execute();
  return context_->connection().changes() == 1;
}

TarotIntegrationReport SqliteTarotIntegrationRepository::inspect() {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  return inspect_unlocked(connection);
}

TarotIntegrationProjectionReport
SqliteTarotIntegrationRepository::check_projection() {
  std::scoped_lock lock{context_->mutex()};
  return check_projection_unlocked();
}

TarotIntegrationProjectionReport
SqliteTarotIntegrationRepository::check_projection_unlocked() {
  return check_title_projection(context_->connection());
}

TarotIntegrationProjectionReport
SqliteTarotIntegrationRepository::rebuild_projection_uncommitted() {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  const auto expected = expected_titles(connection);
  connection.execute("DELETE FROM tarot_title_source");
  auto insert = connection.prepare(
      "INSERT INTO tarot_title_source(source_event_id,user_id,title_name,"
      "title_definition_id,state,created_at_ms) VALUES(?,?,?,?,?,?)");
  for (const auto &[key, title] : expected) {
    static_cast<void>(key);
    const auto source_idempotency = "title:tarot:" + title.source_event_id +
                                    ":" + title.user_id + ":" + title.title;
    auto grant = connection.prepare(
        "SELECT definition_id,state FROM chronicle_title_grant WHERE "
        "source_idempotency_key=?");
    grant.bind(1, source_idempotency);
    std::optional<std::string> definition_id;
    std::string state{"proposed"};
    if (grant.step()) {
      definition_id = grant.column_text(0);
      state = title_state_for_grant(grant.column_text(1));
    } else {
      auto suppressed = connection.prepare(
          "SELECT 1 FROM tarot_integration_effect_receipt WHERE "
          "source_event_id=? AND sink_kind='title' AND sink_key=? AND "
          "sink_reference='recipient_opted_out'");
      suppressed.bind(1, title.source_event_id);
      suppressed.bind(2, title.user_id + ":" + title.title);
      if (suppressed.step())
        state = "suppressed";
    }
    insert.bind(1, title.source_event_id);
    insert.bind(2, title.user_id);
    insert.bind(3, title.title);
    if (definition_id)
      insert.bind(4, *definition_id);
    else
      insert.bind_null(4);
    insert.bind(5, state);
    insert.bind(6, title.occurred_at_ms);
    insert.execute();
    insert.reset();
  }
  return check_projection_unlocked();
}

} // namespace sanguinius::persistence
