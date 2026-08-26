#include "sanguinius/persistence/sqlite_speech_repository.hpp"

#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include "sqlite_durable_work_writes.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr std::int64_t rolling_day_ms = 24 * 60 * 60 * 1'000;

void bind_optional(SqliteStatement &statement, const std::size_t index,
                   const std::optional<std::string> &value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.bind_null(index);
}

void bind_optional(SqliteStatement &statement, const std::size_t index,
                   const std::optional<std::int64_t> value) {
  if (value)
    statement.bind(index, *value);
  else
    statement.bind_null(index);
}

[[nodiscard]] std::optional<std::string> optional_text(SqliteStatement &query,
                                                       const int column) {
  return query.column_is_null(column)
             ? std::nullopt
             : std::optional<std::string>{query.column_text(column)};
}

[[nodiscard]] std::optional<std::int64_t>
optional_integer(SqliteStatement &query, const int column) {
  return query.column_is_null(column)
             ? std::nullopt
             : std::optional<std::int64_t>{query.column_int64(column)};
}

[[nodiscard]] SpeechState state_from(const std::string_view value) {
  if (value == "pending")
    return SpeechState::pending;
  if (value == "synthesizing")
    return SpeechState::synthesizing;
  if (value == "ready")
    return SpeechState::ready;
  if (value == "playing")
    return SpeechState::playing;
  if (value == "played")
    return SpeechState::played;
  if (value == "failed")
    return SpeechState::failed;
  if (value == "expired")
    return SpeechState::expired;
  if (value == "cancelled")
    return SpeechState::cancelled;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA, "Stored speech state is invalid."};
}

[[nodiscard]] SpeechPriority priority_from(const std::int64_t value) {
  switch (value) {
  case 100:
    return SpeechPriority::flavor;
  case 200:
    return SpeechPriority::event_narration;
  case 300:
    return SpeechPriority::interactive;
  case 400:
    return SpeechPriority::critical_control;
  default:
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Stored speech priority is invalid."};
  }
}

[[nodiscard]] SpeechItem item_from(SqliteStatement &query) {
  return {.speech_id = query.column_text(0),
          .voice_session_id = query.column_text(1),
          .source_event_id = optional_text(query, 2),
          .source_kind = query.column_text(3),
          .text = optional_text(query, 4),
          .text_hash = query.column_text(5),
          .scalar_count = static_cast<std::size_t>(query.column_int64(6)),
          .provider = query.column_text(7),
          .model = query.column_text(8),
          .voice = query.column_text(9),
          .priority = priority_from(query.column_int64(10)),
          .narration_rank = static_cast<std::uint8_t>(query.column_int64(11)),
          .state = state_from(query.column_text(12)),
          .revision = static_cast<std::size_t>(query.column_int64(13)),
          .earliest_at_ms = query.column_int64(14),
          .expires_at_ms = optional_integer(query, 15),
          .interruptible = query.column_int64(16) != 0,
          .deduplication_key = query.column_text(17),
          .provider_request_id = optional_text(query, 18),
          .cache_key = optional_text(query, 19),
          .cache_checksum = optional_text(query, 20),
          .marker = optional_text(query, 21),
          .duration_ms = optional_integer(query, 22),
          .attempt_count = static_cast<std::size_t>(query.column_int64(23)),
          .created_at_ms = query.column_int64(24),
          .terminal_at_ms = optional_integer(query, 25),
          .last_error_code = optional_text(query, 26)};
}

constexpr std::string_view item_columns{
    "speech_id,voice_session_id,source_event_id,source_kind,text,text_hash,"
    "scalar_count,provider,model,voice_id,priority,narration_rank,state,state_"
    "version,"
    "earliest_at_ms,expires_at_ms,interruptible,deduplication_key,"
    "provider_request_id,cache_key,cache_checksum,marker,duration_ms,"
    "attempt_count,created_at_ms,terminal_at_ms,last_error_code"};

