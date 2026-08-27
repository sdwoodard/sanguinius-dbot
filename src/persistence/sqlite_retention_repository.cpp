#include "sanguinius/persistence/sqlite_retention_repository.hpp"

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include "notice_retention.hpp"
#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sanguinius::persistence {
namespace {

constexpr std::int64_t day_ms = 24 * 60 * 60 * 1'000;
constexpr std::int64_t thirty_days_ms = 30 * day_ms;
constexpr std::size_t batch_size = 500;

struct PayloadRow {
  std::string id;
  std::string payload_json;
};

[[nodiscard]] std::size_t changed(SqliteConnection &connection) {
  return static_cast<std::size_t>(connection.changes());
}

[[nodiscard]] std::int64_t usage_retention_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::months{13})
      .count();
}

[[nodiscard]] std::vector<PayloadRow> payload_rows(SqliteStatement &statement) {
  std::vector<PayloadRow> result;
  result.reserve(batch_size);
  while (statement.step())
    result.push_back({statement.column_text(0), statement.column_text(1)});
  return result;
}

[[nodiscard]] std::size_t tombstone_payload_rows(
    SqliteConnection &connection, const std::vector<PayloadRow> &rows,
    const std::string_view table, const std::string_view id_column) {
  auto update = connection.prepare("UPDATE " + std::string{table} +
                                   " SET payload_json=? WHERE " +
                                   std::string{id_column} + "=?");
  std::size_t count{};
  for (const auto &row : rows) {
    const auto payload = nlohmann::json::parse(row.payload_json);
    const auto tombstone = detail::retained_notice_tombstone(payload).dump();
    update.bind(1, tombstone);
    update.bind(2, row.id);
    update.execute();
    count += changed(connection);
    update.reset();
  }
  return count;
}

[[nodiscard]] std::string counts_json(const RetentionCounts &counts) {
  return nlohmann::json{{"appearance_contexts", counts.appearance_contexts},
                        {"appearance_previews", counts.appearance_previews},
                        {"notice_payloads", counts.notice_payloads},
                        {"interaction_snapshots", counts.interaction_snapshots},
                        {"speech_items", counts.speech_items},
                        {"provider_usage", counts.provider_usage},
                        {"diagnostics", counts.diagnostics},
                        {"tts_cache_removals", counts.tts_cache_removals},
                        {"tts_cache_failures", counts.tts_cache_failures}}
      .dump();
}

} // namespace

SqliteRetentionRepository::SqliteRetentionRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite repository context is required."};
}

void SqliteRetentionRepository::ensure_schedule(const std::int64_t now_ms,
                                                std::string job_id) {
  if (now_ms < 0 || !valid_uuid_v4(job_id))
    throw std::invalid_argument{"Retention schedule is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto existing = connection.prepare(
      "SELECT job_id,job_type,state FROM scheduled_job WHERE "
      "idempotency_key='job:maintenance:retention-daily'");
  const auto due = RetentionService::next_due_utc(now_ms);
  if (existing.step()) {
    if (existing.column_text(1) != retention_job_type)
      throw std::runtime_error{"Retention schedule version conflicts."};
    const auto state = existing.column_text(2);
    if (state == "dead" || state == "cancelled" || state == "completed") {
      auto rearm = connection.prepare(
          "UPDATE scheduled_job SET "
          "state='pending',attempt_count=0,due_at_ms=?,"
          "lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,"
          "last_error_code=NULL,completed_at_ms=NULL,terminal_at_ms=NULL,"
          "updated_at_ms=max(?,updated_at_ms) WHERE job_id=?");
      rearm.bind(1, due);
      rearm.bind(2, now_ms);
      rearm.bind(3, existing.column_text(0));
      rearm.execute();
    }
    transaction.commit();
    return;
  }
  const ScheduledJobEnqueue job{
      .job_id = std::move(job_id),
      .job_type = std::string{retention_job_type},
      .aggregate_type = "maintenance",
      .aggregate_id = "retention",
      .due_at_ms = due,
      .max_attempts = 10,
      .idempotency_key = "job:maintenance:retention-daily",
      .created_at_ms = now_ms,
  };
  static_cast<void>(detail::insert_job_uncommitted(connection, job,
                                                   "{\"payload_version\":1}"));
  transaction.commit();
}

