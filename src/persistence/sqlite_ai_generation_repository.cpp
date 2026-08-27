#include "sanguinius/persistence/sqlite_ai_generation_repository.hpp"

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr std::int64_t rolling_day_ms = 24 * 60 * 60 * 1'000;
constexpr std::int64_t direct_window_ms = 10 * 60 * 1'000;

void require_timestamp(const std::int64_t value) {
  if (value < 0)
    throw std::invalid_argument{"AI generation timestamp is invalid."};
}

void require_identifier(const std::string_view value,
                        const std::size_t maximum) {
  if (value.empty() || value.size() > maximum)
    throw std::invalid_argument{"AI generation identifier is invalid."};
}

[[nodiscard]] std::int64_t month_start(const std::int64_t now_ms) {
  require_timestamp(now_ms);
  using namespace std::chrono;
  const auto now = sys_time<milliseconds>{milliseconds{now_ms}};
  const year_month_day date{floor<days>(now)};
  return duration_cast<milliseconds>(
             sys_days{date.year() / date.month() / day{1}}.time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t charged_usage(SqliteConnection &connection,
                                         const std::string_view guild_id,
                                         const std::int64_t since_ms) {
  auto query = connection.prepare(
      "SELECT COALESCE(SUM(CASE WHEN state='succeeded' THEN actual_micro_usd "
      "ELSE reserved_micro_usd END),0) FROM ai_generation_attempt "
      "WHERE guild_id=? AND created_at_ms>=? AND state<>'cancelled'");
  query.bind(1, guild_id);
  query.bind(2, since_ms);
  if (!query.step())
    throw std::runtime_error{"AI generation usage query returned no row."};
  return query.column_int64(0);
}

[[nodiscard]] std::int64_t accepted_count(SqliteConnection &connection,
                                          const std::string_view guild_id,
                                          const std::int64_t since_ms) {
  auto query = connection.prepare(
      "SELECT COUNT(*) FROM ai_generation_attempt WHERE guild_id=? AND "
      "created_at_ms>=? AND state<>'cancelled'");
  query.bind(1, guild_id);
  query.bind(2, since_ms);
  if (!query.step())
    throw std::runtime_error{"AI generation count query returned no row."};
  return query.column_int64(0);
}

[[nodiscard]] bool would_exceed(const std::int64_t used,
                                const std::int64_t reserved,
                                const std::int64_t limit) noexcept {
  return used < 0 || reserved < 0 || limit < 0 || used > limit ||
         reserved > limit - used;
}

void require_changed(SqliteConnection &connection) {
  if (connection.changes() != 1)
    throw std::runtime_error{"AI generation state transition was stale."};
}

struct CircuitRow {
  std::string state;
  std::int64_t failures{};
  std::optional<std::int64_t> first_failure;
  std::optional<std::int64_t> retry_after;
  bool indefinite{};
  std::int64_t revision{};
};

[[nodiscard]] CircuitRow circuit(SqliteConnection &connection) {
  auto query = connection.prepare(
      "SELECT state,consecutive_failures,first_failure_at_ms,retry_after_ms,"
      "indefinite,revision FROM provider_circuit_state WHERE "
      "provider='openai_text'");
  if (!query.step())
    throw std::runtime_error{"Text provider circuit is missing."};
  return {.state = query.column_text(0),
          .failures = query.column_int64(1),
          .first_failure = query.column_is_null(2)
                               ? std::nullopt
                               : std::optional{query.column_int64(2)},
          .retry_after = query.column_is_null(3)
                             ? std::nullopt
                             : std::optional{query.column_int64(3)},
          .indefinite = query.column_int64(4) != 0,
          .revision = query.column_int64(5)};
}

void append_circuit_transition(SqliteConnection &connection,
                               const CircuitRow &before,
                               const std::string_view target,
                               const std::string_view reason,
                               const std::string_view transition_id,
                               const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO provider_circuit_transition(transition_id,provider,"
      "from_state,to_state,reason_code,from_revision,to_revision,"
      "occurred_at_ms,idempotency_key) VALUES(?,'openai_text',?,?,?,?,?,?,?)");
  insert.bind(1, transition_id);
  insert.bind(2, before.state);
  insert.bind(3, target);
  insert.bind(4, reason);
  insert.bind(5, before.revision);
  insert.bind(6, before.revision + 1);
  insert.bind(7, now_ms);
  insert.bind(8, "circuit:openai-text:" + std::string{transition_id});
  insert.execute();
}

[[nodiscard]] bool retryable(const AiProviderErrorCategory category) noexcept {
  return category == AiProviderErrorCategory::timeout ||
         category == AiProviderErrorCategory::rate_limited ||
         category == AiProviderErrorCategory::server ||
         category == AiProviderErrorCategory::invalid_response ||
         category == AiProviderErrorCategory::transport;
}

[[nodiscard]] const char *
circuit_failure_code(const AiProviderErrorCategory category) noexcept {
  switch (category) {
  case AiProviderErrorCategory::timeout:
    return "timeout";
  case AiProviderErrorCategory::rate_limited:
    return "rate_limited";
  case AiProviderErrorCategory::authentication:
    return "authentication";
  case AiProviderErrorCategory::server:
    return "server";
  case AiProviderErrorCategory::invalid_request:
    return "invalid_request";
  case AiProviderErrorCategory::invalid_response:
    return "invalid_response";
  case AiProviderErrorCategory::transport:
    return "transport";
  }
  return "provider_failure";
}

} // namespace

SqliteAiGenerationRepository::SqliteAiGenerationRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite repository context is required."};
}