[[nodiscard]] std::optional<SpeechItem>
load_item(SqliteConnection &connection, const std::string_view speech_id) {
  auto query = connection.prepare("SELECT " + std::string{item_columns} +
                                  " FROM speech_item WHERE speech_id=?");
  query.bind(1, speech_id);
  if (!query.step())
    return std::nullopt;
  auto item = item_from(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate stored speech item."};
  return item;
}

[[nodiscard]] bool active_session(SqliteConnection &connection,
                                  const std::string_view session_id,
                                  const std::string_view source_kind) {
  auto query =
      connection.prepare("SELECT state FROM voice_session WHERE session_id=?");
  query.bind(1, session_id);
  if (!query.step())
    return false;
  const auto state = query.column_text(0);
  return state == "ready" || state == "muted" ||
         (state == "leaving" && source_kind.ends_with("farewell"));
}

void validate_enqueue(const SpeechEnqueueRequest &request) {
  const auto normalized = normalize_tts_text(request.text.text);
  const auto bytes =
      std::as_bytes(std::span{normalized.text.data(), normalized.text.size()});
  if (request.speech_id.empty() || request.voice_session_id.empty() ||
      request.source_kind.empty() || request.source_kind.size() > 64 ||
      request.text.text.empty() || request.text.scalar_count == 0 ||
      request.text.scalar_count > maximum_tts_scalar_count ||
      request.text.scalar_count != normalized.scalar_count ||
      request.text.text != normalized.text ||
      request.text_hash != sha256_hex(bytes) ||
      !((request.provider == "openai" && request.model == "tts-1" &&
         request.voice == "onyx") ||
        (request.provider == "static" && request.model == "static-v1" &&
         request.voice == "onyx")) ||
      request.deduplication_key.empty() ||
      request.deduplication_key.size() > 160 || request.earliest_at_ms < 0 ||
      (request.priority == SpeechPriority::event_narration) !=
          (request.narration_rank > 0) ||
      request.narration_rank > 100 || request.created_at_ms < 0 ||
      request.earliest_at_ms < request.created_at_ms ||
      (request.expires_at_ms &&
       *request.expires_at_ms <= request.earliest_at_ms))
    throw std::invalid_argument{"Speech enqueue request is invalid."};
}

void insert_transition(SqliteConnection &connection,
                       const SpeechTransitionRequest &request,
                       const SpeechItem &previous) {
  auto insert = connection.prepare(
      "INSERT INTO speech_item_transition(transition_id,speech_id,from_state,"
      "to_state,from_version,to_version,reason,idempotency_key,occurred_at_ms) "
      "VALUES(?,?,?,?,?,?,?,?,?)");
  insert.bind(1, request.transition_id);
  insert.bind(2, previous.speech_id);
  insert.bind(3, std::string{speech_state_name(previous.state)});
  insert.bind(4, std::string{speech_state_name(request.target)});
  insert.bind(5, static_cast<std::int64_t>(previous.revision));
  insert.bind(6, static_cast<std::int64_t>(previous.revision + 1));
  insert.bind(7, request.reason);
  insert.bind(8, request.idempotency_key);
  insert.bind(9, request.occurred_at_ms);
  insert.execute();
}

[[nodiscard]] bool terminal(const SpeechState state) noexcept {
  return state == SpeechState::played || state == SpeechState::failed ||
         state == SpeechState::expired || state == SpeechState::cancelled;
}

[[nodiscard]] TtsUsageSummary usage_summary(SqliteConnection &connection,
                                            const std::int64_t now_ms,
                                            const std::int64_t month_start) {
  auto query = connection.prepare(
      "SELECT COALESCE(SUM(CASE WHEN submitted_at_ms>? THEN "
      "estimated_micro_usd ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>=? THEN estimated_micro_usd "
      "ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>? THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>? AND state='succeeded' THEN 1 "
      "ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>? AND state='failed' THEN 1 ELSE "
      "0 END),0),"
      "COALESCE(SUM(CASE WHEN submitted_at_ms>? AND state IN "
      "('unknown','submitted') THEN 1 ELSE 0 END),0) "
      "FROM tts_usage_attempt");
  const auto day_start = std::max<std::int64_t>(0, now_ms - rolling_day_ms);
  query.bind(1, day_start);
  query.bind(2, month_start);
  query.bind(3, day_start);
  query.bind(4, day_start);
  query.bind(5, day_start);
  query.bind(6, day_start);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Unable to summarize TTS usage."};
  return {
      .rolling_day_micro_usd = query.column_int64(0),
      .calendar_month_micro_usd = query.column_int64(1),
      .rolling_day_attempts = static_cast<std::size_t>(query.column_int64(2)),
      .rolling_day_succeeded = static_cast<std::size_t>(query.column_int64(3)),
      .rolling_day_failed = static_cast<std::size_t>(query.column_int64(4)),
      .rolling_day_unknown = static_cast<std::size_t>(query.column_int64(5))};
}

[[nodiscard]] bool add_exceeds(const std::int64_t current,
                               const std::int64_t addition,
                               const std::int64_t limit) noexcept {
  return addition < 0 || current > limit || addition > limit - current;
}

} // namespace

SqliteSpeechRepository::SqliteSpeechRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"Speech repository context is required."};
}