RetentionCounts SqliteRetentionRepository::run(const std::int64_t now_ms,
                                               std::string run_id,
                                               RetentionCounts initial) {
  if (now_ms < 0 || !valid_uuid_v4(run_id))
    throw std::invalid_argument{"Retention run is invalid."};
  const auto notices_before =
      std::max<std::int64_t>(0, now_ms - thirty_days_ms);
  const auto snapshots_before = std::max<std::int64_t>(0, now_ms - day_ms);
  const auto usage_before =
      std::max<std::int64_t>(0, now_ms - usage_retention_ms());
  RetentionCounts counts = initial;
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  {
    Transaction transaction{connection, TransactionMode::immediate};
    auto interrupted = connection.prepare(
        "UPDATE retention_run SET state='failed',completed_at_ms="
        "max(started_at_ms,?),error_code='interrupted' WHERE state='running'");
    interrupted.bind(1, now_ms);
    interrupted.execute();
    auto begin = connection.prepare(
        "INSERT INTO retention_run(run_id,state,counts_json,started_at_ms) "
        "VALUES(?,'running',?,?)");
    begin.bind(1, run_id);
    begin.bind(2, counts_json(initial));
    begin.bind(3, now_ms);
    begin.execute();
    transaction.commit();
  }

  const auto cleanup = [&] {
    Transaction transaction{connection, TransactionMode::immediate};

    auto contexts = connection.prepare(
        "UPDATE appearance_candidate SET context_json=json_set(context_json,"
        "'$.excerpts',json('[]')) WHERE candidate_id IN (SELECT candidate_id "
        "FROM appearance_candidate WHERE context_expires_at_ms<=? AND "
        "json_array_length(context_json,'$.excerpts')>0 LIMIT ?)");
    contexts.bind(1, now_ms);
    contexts.bind(2, static_cast<std::int64_t>(batch_size));
    contexts.execute();
    counts.appearance_contexts = changed(connection);

    auto previews = connection.prepare(
        "DELETE FROM appearance_preview WHERE decision_id IN (SELECT "
        "decision_id "
        "FROM appearance_preview WHERE expires_at_ms<=? LIMIT ?)");
    previews.bind(1, now_ms);
    previews.bind(2, static_cast<std::int64_t>(batch_size));
    previews.execute();
    counts.appearance_previews = changed(connection);

    auto notices = connection.prepare(
        "SELECT notice_id,payload_json FROM pending_notice WHERE state IN "
        "('consumed','expired','cancelled') AND created_at_ms<? AND "
        "COALESCE(json_extract(payload_json,'$.retention_tombstone_version'),"
        "0)<>1 LIMIT ?");
    notices.bind(1, notices_before);
    notices.bind(2, static_cast<std::int64_t>(batch_size));
    counts.notice_payloads = tombstone_payload_rows(
        connection, payload_rows(notices), "pending_notice", "notice_id");

    auto notice_jobs = connection.prepare(
        "SELECT job_id,payload_json FROM scheduled_job WHERE job_type=? AND "
        "state IN ('completed','cancelled','dead') AND terminal_at_ms<? AND "
        "COALESCE(json_extract(payload_json,'$.retention_tombstone_version'),"
        "0)<>1 LIMIT ?");
    notice_jobs.bind(1, owner_test_notice_job_type);
    notice_jobs.bind(2, notices_before);
    notice_jobs.bind(3, static_cast<std::int64_t>(batch_size));
    counts.notice_payloads += tombstone_payload_rows(
        connection, payload_rows(notice_jobs), "scheduled_job", "job_id");

    auto notice_outbox = connection.prepare(
        "SELECT outbox_id,payload_json FROM outbox_message WHERE kind=? AND "
        "(state IN ('delivered','dead','cancelled') OR (state='failed' AND "
        "last_error_code IS NOT NULL AND instr(last_error_code,'unknown')=0)) "
        "AND terminal_at_ms<? AND COALESCE(json_extract(payload_json,"
        "'$.retention_tombstone_version'),0)<>1 LIMIT ?");
    notice_outbox.bind(1, pending_notice_outbox_kind);
    notice_outbox.bind(2, notices_before);
    notice_outbox.bind(3, static_cast<std::int64_t>(batch_size));
    counts.notice_payloads += tombstone_payload_rows(
        connection, payload_rows(notice_outbox), "outbox_message", "outbox_id");

    auto chronicle_cursors = connection.prepare(
        "DELETE FROM chronicle_search_cursor WHERE cursor_id IN (SELECT "
        "cursor_id FROM chronicle_search_cursor WHERE expires_at_ms<? LIMIT "
        "?)");
    chronicle_cursors.bind(1, snapshots_before);
    chronicle_cursors.bind(2, static_cast<std::int64_t>(batch_size));
    chronicle_cursors.execute();
    counts.interaction_snapshots = changed(connection);
    auto tarot_cursors = connection.prepare(
        "DELETE FROM tarot_history_cursor WHERE cursor_id IN (SELECT "
        "cursor_id FROM tarot_history_cursor WHERE expires_at_ms<? LIMIT ?)");
    tarot_cursors.bind(1, snapshots_before);
    tarot_cursors.bind(2, static_cast<std::int64_t>(batch_size));
    tarot_cursors.execute();
    counts.interaction_snapshots += changed(connection);
    auto wager_cursors = connection.prepare(
        "DELETE FROM tarot_wager_history_cursor WHERE cursor_id IN (SELECT "
        "cursor_id FROM tarot_wager_history_cursor WHERE expires_at_ms<? LIMIT "
        "?)");
    wager_cursors.bind(1, snapshots_before);
    wager_cursors.bind(2, static_cast<std::int64_t>(batch_size));
    wager_cursors.execute();
    counts.interaction_snapshots += changed(connection);
    auto list_snapshots = connection.prepare(
        "DELETE FROM interaction_list_snapshot WHERE snapshot_id IN (SELECT "
        "snapshot_id FROM interaction_list_snapshot WHERE expires_at_ms<? "
        "LIMIT "
        "?)");
    list_snapshots.bind(1, snapshots_before);
    list_snapshots.bind(2, static_cast<std::int64_t>(batch_size));
    list_snapshots.execute();
    counts.interaction_snapshots += changed(connection);

    auto voice_usage = connection.prepare(
        "DELETE FROM voice_transcription_usage WHERE window_id IN (SELECT "
        "window_id FROM voice_transcription_usage WHERE recorded_at_ms<? LIMIT "
        "?)");
    voice_usage.bind(1, usage_before);
    voice_usage.bind(2, static_cast<std::int64_t>(batch_size));
    voice_usage.execute();
    counts.provider_usage = changed(connection);
    auto ai_usage = connection.prepare(
        "DELETE FROM ai_generation_attempt WHERE attempt_id IN (SELECT "
        "attempt_id FROM ai_generation_attempt WHERE completed_at_ms IS NOT "
        "NULL "
        "AND completed_at_ms<? LIMIT ?)");
    ai_usage.bind(1, usage_before);
    ai_usage.bind(2, static_cast<std::int64_t>(batch_size));
    ai_usage.execute();
    counts.provider_usage += changed(connection);
    auto tts_usage = connection.prepare(
        "DELETE FROM tts_usage_attempt WHERE attempt_id IN (SELECT attempt_id "
        "FROM tts_usage_attempt WHERE state<>'submitted' AND submitted_at_ms<? "
        "LIMIT ?)");
    tts_usage.bind(1, usage_before);
    tts_usage.bind(2, static_cast<std::int64_t>(batch_size));
    tts_usage.execute();
    counts.provider_usage += changed(connection);

    auto speech = connection.prepare(
        "DELETE FROM speech_item WHERE speech_id IN (SELECT item.speech_id "
        "FROM "
        "speech_item item WHERE item.terminal_at_ms IS NOT NULL AND "
        "item.terminal_at_ms<? AND NOT EXISTS(SELECT 1 FROM "
        "voice_narration_intent narration WHERE narration.speech_id="
        "item.speech_id AND narration.state IN "
        "('pending','generating','prepared','queued')) LIMIT ?)");
    speech.bind(1, notices_before);
    speech.bind(2, static_cast<std::int64_t>(batch_size));
    speech.execute();
    counts.speech_items = changed(connection);

    auto jobs = connection.prepare(
        "UPDATE scheduled_job SET last_error_code=NULL WHERE job_id IN (SELECT "
        "job_id FROM scheduled_job WHERE state IN "
        "('completed','cancelled','dead') "
        "AND terminal_at_ms<? AND last_error_code IS NOT NULL LIMIT ?)");
    jobs.bind(1, notices_before);
    jobs.bind(2, static_cast<std::int64_t>(batch_size));
    jobs.execute();
    counts.diagnostics = changed(connection);
    auto outbox = connection.prepare(
        "UPDATE outbox_message SET last_error_code=NULL WHERE outbox_id IN "
        "(SELECT outbox_id FROM outbox_message WHERE (state IN ('delivered',"
        "'dead','cancelled') OR (state='failed' AND "
        "instr(last_error_code,'unknown')=0)) AND terminal_at_ms<? AND "
        "last_error_code IS NOT NULL LIMIT ?)");
    outbox.bind(1, notices_before);
    outbox.bind(2, static_cast<std::int64_t>(batch_size));
    outbox.execute();
    counts.diagnostics += changed(connection);

    transaction.commit();
  };

  const auto record_failed = [&](const RetentionCounts &failed_counts) {
    Transaction transaction{connection, TransactionMode::immediate};
    auto failed = connection.prepare(
        "UPDATE retention_run SET state='failed',counts_json=?,"
        "completed_at_ms=max(started_at_ms,?),"
        "error_code='database_retention_failed' WHERE run_id=? AND "
        "state='running'");
    failed.bind(1, counts_json(failed_counts));
    failed.bind(2, now_ms);
    failed.bind(3, run_id);
    failed.execute();
    transaction.commit();
  };

  try {
    cleanup();
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      record_failed(initial);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }

  try {
    Transaction transaction{connection, TransactionMode::immediate};
    auto complete = connection.prepare(
        "UPDATE retention_run SET state='completed',counts_json=?,"
        "completed_at_ms=? WHERE run_id=? AND state='running'");
    complete.bind(1, counts_json(counts));
    complete.bind(2, now_ms);
    complete.bind(3, run_id);
    complete.execute();
    transaction.commit();
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      record_failed(counts);
    } catch (...) {
    }
    std::rethrow_exception(failure);
  }
  return counts;
}

} // namespace sanguinius::persistence