AiGenerationAdmissionResult SqliteAiGenerationRepository::reserve(
    const DiscordSnowflake &guild_id,
    const AiGenerationReservation &reservation,
    const AiGenerationPolicy &policy) {
  if (!guild_id.is_set() || (!reservation.requester_user_id.has_value() &&
                             reservation.priority == AiPriority::direct))
    throw std::invalid_argument{"Direct AI generation requester is required."};
  require_identifier(reservation.attempt_id, 36);
  require_identifier(reservation.idempotency_key, 160);
  require_identifier(reservation.model, 128);
  require_timestamp(reservation.now_ms);
  if (reservation.reserved_input_tokens == 0 ||
      reservation.reserved_input_tokens > policy.maximum_input_bytes ||
      reservation.reserved_output_tokens == 0 ||
      reservation.reserved_output_tokens > policy.maximum_output_tokens ||
      reservation.reserved_micro_usd <= 0)
    throw std::invalid_argument{"AI generation reservation is invalid."};

  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  auto duplicate = connection.prepare(
      "SELECT attempt_id FROM ai_generation_attempt WHERE idempotency_key=?");
  duplicate.bind(1, reservation.idempotency_key);
  if (duplicate.step()) {
    const auto result =
        AiGenerationAdmissionResult{.status = AiGenerationAdmission::duplicate,
                                    .attempt_id = duplicate.column_text(0)};
    transaction.commit();
    return result;
  }

  auto control = connection.prepare(
      "SELECT disabled FROM runtime_feature_control WHERE feature='text-ai'");
  if (!control.step() || control.column_int64(0) != 0) {
    transaction.commit();
    return {.status = AiGenerationAdmission::disabled, .attempt_id = {}};
  }

  const auto day_start = reservation.now_ms > rolling_day_ms
                             ? reservation.now_ms - rolling_day_ms
                             : 0;
  const auto count = accepted_count(connection, guild_id.str(), day_start);
  if (count >= static_cast<std::int64_t>(policy.rolling_day_generations)) {
    transaction.commit();
    return {.status = AiGenerationAdmission::daily_count, .attempt_id = {}};
  }

  if (reservation.priority == AiPriority::direct) {
    const auto direct_start = reservation.now_ms > direct_window_ms
                                  ? reservation.now_ms - direct_window_ms
                                  : 0;
    auto direct = connection.prepare(
        "SELECT COUNT(*) FROM ai_generation_attempt WHERE guild_id=? AND "
        "requester_user_id=? AND priority='direct' AND created_at_ms>=? AND "
        "state<>'cancelled'");
    direct.bind(1, guild_id.str());
    direct.bind(2, *reservation.requester_user_id);
    direct.bind(3, direct_start);
    if (!direct.step())
      throw std::runtime_error{"Direct AI rate query returned no row."};
    if (direct.column_int64(0) >=
        static_cast<std::int64_t>(policy.direct_user_ten_minute_generations)) {
      transaction.commit();
      return {.status = AiGenerationAdmission::direct_rate, .attempt_id = {}};
    }
  }

  if (would_exceed(charged_usage(connection, guild_id.str(), day_start),
                   reservation.reserved_micro_usd,
                   policy.rolling_day_micro_usd)) {
    transaction.commit();
    return {.status = AiGenerationAdmission::daily_cost, .attempt_id = {}};
  }
  if (would_exceed(charged_usage(connection, guild_id.str(),
                                 month_start(reservation.now_ms)),
                   reservation.reserved_micro_usd,
                   policy.calendar_month_micro_usd)) {
    transaction.commit();
    return {.status = AiGenerationAdmission::monthly_cost, .attempt_id = {}};
  }

  auto insert = connection.prepare(
      "INSERT INTO ai_generation_attempt(attempt_id,guild_id,"
      "requester_user_id,purpose,priority,model,"
      "input_rate_micro_usd_per_million,"
      "output_rate_micro_usd_per_million,reserved_input_tokens,"
      "reserved_output_tokens,reserved_micro_usd,provider_sent,state,"
      "idempotency_key,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,0,"
      "'reserved',?,?)");
  insert.bind(1, reservation.attempt_id);
  insert.bind(2, guild_id.str());
  if (reservation.requester_user_id)
    insert.bind(3, *reservation.requester_user_id);
  else
    insert.bind_null(3);
  insert.bind(4, ai_purpose_name(reservation.purpose));
  insert.bind(5, ai_priority_name(reservation.priority));
  insert.bind(6, reservation.model);
  insert.bind(7, reservation.input_rate);
  insert.bind(8, reservation.output_rate);
  insert.bind(9, static_cast<std::int64_t>(reservation.reserved_input_tokens));
  insert.bind(10,
              static_cast<std::int64_t>(reservation.reserved_output_tokens));
  insert.bind(11, reservation.reserved_micro_usd);
  insert.bind(12, reservation.idempotency_key);
  insert.bind(13, reservation.now_ms);
  insert.execute();
  transaction.commit();
  return {.status = AiGenerationAdmission::accepted,
          .attempt_id = reservation.attempt_id};
}