SpeechEnqueueResult
SqliteSpeechRepository::enqueue(const SpeechEnqueueRequest &request) {
  validate_enqueue(request);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  {
    auto replay =
        connection.prepare("SELECT " + std::string{item_columns} +
                           " FROM speech_item WHERE deduplication_key=?");
    replay.bind(1, request.deduplication_key);
    if (replay.step()) {
      auto item = item_from(replay);
      transaction.commit();
      return {.status = SpeechEnqueueStatus::replay,
              .item = std::move(item),
              .evicted_speech_id = std::nullopt};
    }
  }
  if (!active_session(connection, request.voice_session_id,
                      request.source_kind)) {
    transaction.commit();
    return {.status = SpeechEnqueueStatus::invalid_session,
            .item = std::nullopt,
            .evicted_speech_id = std::nullopt};
  }
  const auto terminalize = [&connection, &request](
                               const SpeechItem &item, const SpeechState target,
                               const std::string_view reason) {
    auto update = connection.prepare(
        "UPDATE speech_item SET "
        "state=?,state_version=state_version+1,text=NULL,"
        "terminal_at_ms=?,last_error_code=? WHERE speech_id=? AND "
        "state_version=?");
    update.bind(1, std::string{speech_state_name(target)});
    const auto occurred_at_ms =
        std::max(request.created_at_ms, item.created_at_ms);
    update.bind(2, occurred_at_ms);
    update.bind(3, std::string{reason});
    update.bind(4, item.speech_id);
    update.bind(5, static_cast<std::int64_t>(item.revision));
    update.execute();
    auto audit = connection.prepare(
        "INSERT INTO speech_item_transition(transition_id,speech_id,from_state,"
        "to_state,from_version,to_version,reason,idempotency_key,occurred_at_"
        "ms) "
        "VALUES(lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'"
        "||"
        "substr(lower(hex(randomblob(2))),2)||'-a'||substr(lower(hex("
        "randomblob(2))),2)||'-'||"
        "lower(hex(randomblob(6))),?,?,?,?,?,?,?,?)");
    audit.bind(1, item.speech_id);
    audit.bind(2, std::string{speech_state_name(item.state)});
    audit.bind(3, std::string{speech_state_name(target)});
    audit.bind(4, static_cast<std::int64_t>(item.revision));
    audit.bind(5, static_cast<std::int64_t>(item.revision + 1));
    audit.bind(6, std::string{reason});
    audit.bind(7, "speech:" + std::string{reason} + ":" + item.speech_id + ":" +
                      std::to_string(item.revision));
    audit.bind(8, occurred_at_ms);
    audit.execute();
  };
  {
    auto expired = connection.prepare(
        "SELECT " + std::string{item_columns} +
        " FROM speech_item WHERE voice_session_id=? AND state IN "
        "('pending','ready') AND expires_at_ms IS NOT NULL AND "
        "expires_at_ms<=?");
    expired.bind(1, request.voice_session_id);
    expired.bind(2, request.created_at_ms);
    std::vector<SpeechItem> items;
    while (expired.step())
      items.push_back(item_from(expired));
    for (const auto &item : items)
      terminalize(item, SpeechState::expired, "expired_before_admission");
  }
  auto count = connection.prepare(
      "SELECT COUNT(*) FROM speech_item WHERE voice_session_id=? AND state IN "
      "('pending','synthesizing','ready','playing') AND "
      "(expires_at_ms IS NULL OR expires_at_ms>?) AND "
      "((priority=400)=?)");
  count.bind(1, request.voice_session_id);
  count.bind(2, request.created_at_ms);
  count.bind(3, request.priority == SpeechPriority::critical_control ? 1 : 0);
  static_cast<void>(count.step());
  const auto capacity = request.priority == SpeechPriority::critical_control
                            ? std::int64_t{2}
                            : std::int64_t{16};
  std::optional<std::string> evicted_speech_id;
  if (count.column_int64(0) >= capacity) {
    std::optional<SpeechItem> victim;
    if (request.priority != SpeechPriority::critical_control &&
        request.priority > SpeechPriority::flavor) {
      auto candidate = connection.prepare(
          "SELECT " + std::string{item_columns} +
          " FROM speech_item WHERE voice_session_id=? AND priority=100 AND "
          "interruptible=1 AND state IN ('pending','ready') ORDER BY "
          "created_at_ms,speech_id LIMIT 1");
      candidate.bind(1, request.voice_session_id);
      if (candidate.step())
        victim = item_from(candidate);
    }
    if (!victim) {
      transaction.commit();
      return {.status = SpeechEnqueueStatus::queue_full,
              .item = std::nullopt,
              .evicted_speech_id = std::nullopt};
    }
    terminalize(*victim, SpeechState::cancelled, "queue_overflow_evicted");
    evicted_speech_id = victim->speech_id;
  }
  auto insert = connection.prepare(
      "INSERT INTO speech_item(speech_id,voice_session_id,source_event_id,"
      "source_kind,text,text_hash,scalar_count,provider,model,voice_id,"
      "priority,"
      "narration_rank,state,state_version,earliest_at_ms,expires_at_ms,"
      "interruptible,"
      "deduplication_key,created_at_ms) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,'pending',"
      "1,?,?,?,?,?)");
  insert.bind(1, request.speech_id);
  insert.bind(2, request.voice_session_id);
  bind_optional(insert, 3, request.source_event_id);
  insert.bind(4, request.source_kind);
  insert.bind(5, request.text.text);
  insert.bind(6, request.text_hash);
  insert.bind(7, static_cast<std::int64_t>(request.text.scalar_count));
  insert.bind(8, request.provider);
  insert.bind(9, request.model);
  insert.bind(10, request.voice);
  insert.bind(11, static_cast<std::int64_t>(request.priority));
  insert.bind(12, static_cast<std::int64_t>(request.narration_rank));
  insert.bind(13, request.earliest_at_ms);
  bind_optional(insert, 14, request.expires_at_ms);
  insert.bind(15, request.interruptible ? 1 : 0);
  insert.bind(16, request.deduplication_key);
  insert.bind(17, request.created_at_ms);
  insert.execute();
  auto item = load_item(connection, request.speech_id);
  transaction.commit();
  return {.status = SpeechEnqueueStatus::accepted,
          .item = std::move(item),
          .evicted_speech_id = std::move(evicted_speech_id)};
}

std::optional<SpeechItem> SqliteSpeechRepository::claim_next(
    const std::string_view voice_session_id, const std::int64_t now_ms,
    std::string transition_id, std::string idempotency_key) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  {
    auto expired = connection.prepare(
        "SELECT " + std::string{item_columns} +
        " FROM speech_item WHERE voice_session_id=? AND state='pending' AND "
        "expires_at_ms IS NOT NULL AND expires_at_ms<=?");
    expired.bind(1, voice_session_id);
    expired.bind(2, now_ms);
    std::vector<SpeechItem> items;
    while (expired.step())
      items.push_back(item_from(expired));
    for (const auto &item : items) {
      auto update_expired = connection.prepare(
          "UPDATE speech_item SET "
          "state='expired',state_version=state_version+1,"
          "text=NULL,terminal_at_ms=?,last_error_code='queue_expired' WHERE "
          "speech_id=? AND state_version=?");
      update_expired.bind(1, now_ms);
      update_expired.bind(2, item.speech_id);
      update_expired.bind(3, static_cast<std::int64_t>(item.revision));
      update_expired.execute();
      auto audit = connection.prepare(
          "INSERT INTO "
          "speech_item_transition(transition_id,speech_id,from_state,"
          "to_state,from_version,to_version,reason,idempotency_key,occurred_at_"
          "ms) "
          "VALUES(lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-"
          "4'||"
          "substr(lower(hex(randomblob(2))),2)||'-a'||substr(lower(hex("
          "randomblob(2))),2)||'-'||"
          "lower(hex(randomblob(6))),?,'pending','expired',?,?,"
          "'queue_expired',?,?)");
      audit.bind(1, item.speech_id);
      audit.bind(2, static_cast<std::int64_t>(item.revision));
      audit.bind(3, static_cast<std::int64_t>(item.revision + 1));
      audit.bind(4, "speech:expired:" + item.speech_id + ":" +
                        std::to_string(item.revision));
      audit.bind(5, now_ms);
      audit.execute();
    }
  }
  auto query = connection.prepare(
      "SELECT " + std::string{item_columns} +
      " FROM speech_item WHERE voice_session_id=? AND state='pending' AND "
      "earliest_at_ms<=? AND (expires_at_ms IS NULL OR expires_at_ms>?) "
      "ORDER BY priority DESC,narration_rank DESC,earliest_at_ms,created_at_ms,"
      "speech_id LIMIT 1");
  query.bind(1, voice_session_id);
  query.bind(2, now_ms);
  query.bind(3, now_ms);
  if (!query.step()) {
    transaction.commit();
    return std::nullopt;
  }
  const auto previous = item_from(query);
  SpeechTransitionRequest transition_request{
      .speech_id = previous.speech_id,
      .expected_revision = previous.revision,
      .target = SpeechState::synthesizing,
      .transition_id = std::move(transition_id),
      .reason = "synthesis_claimed",
      .idempotency_key = std::move(idempotency_key),
      .occurred_at_ms = now_ms,
      .provider_request_id = std::nullopt,
      .cache_key = std::nullopt,
      .cache_checksum = std::nullopt,
      .marker = std::nullopt,
      .duration_ms = std::nullopt,
      .error_code = std::nullopt};
  auto update = connection.prepare(
      "UPDATE speech_item SET "
      "state='synthesizing',state_version=state_version+1 "
      "WHERE speech_id=? AND state='pending' AND state_version=?");
  update.bind(1, previous.speech_id);
  update.bind(2, static_cast<std::int64_t>(previous.revision));
  update.execute();
  if (connection.changes() != 1)
    throw DatabaseError{DatabaseErrorCategory::busy, SQLITE_BUSY, SQLITE_BUSY,
                        "Speech claim lost its revision fence."};
  insert_transition(connection, transition_request, previous);
  auto claimed = load_item(connection, previous.speech_id);
  transaction.commit();
  return claimed;
}