void SqliteAiGenerationRepository::mark_submitted(
    const std::string_view attempt_id, const std::int64_t now_ms) {
  require_timestamp(now_ms);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE ai_generation_attempt SET provider_sent=1,state='submitted',"
      "submitted_at_ms=? WHERE attempt_id=? AND state='reserved'");
  update.bind(1, now_ms);
  update.bind(2, attempt_id);
  update.execute();
  require_changed(connection);
}

void SqliteAiGenerationRepository::complete(const std::string_view attempt_id,
                                            const AiGenerationUsage &usage) {
  require_timestamp(usage.completed_at_ms);
  const auto request_id =
      sanitize_ai_provider_request_id(usage.provider_request_id);
  if (!usage.provider_request_id.empty() && request_id.empty())
    throw std::invalid_argument{"Provider request ID is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE ai_generation_attempt SET state='succeeded',"
      "actual_input_tokens=?,actual_output_tokens=?,actual_micro_usd=?,"
      "provider_request_id=?,result_code='success',completed_at_ms=? "
      "WHERE attempt_id=? AND state='submitted'");
  update.bind(1, static_cast<std::int64_t>(usage.input_tokens));
  update.bind(2, static_cast<std::int64_t>(usage.output_tokens));
  update.bind(3, usage.micro_usd);
  if (request_id.empty())
    update.bind_null(4);
  else
    update.bind(4, request_id);
  update.bind(5, usage.completed_at_ms);
  update.bind(6, attempt_id);
  update.execute();
  require_changed(connection);
}