SpeechMutationStatus
SqliteSpeechRepository::transition(const SpeechTransitionRequest &request) {
  if (request.speech_id.empty() || request.transition_id.empty() ||
      request.idempotency_key.empty() || request.reason.empty() ||
      request.reason.size() > 64 || request.occurred_at_ms < 0)
    throw std::invalid_argument{"Speech transition request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  {
    auto replay = connection.prepare(
        "SELECT speech_id FROM speech_item_transition WHERE idempotency_key=?");
    replay.bind(1, request.idempotency_key);
    if (replay.step()) {
      const auto unchanged = replay.column_text(0) == request.speech_id;
      transaction.commit();
      return unchanged ? SpeechMutationStatus::unchanged
                       : SpeechMutationStatus::invalid_state;
    }
  }
  const auto previous = load_item(connection, request.speech_id);
  if (!previous) {
    transaction.commit();
    return SpeechMutationStatus::not_found;
  }
  if (previous->revision != request.expected_revision) {
    transaction.commit();
    return SpeechMutationStatus::stale;
  }
  if (!speech_transition_allowed(previous->state, request.target)) {
    transaction.commit();
    return SpeechMutationStatus::invalid_state;
  }
  auto effective_request = request;
  if (terminal(request.target))
    effective_request.occurred_at_ms =
        std::max(request.occurred_at_ms, previous->created_at_ms);
  const auto terminal_at = terminal(request.target)
                               ? std::optional{effective_request.occurred_at_ms}
                               : std::nullopt;
  const auto clear_text =
      terminal(request.target) ||
      (request.target == SpeechState::ready &&
       previous->priority != SpeechPriority::event_narration);
  auto update = connection.prepare(
      "UPDATE speech_item SET state=?,state_version=state_version+1,"
      "text=CASE WHEN ? THEN NULL ELSE text "
      "END,provider_request_id=COALESCE(?,provider_request_id),"
      "cache_key=COALESCE(?,cache_key),cache_checksum=COALESCE(?,cache_"
      "checksum),"
      "marker=COALESCE(?,marker),duration_ms=COALESCE(?,duration_ms),"
      "attempt_count=(SELECT COUNT(*) FROM tts_usage_attempt WHERE "
      "speech_id=?),"
      "terminal_at_ms=?,last_error_code=? WHERE speech_id=? AND "
      "state_version=?");
  update.bind(1, std::string{speech_state_name(request.target)});
  update.bind(2, clear_text ? 1 : 0);
  bind_optional(update, 3, request.provider_request_id);
  bind_optional(update, 4, request.cache_key);
  bind_optional(update, 5, request.cache_checksum);
  bind_optional(update, 6, request.marker);
  bind_optional(update, 7, request.duration_ms);
  update.bind(8, request.speech_id);
  bind_optional(update, 9, terminal_at);
  bind_optional(update, 10, request.error_code);
  update.bind(11, request.speech_id);
  update.bind(12, static_cast<std::int64_t>(previous->revision));
  update.execute();
  insert_transition(connection, effective_request, *previous);
  transaction.commit();
  return SpeechMutationStatus::applied;
}

std::optional<SpeechItem>
SqliteSpeechRepository::find(const std::string_view speech_id) {
  if (speech_id.empty())
    return std::nullopt;
  const std::scoped_lock lock{context_->mutex()};
  return load_item(context_->connection(), speech_id);
}

std::size_t SqliteSpeechRepository::cancel_session(
    const std::string_view voice_session_id, const std::int64_t now_ms,
    const std::string_view reason, const bool include_interactive,
    const bool preserve_event_narration) {
  if (voice_session_id.empty() || now_ms < 0 || reason.empty() ||
      reason.size() > 64)
    throw std::invalid_argument{"Speech cancellation request is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto query = connection.prepare(
      "SELECT " + std::string{item_columns} +
      " FROM speech_item WHERE voice_session_id=? AND state IN "
      "('pending','synthesizing','ready','playing') AND (? OR priority<>300)");
  query.bind(1, voice_session_id);
  query.bind(2, include_interactive ? 1 : 0);
  std::vector<SpeechItem> items;
  while (query.step())
    items.push_back(item_from(query));
  std::size_t changed{};
  for (const auto &item : items) {
    const auto occurred_at_ms = std::max(now_ms, item.created_at_ms);
    if (preserve_event_narration &&
        item.priority == SpeechPriority::event_narration &&
        item.state != SpeechState::playing) {
      if (item.state == SpeechState::pending)
        continue;
      auto defer = connection.prepare(
          "UPDATE speech_item SET state='pending',"
          "state_version=state_version+1 WHERE speech_id=? AND "
          "state_version=?");
      defer.bind(1, item.speech_id);
      defer.bind(2, static_cast<std::int64_t>(item.revision));
      defer.execute();
      if (connection.changes() != 1)
        continue;
      auto audit = connection.prepare(
          "INSERT INTO speech_item_transition(transition_id,speech_id,"
          "from_state,to_state,from_version,to_version,reason,"
          "idempotency_key,occurred_at_ms) VALUES("
          "lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'||"
          "substr(lower(hex(randomblob(2))),2)||'-a'||"
          "substr(lower(hex(randomblob(2))),2)||'-'||"
          "lower(hex(randomblob(6))),?,?,'pending',?,?,"
          "'reconnect_deferred',?,?)");
      audit.bind(1, item.speech_id);
      audit.bind(2, std::string{speech_state_name(item.state)});
      audit.bind(3, static_cast<std::int64_t>(item.revision));
      audit.bind(4, static_cast<std::int64_t>(item.revision + 1));
      audit.bind(5, "speech:reconnect-deferred:" + item.speech_id + ":" +
                        std::to_string(item.revision));
      audit.bind(6, occurred_at_ms);
      audit.execute();
      ++changed;
      continue;
    }
    auto update = connection.prepare(
        "UPDATE speech_item SET "
        "state='cancelled',state_version=state_version+1,"
        "text=NULL,terminal_at_ms=?,last_error_code=? WHERE speech_id=?");
    update.bind(1, occurred_at_ms);
    update.bind(2, std::string{reason});
    update.bind(3, item.speech_id);
    update.execute();
    auto insert = connection.prepare(
        "INSERT INTO speech_item_transition(transition_id,speech_id,from_state,"
        "to_state,from_version,to_version,reason,idempotency_key,occurred_at_"
        "ms) "
        "VALUES(lower(hex(randomblob(4)))||'-'||lower(hex(randomblob(2)))||'-4'"
        "||"
        "substr(lower(hex(randomblob(2))),2)||'-a'||substr(lower(hex("
        "randomblob(2))),2)||'-'||"
        "lower(hex(randomblob(6))),? ,?,'cancelled',?,?,?, ?,?)");
    insert.bind(1, item.speech_id);
    insert.bind(2, std::string{speech_state_name(item.state)});
    insert.bind(3, static_cast<std::int64_t>(item.revision));
    insert.bind(4, static_cast<std::int64_t>(item.revision + 1));
    insert.bind(5, std::string{reason});
    insert.bind(6, "speech:cancel:" + item.speech_id + ":" +
                       std::to_string(item.revision));
    insert.bind(7, occurred_at_ms);
    insert.execute();
    ++changed;
  }
  transaction.commit();
  return changed;
}