void SqliteAiGenerationRepository::fail(
    const std::string_view attempt_id, const std::string_view result_code,
    const std::string_view provider_request_id, const std::int64_t now_ms) {
  require_timestamp(now_ms);
  require_identifier(result_code, 96);
  const auto request_id = sanitize_ai_provider_request_id(provider_request_id);
  if (!provider_request_id.empty() && request_id.empty())
    throw std::invalid_argument{"Provider request ID is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE ai_generation_attempt SET state='failed',result_code=?,"
      "provider_request_id=?,completed_at_ms=? WHERE attempt_id=? AND "
      "state='submitted'");
  update.bind(1, result_code);
  if (request_id.empty())
    update.bind_null(2);
  else
    update.bind(2, request_id);
  update.bind(3, now_ms);
  update.bind(4, attempt_id);
  update.execute();
  require_changed(connection);
}

void SqliteAiGenerationRepository::cancel(const std::string_view attempt_id,
                                          const std::string_view result_code,
                                          const std::int64_t now_ms) {
  require_timestamp(now_ms);
  require_identifier(result_code, 96);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE ai_generation_attempt SET state='cancelled',result_code=?,"
      "completed_at_ms=? WHERE attempt_id=? AND state='reserved'");
  update.bind(1, result_code);
  update.bind(2, now_ms);
  update.bind(3, attempt_id);
  update.execute();
  require_changed(connection);
}

std::size_t
SqliteAiGenerationRepository::recover_reserved(const std::int64_t now_ms) {
  require_timestamp(now_ms);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update = connection.prepare(
      "UPDATE ai_generation_attempt SET state='cancelled',"
      "result_code='process_restart_before_send',completed_at_ms=? "
      "WHERE state='reserved' AND provider_sent=0");
  update.bind(1, now_ms);
  update.execute();
  return static_cast<std::size_t>(connection.changes());
}

std::size_t
SqliteAiGenerationRepository::recover_submitted(const std::int64_t now_ms) {
  require_timestamp(now_ms);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto update =
      connection.prepare("UPDATE ai_generation_attempt SET state='unknown',"
                         "result_code='process_restart',completed_at_ms=? "
                         "WHERE state='submitted'");
  update.bind(1, now_ms);
  update.execute();
  return static_cast<std::size_t>(connection.changes());
}

void SqliteAiGenerationRepository::restart_provider(const std::int64_t now_ms,
                                                    std::string transition_id) {
  require_timestamp(now_ms);
  require_identifier(transition_id, 36);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = circuit(connection);
  if (before.state == "half_open") {
    auto release = connection.prepare(
        "UPDATE provider_circuit_state SET state='open',"
        "retry_after_ms=max(opened_at_ms,?),"
        "indefinite=0,probe_in_flight=0,last_error_code='probe_abandoned',"
        "revision=revision+1,updated_at_ms=? WHERE provider='openai_text' "
        "AND state='half_open' AND revision=?");
    release.bind(1, now_ms);
    release.bind(2, now_ms);
    release.bind(3, before.revision);
    release.execute();
    require_changed(connection);
    append_circuit_transition(connection, before, "open",
                              "process_restart_probe", transition_id, now_ms);
  } else if (before.state == "open" && before.indefinite) {
    auto close = connection.prepare(
        "UPDATE provider_circuit_state SET state='closed',"
        "consecutive_failures=0,first_failure_at_ms=NULL,opened_at_ms=NULL,"
        "retry_after_ms=NULL,indefinite=0,probe_in_flight=0,"
        "last_error_code=NULL,revision=revision+1,updated_at_ms=? "
        "WHERE provider='openai_text' AND revision=?");
    close.bind(1, now_ms);
    close.bind(2, before.revision);
    close.execute();
    require_changed(connection);
    append_circuit_transition(connection, before, "closed", "process_restart",
                              transition_id, now_ms);
  }
  transaction.commit();
}