std::size_t SqliteSpeechRepository::recover(const std::int64_t now_ms,
                                            const std::string_view reason) {
  std::vector<std::string> sessions;
  {
    const std::scoped_lock lock{context_->mutex()};
    auto query = context_->connection().prepare(
        "SELECT DISTINCT voice_session_id FROM speech_item WHERE state IN "
        "('pending','synthesizing','ready','playing')");
    while (query.step())
      sessions.push_back(query.column_text(0));
  }
  std::size_t recovered{};
  for (const auto &session : sessions)
    recovered += cancel_session(session, now_ms, reason, true);
  return recovered;
}

void SqliteSpeechRepository::ensure_purge_schedule(const std::int64_t now_ms,
                                                   std::string job_id) {
  if (now_ms < 0 || !valid_uuid_v4(job_id))
    throw std::invalid_argument{"TTS purge schedule is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT job_id,job_type,state FROM scheduled_job WHERE "
      "idempotency_key='job:vox:tts-purge-hourly'");
  if (existing.step()) {
    if (existing.column_text(1) != vox_tts_purge_job_type)
      throw DatabaseError{DatabaseErrorCategory::constraint, SQLITE_CONSTRAINT,
                          SQLITE_CONSTRAINT,
                          "TTS purge schedule version conflicts."};
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
      .job_type = std::string{vox_tts_purge_job_type},
      .aggregate_type = "vox_tts_cache",
      .aggregate_id = "primary",
      .due_at_ms = now_ms,
      .max_attempts = 10,
      .idempotency_key = "job:vox:tts-purge-hourly",
      .created_at_ms = now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(connection, job,
                                                   "{\"payload_version\":1}"));
  transaction.commit();
}

std::size_t SqliteSpeechRepository::purge_retained(const std::int64_t now_ms) {
  if (now_ms < 0)
    throw std::invalid_argument{"TTS retention time is invalid."};
  constexpr std::int64_t speech_retention_ms = 30LL * 24 * 60 * 60 * 1'000;
  const auto usage_retention =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::months{13})
          .count();
  const auto speech_before =
      std::max<std::int64_t>(0, now_ms - speech_retention_ms);
  const auto usage_before = std::max<std::int64_t>(0, now_ms - usage_retention);
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto usage = connection.prepare(
      "DELETE FROM tts_usage_attempt WHERE submitted_at_ms<?");
  usage.bind(1, usage_before);
  usage.execute();
  const auto removed_usage = static_cast<std::size_t>(connection.changes());
  auto speech = connection.prepare(
      "DELETE FROM speech_item WHERE terminal_at_ms IS NOT NULL AND "
      "terminal_at_ms<? AND NOT EXISTS(SELECT 1 FROM "
      "voice_narration_intent narration WHERE "
      "narration.speech_id=speech_item.speech_id AND narration.state IN "
      "('pending','generating','prepared','queued'))");
  speech.bind(1, speech_before);
  speech.execute();
  const auto removed_speech = static_cast<std::size_t>(connection.changes());
  transaction.commit();
  return removed_usage + removed_speech;
}