void SqliteAiGenerationRepository::release_provider_probe(
    const std::int64_t now_ms, std::string transition_id) {
  require_timestamp(now_ms);
  require_identifier(transition_id, 36);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = circuit(connection);
  if (before.state == "half_open") {
    auto release = connection.prepare(
        "UPDATE provider_circuit_state SET state='open',"
        "retry_after_ms=max(opened_at_ms,?),"
        "indefinite=0,probe_in_flight=0,last_error_code='probe_abandoned',"
        "revision=revision+1,updated_at_ms=? WHERE provider='openai_text' "
        "AND state='half_open' AND revision=?");
    release.bind(1, now_ms);
    release.bind(2, now_ms);
    release.bind(3, before.revision);
    release.execute();
    require_changed(connection);
    append_circuit_transition(connection, before, "open", "probe_abandoned",
                              transition_id, now_ms);
  }
  transaction.commit();
}

ProviderCircuitAdmission
SqliteAiGenerationRepository::admit_provider(const std::int64_t now_ms,
                                             std::string transition_id) {
  require_timestamp(now_ms);
  require_identifier(transition_id, 36);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = circuit(connection);
  if (before.state == "closed") {
    transaction.commit();
    return ProviderCircuitAdmission::allowed;
  }
  if (before.state == "half_open" || before.indefinite || !before.retry_after ||
      now_ms < *before.retry_after) {
    transaction.commit();
    return ProviderCircuitAdmission::open;
  }
  auto update = connection.prepare(
      "UPDATE provider_circuit_state SET state='half_open',probe_in_flight=1,"
      "revision=revision+1,updated_at_ms=? WHERE provider='openai_text' AND "
      "state='open' AND revision=?");
  update.bind(1, now_ms);
  update.bind(2, before.revision);
  update.execute();
  require_changed(connection);
  append_circuit_transition(connection, before, "half_open", "cooldown_probe",
                            transition_id, now_ms);
  transaction.commit();
  return ProviderCircuitAdmission::allowed;
}

void SqliteAiGenerationRepository::provider_succeeded(
    const std::int64_t now_ms, std::string transition_id) {
  require_timestamp(now_ms);
  require_identifier(transition_id, 36);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = circuit(connection);
  if (before.state == "closed") {
    auto reset = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=0,"
        "first_failure_at_ms=NULL,last_error_code=NULL,updated_at_ms=? "
        "WHERE provider='openai_text'");
    reset.bind(1, now_ms);
    reset.execute();
    transaction.commit();
    return;
  }
  if (before.state != "half_open") {
    transaction.commit();
    return;
  }
  auto close = connection.prepare(
      "UPDATE provider_circuit_state SET state='closed',"
      "consecutive_failures=0,first_failure_at_ms=NULL,opened_at_ms=NULL,"
      "retry_after_ms=NULL,indefinite=0,probe_in_flight=0,"
      "last_error_code=NULL,revision=revision+1,updated_at_ms=? "
      "WHERE provider='openai_text' AND state='half_open' AND revision=?");
  close.bind(1, now_ms);
  close.bind(2, before.revision);
  close.execute();
  require_changed(connection);
  append_circuit_transition(connection, before, "closed", "probe_succeeded",
                            transition_id, now_ms);
  transaction.commit();
}