TtsUsageReservationResult SqliteSpeechRepository::reserve_usage(
    const TtsUsageReservationRequest &request) {
  if (request.attempt_id.empty() || request.speech_id.empty() ||
      request.attempt_number < 1 || request.attempt_number > 2 ||
      request.scalar_count < 1 ||
      request.scalar_count > maximum_tts_scalar_count ||
      request.estimated_micro_usd !=
          estimated_tts_cost_micro_usd(request.scalar_count) ||
      request.now_ms < 0 || request.calendar_month_start_ms < 0 ||
      request.calendar_month_start_ms > request.now_ms)
    throw std::invalid_argument{"TTS usage reservation is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  {
    auto replay = connection.prepare(
        "SELECT speech_id FROM tts_usage_attempt WHERE attempt_id=?");
    replay.bind(1, request.attempt_id);
    if (replay.step()) {
      const auto usage = usage_summary(connection, request.now_ms,
                                       request.calendar_month_start_ms);
      const auto same = replay.column_text(0) == request.speech_id;
      transaction.commit();
      return {.accepted = same, .replay = same, .usage = usage};
    }
  }
  auto usage = usage_summary(connection, request.now_ms,
                             request.calendar_month_start_ms);
  const bool allowed =
      usage.rolling_day_attempts < request.policy.rolling_day_attempts &&
      !add_exceeds(usage.rolling_day_micro_usd, request.estimated_micro_usd,
                   request.policy.rolling_day_micro_usd) &&
      !add_exceeds(usage.calendar_month_micro_usd, request.estimated_micro_usd,
                   request.policy.calendar_month_micro_usd);
  if (!allowed) {
    transaction.commit();
    return {.accepted = false, .replay = false, .usage = usage};
  }
  auto insert = connection.prepare(
      "INSERT INTO tts_usage_attempt(attempt_id,speech_id,attempt_number,"
      "provider,model,voice_id,scalar_count,estimated_micro_usd,state,"
      "submitted_at_ms) VALUES(?,?,?,?,?,?,?,?,'submitted',?)");
  insert.bind(1, request.attempt_id);
  insert.bind(2, request.speech_id);
  insert.bind(3, static_cast<std::int64_t>(request.attempt_number));
  insert.bind(4, request.provider);
  insert.bind(5, request.model);
  insert.bind(6, request.voice);
  insert.bind(7, static_cast<std::int64_t>(request.scalar_count));
  insert.bind(8, request.estimated_micro_usd);
  insert.bind(9, request.now_ms);
  insert.execute();
  usage.rolling_day_micro_usd += request.estimated_micro_usd;
  usage.calendar_month_micro_usd += request.estimated_micro_usd;
  ++usage.rolling_day_attempts;
  transaction.commit();
  return {.accepted = true, .replay = false, .usage = usage};
}

SpeechMutationStatus
SqliteSpeechRepository::complete_usage(const TtsUsageCompletion &completion) {
  if (completion.attempt_id.empty() ||
      (completion.state != "succeeded" && completion.state != "failed" &&
       completion.state != "unknown") ||
      completion.completed_at_ms < 0)
    throw std::invalid_argument{"TTS usage completion is invalid."};
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT state,submitted_at_ms FROM tts_usage_attempt WHERE attempt_id=?");
  existing.bind(1, completion.attempt_id);
  if (!existing.step()) {
    transaction.commit();
    return SpeechMutationStatus::not_found;
  }
  const auto state = existing.column_text(0);
  if (state != "submitted") {
    transaction.commit();
    return state == completion.state ? SpeechMutationStatus::unchanged
                                     : SpeechMutationStatus::invalid_state;
  }
  const auto completed_at_ms =
      std::max(completion.completed_at_ms, existing.column_int64(1));
  auto update = connection.prepare(
      "UPDATE tts_usage_attempt SET state=?,provider_request_id=?,latency_ms=?,"
      "duration_ms=?,error_code=?,completed_at_ms=? WHERE attempt_id=? AND "
      "state='submitted'");
  update.bind(1, completion.state);
  bind_optional(update, 2, completion.provider_request_id);
  bind_optional(update, 3, completion.latency_ms);
  bind_optional(update, 4, completion.duration_ms);
  bind_optional(update, 5, completion.error_code);
  update.bind(6, completed_at_ms);
  update.bind(7, completion.attempt_id);
  update.execute();
  if (connection.changes() == 1) {
    transaction.commit();
    return SpeechMutationStatus::applied;
  }
  transaction.commit();
  return SpeechMutationStatus::stale;
}

std::optional<TtsCacheMetadata>
SqliteSpeechRepository::cache_metadata(const std::string_view cache_key,
                                       const std::int64_t accessed_at_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto query = connection.prepare(
      "SELECT "
      "cache_key,checksum,byte_count,frame_count,provider,model,voice_id,"
      "created_at_ms,last_access_at_ms FROM tts_cache_entry WHERE cache_key=?");
  query.bind(1, cache_key);
  if (!query.step())
    return std::nullopt;
  TtsCacheMetadata result{
      .cache_key = query.column_text(0),
      .checksum = query.column_text(1),
      .byte_count = static_cast<std::uintmax_t>(query.column_int64(2)),
      .frame_count = static_cast<std::uintmax_t>(query.column_int64(3)),
      .provider = query.column_text(4),
      .model = query.column_text(5),
      .voice = query.column_text(6),
      .created_at_ms = query.column_int64(7),
      .last_access_at_ms = query.column_int64(8)};
  auto touch = connection.prepare(
      "UPDATE tts_cache_entry SET last_access_at_ms=max(last_access_at_ms,?) "
      "WHERE cache_key=?");
  touch.bind(1, accessed_at_ms);
  touch.bind(2, cache_key);
  touch.execute();
  result.last_access_at_ms = std::max(result.last_access_at_ms, accessed_at_ms);
  return result;
}

void SqliteSpeechRepository::put_cache_metadata(
    const TtsCacheMetadata &metadata) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto statement = connection.prepare(
      "INSERT INTO tts_cache_entry(cache_key,file_name,checksum,byte_count,"
      "frame_count,provider,model,voice_id,created_at_ms,last_access_at_ms) "
      "VALUES(?,?,?, ?,?,?,?,?,?,?) ON CONFLICT(cache_key) DO UPDATE SET "
      "checksum=excluded.checksum,byte_count=excluded.byte_count,"
      "frame_count=excluded.frame_count,last_access_at_ms=max("
      "tts_cache_entry.last_access_at_ms,excluded.last_access_at_ms)");
  statement.bind(1, metadata.cache_key);
  statement.bind(2, metadata.cache_key + ".pcm");
  statement.bind(3, metadata.checksum);
  statement.bind(4, static_cast<std::int64_t>(metadata.byte_count));
  statement.bind(5, static_cast<std::int64_t>(metadata.frame_count));
  statement.bind(6, metadata.provider);
  statement.bind(7, metadata.model);
  statement.bind(8, metadata.voice);
  statement.bind(9, metadata.created_at_ms);
  statement.bind(10, metadata.last_access_at_ms);
  statement.execute();
}

void SqliteSpeechRepository::remove_cache_metadata(
    const std::string_view cache_key) {
  const std::scoped_lock lock{context_->mutex()};
  auto statement = context_->connection().prepare(
      "DELETE FROM tts_cache_entry WHERE cache_key=?");
  statement.bind(1, cache_key);
  statement.execute();
}

std::vector<std::string> SqliteSpeechRepository::cache_keys() {
  const std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT cache_key FROM tts_cache_entry ORDER BY cache_key");
  std::vector<std::string> result;
  while (query.step())
    result.push_back(query.column_text(0));
  return result;
}

std::string
SqliteSpeechRepository::selected_voice(const std::string_view guild_id) {
  const std::scoped_lock lock{context_->mutex()};
  auto ensure = context_->connection().prepare(
      "INSERT OR IGNORE INTO "
      "vox_voice_configuration(guild_id,voice_id,revision,"
      "updated_at_ms) SELECT guild_id,'onyx',1,created_at_ms FROM guild_config "
      "WHERE guild_id=?");
  ensure.bind(1, guild_id);
  ensure.execute();
  auto query = context_->connection().prepare(
      "SELECT voice_id FROM vox_voice_configuration WHERE guild_id=?");
  query.bind(1, guild_id);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Vox voice configuration is missing."};
  return query.column_text(0);
}

SpeechMutationStatus SqliteSpeechRepository::select_voice(
    const std::string_view guild_id, const std::string_view voice,
    const std::string_view actor_user_id, const std::int64_t now_ms) {
  if (voice != "onyx" || guild_id.empty() || actor_user_id.empty() ||
      now_ms < 0)
    return SpeechMutationStatus::invalid_state;
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE vox_voice_configuration SET voice_id=?,revision=revision+1,"
      "updated_by_user_id=?,updated_at_ms=max(updated_at_ms,?) WHERE "
      "guild_id=? "
      "AND voice_id<>?");
  update.bind(1, voice);
  update.bind(2, actor_user_id);
  update.bind(3, now_ms);
  update.bind(4, guild_id);
  update.bind(5, voice);
  update.execute();
  if (connection.changes() == 1)
    return SpeechMutationStatus::applied;
  auto query = connection.prepare(
      "SELECT voice_id FROM vox_voice_configuration WHERE guild_id=?");
  query.bind(1, guild_id);
  if (!query.step())
    return SpeechMutationStatus::not_found;
  return query.column_text(0) == voice ? SpeechMutationStatus::unchanged
                                       : SpeechMutationStatus::invalid_state;
}

SpeechRepositoryHealth
SqliteSpeechRepository::health(const std::int64_t now_ms,
                               const std::int64_t calendar_month_start_ms) {
  const std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto queue = connection.prepare(
      "SELECT COALESCE(SUM(state='pending'),0),"
      "COALESCE(SUM(state='synthesizing'),0),COALESCE(SUM(state='ready'),0),"
      "COALESCE(SUM(state='playing'),0) FROM speech_item WHERE state IN "
      "('pending','synthesizing','ready','playing')");
  static_cast<void>(queue.step());
  auto cache = connection.prepare(
      "SELECT COUNT(*),COALESCE(SUM(byte_count),0) FROM tts_cache_entry");
  static_cast<void>(cache.step());
  return {.queued = static_cast<std::size_t>(queue.column_int64(0)),
          .synthesizing = static_cast<std::size_t>(queue.column_int64(1)),
          .ready = static_cast<std::size_t>(queue.column_int64(2)),
          .playing = static_cast<std::size_t>(queue.column_int64(3)),
          .usage = usage_summary(connection, now_ms, calendar_month_start_ms),
          .cache_entries = static_cast<std::size_t>(cache.column_int64(0)),
          .cache_bytes = static_cast<std::uintmax_t>(cache.column_int64(1))};
}

} // namespace sanguinius::persistence