void SqliteAiGenerationRepository::provider_failed(
    const AiProviderErrorCategory category, const std::int64_t now_ms,
    std::string transition_id) {
  require_timestamp(now_ms);
  require_identifier(transition_id, 36);
  if (category != AiProviderErrorCategory::authentication &&
      !retryable(category))
    return;
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = circuit(connection);
  const bool authentication =
      category == AiProviderErrorCategory::authentication;
  constexpr std::int64_t failure_window_ms = 5 * 60 * 1'000;
  const bool fresh_window = !before.first_failure ||
                            now_ms - *before.first_failure > failure_window_ms;
  const auto failures = fresh_window ? std::int64_t{1} : before.failures + 1;
  const auto first = fresh_window ? now_ms : *before.first_failure;
  const bool should_open =
      authentication || before.state == "half_open" || failures >= 3;
  if (!should_open) {
    auto update = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=?,"
        "first_failure_at_ms=?,last_error_code=?,updated_at_ms=? WHERE "
        "provider='openai_text' AND state='closed'");
    update.bind(1, failures);
    update.bind(2, first);
    update.bind(3, circuit_failure_code(category));
    update.bind(4, now_ms);
    update.execute();
    transaction.commit();
    return;
  }
  if (before.state == "open") {
    if (before.indefinite) {
      transaction.commit();
      return;
    }
    auto keep = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=?,"
        "first_failure_at_ms=?,indefinite=?,retry_after_ms=?,"
        "last_error_code=?,revision=revision+1,updated_at_ms=? WHERE "
        "provider='openai_text' AND state='open' AND revision=?");
    keep.bind(1, failures);
    keep.bind(2, first);
    keep.bind(3, authentication ? std::int64_t{1} : std::int64_t{0});
    if (authentication)
      keep.bind_null(4);
    else
      keep.bind(4, now_ms + failure_window_ms);
    keep.bind(5, circuit_failure_code(category));
    keep.bind(6, now_ms);
    keep.bind(7, before.revision);
    keep.execute();
    require_changed(connection);
    append_circuit_transition(connection, before, "open",
                              authentication ? "configuration_failure"
                                             : "retryable_failure_while_open",
                              transition_id, now_ms);
    transaction.commit();
    return;
  }
  auto open = connection.prepare(
      "UPDATE provider_circuit_state SET state='open',"
      "consecutive_failures=?,first_failure_at_ms=?,opened_at_ms=?,"
      "retry_after_ms=?,indefinite=?,probe_in_flight=0,last_error_code=?,"
      "revision=revision+1,updated_at_ms=? WHERE provider='openai_text' AND "
      "revision=?");
  open.bind(1, failures);
  open.bind(2, first);
  open.bind(3, now_ms);
  if (authentication)
    open.bind_null(4);
  else
    open.bind(4, now_ms + failure_window_ms);
  open.bind(5, authentication ? std::int64_t{1} : std::int64_t{0});
  open.bind(6, circuit_failure_code(category));
  open.bind(7, now_ms);
  open.bind(8, before.revision);
  open.execute();
  require_changed(connection);
  append_circuit_transition(connection, before, "open",
                            authentication ? "configuration_failure"
                                           : "retryable_threshold",
                            transition_id, now_ms);
  transaction.commit();
}

AiGenerationHealth
SqliteAiGenerationRepository::health(const DiscordSnowflake &guild_id,
                                     const std::int64_t now_ms,
                                     const AiGenerationPolicy &policy) {
  if (!guild_id.is_set())
    throw std::invalid_argument{"AI generation health scope is invalid."};
  require_timestamp(now_ms);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  auto control = connection.prepare(
      "SELECT disabled FROM runtime_feature_control WHERE feature='text-ai'");
  if (!control.step())
    throw std::runtime_error{"Text AI runtime control is missing."};
  const auto state = circuit(connection);
  const auto day_start = now_ms > rolling_day_ms ? now_ms - rolling_day_ms : 0;
  const auto generations =
      accepted_count(connection, guild_id.str(), day_start);
  const auto day_cost = charged_usage(connection, guild_id.str(), day_start);
  const auto month_cost =
      charged_usage(connection, guild_id.str(), month_start(now_ms));
  return {
      .operator_disabled = control.column_int64(0) != 0,
      .circuit_state = state.state,
      .rolling_day_generations = static_cast<std::size_t>(generations),
      .rolling_day_micro_usd = day_cost,
      .calendar_month_micro_usd = month_cost,
      .generation_limit_exhausted =
          generations >=
          static_cast<std::int64_t>(policy.rolling_day_generations),
      .rolling_day_budget_exhausted = day_cost >= policy.rolling_day_micro_usd,
      .calendar_month_budget_exhausted =
          month_cost >= policy.calendar_month_micro_usd,
  };
}

} // namespace sanguinius::persistence
