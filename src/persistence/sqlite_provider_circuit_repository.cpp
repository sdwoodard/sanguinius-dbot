#include "sanguinius/persistence/sqlite_provider_circuit_repository.hpp"

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/transaction.hpp"
#include "sanguinius/persistent_id.hpp"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {
namespace {

constexpr std::int64_t failure_window_ms = 5 * 60 * 1'000;

void validate(const std::string_view provider, const std::int64_t now_ms,
              const std::string_view transition_id) {
  if ((provider != "openai_tts" && provider != "openai_transcription") ||
      now_ms < 0 || !valid_uuid_v4(std::string{transition_id}))
    throw std::invalid_argument{"Provider circuit request is invalid."};
}

struct CircuitRow {
  std::string state;
  std::int64_t failures{};
  std::optional<std::int64_t> first_failure;
  std::optional<std::int64_t> retry_after;
  bool indefinite{};
  std::int64_t revision{};
};

[[nodiscard]] CircuitRow load(SqliteConnection &connection,
                              const std::string_view provider) {
  auto query = connection.prepare(
      "SELECT state,consecutive_failures,first_failure_at_ms,retry_after_ms,"
      "indefinite,revision FROM provider_circuit_state WHERE provider=?");
  query.bind(1, provider);
  if (!query.step())
    throw std::runtime_error{"Provider circuit row is missing."};
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

void require_changed(SqliteConnection &connection) {
  if (connection.changes() != 1)
    throw std::runtime_error{"Provider circuit transition became stale."};
}

void append(SqliteConnection &connection, const std::string_view provider,
            const CircuitRow &before, const std::string_view target,
            const std::string_view reason, const std::string_view transition_id,
            const std::int64_t now_ms) {
  auto insert = connection.prepare(
      "INSERT INTO provider_circuit_transition(transition_id,provider,"
      "from_state,to_state,reason_code,from_revision,to_revision,"
      "occurred_at_ms,idempotency_key) VALUES(?,?,?,?,?,?,?,?,?)");
  insert.bind(1, transition_id);
  insert.bind(2, provider);
  insert.bind(3, before.state);
  insert.bind(4, target);
  insert.bind(5, reason);
  insert.bind(6, before.revision);
  insert.bind(7, before.revision + 1);
  insert.bind(8, now_ms);
  insert.bind(9, "circuit:" + std::string{provider} + ":" +
                     std::string{transition_id});
  insert.execute();
}

void close(SqliteConnection &connection, const std::string_view provider,
           const CircuitRow &before, const std::int64_t now_ms,
           const std::string_view transition_id,
           const std::string_view reason) {
  auto update = connection.prepare(
      "UPDATE provider_circuit_state SET state='closed',"
      "consecutive_failures=0,first_failure_at_ms=NULL,opened_at_ms=NULL,"
      "retry_after_ms=NULL,indefinite=0,probe_in_flight=0,"
      "last_error_code=NULL,revision=revision+1,updated_at_ms=? "
      "WHERE provider=? AND revision=?");
  update.bind(1, now_ms);
  update.bind(2, provider);
  update.bind(3, before.revision);
  update.execute();
  require_changed(connection);
  append(connection, provider, before, "closed", reason, transition_id, now_ms);
}

void release_probe(SqliteConnection &connection,
                   const std::string_view provider, const CircuitRow &before,
                   const std::int64_t now_ms,
                   const std::string_view transition_id,
                   const std::string_view reason) {
  if (before.state != "half_open")
    return;
  auto update = connection.prepare(
      "UPDATE provider_circuit_state SET state='open',"
      "retry_after_ms=max(opened_at_ms,?),"
      "indefinite=0,probe_in_flight=0,last_error_code='probe_abandoned',"
      "revision=revision+1,updated_at_ms=? WHERE provider=? AND "
      "state='half_open' AND revision=?");
  update.bind(1, now_ms);
  update.bind(2, now_ms);
  update.bind(3, provider);
  update.bind(4, before.revision);
  update.execute();
  require_changed(connection);
  append(connection, provider, before, "open", reason, transition_id, now_ms);
}

} // namespace

SqliteProviderCircuitRepository::SqliteProviderCircuitRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite repository context is required."};
}

void SqliteProviderCircuitRepository::restart(const std::string_view provider,
                                              const std::int64_t now_ms,
                                              std::string transition_id) {
  validate(provider, now_ms, transition_id);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = load(connection, provider);
  if (before.state == "half_open")
    release_probe(connection, provider, before, now_ms, transition_id,
                  "process_restart_probe");
  else if (before.state == "open" && before.indefinite)
    close(connection, provider, before, now_ms, transition_id,
          "process_restart");
  transaction.commit();
}

bool SqliteProviderCircuitRepository::admit(const std::string_view provider,
                                            const std::int64_t now_ms,
                                            std::string transition_id) {
  validate(provider, now_ms, transition_id);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = load(connection, provider);
  if (before.state == "closed") {
    transaction.commit();
    return true;
  }
  if (before.state == "half_open" || before.indefinite || !before.retry_after ||
      now_ms < *before.retry_after) {
    transaction.commit();
    return false;
  }
  auto update = connection.prepare(
      "UPDATE provider_circuit_state SET state='half_open',probe_in_flight=1,"
      "revision=revision+1,updated_at_ms=? WHERE provider=? AND state='open' "
      "AND revision=?");
  update.bind(1, now_ms);
  update.bind(2, provider);
  update.bind(3, before.revision);
  update.execute();
  require_changed(connection);
  append(connection, provider, before, "half_open", "cooldown_probe",
         transition_id, now_ms);
  transaction.commit();
  return true;
}

std::string
SqliteProviderCircuitRepository::state(const std::string_view provider) const {
  if (provider != "openai_tts" && provider != "openai_transcription")
    throw std::invalid_argument{"Provider circuit request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  return load(context_->connection(), provider).state;
}

void SqliteProviderCircuitRepository::succeeded(const std::string_view provider,
                                                const std::int64_t now_ms,
                                                std::string transition_id) {
  validate(provider, now_ms, transition_id);
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = load(connection, provider);
  if (before.state == "half_open") {
    close(connection, provider, before, now_ms, transition_id,
          "probe_succeeded");
  } else if (before.state == "closed" && before.failures != 0) {
    auto reset = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=0,"
        "first_failure_at_ms=NULL,last_error_code=NULL,updated_at_ms=? "
        "WHERE provider=? AND state='closed'");
    reset.bind(1, now_ms);
    reset.bind(2, provider);
    reset.execute();
  }
  transaction.commit();
}

void SqliteProviderCircuitRepository::failed(
    const std::string_view provider, const ProviderCircuitFailure failure,
    const std::string_view reason_code, const std::int64_t now_ms,
    std::string transition_id) {
  validate(provider, now_ms, transition_id);
  if (reason_code.empty() || reason_code.size() > 96)
    throw std::invalid_argument{"Provider circuit reason is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  const auto before = load(connection, provider);
  if (failure == ProviderCircuitFailure::ignored) {
    release_probe(connection, provider, before, now_ms, transition_id,
                  "probe_abandoned");
    transaction.commit();
    return;
  }
  const bool authentication = failure == ProviderCircuitFailure::authentication;
  const bool fresh = !before.first_failure ||
                     now_ms - *before.first_failure > failure_window_ms;
  const auto failures = fresh ? std::int64_t{1} : before.failures + 1;
  const auto first = fresh ? now_ms : *before.first_failure;
  const bool should_open =
      authentication || before.state == "half_open" || failures >= 3;
  if (!should_open) {
    auto update = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=?,"
        "first_failure_at_ms=?,last_error_code=?,updated_at_ms=? "
        "WHERE provider=? AND state='closed'");
    update.bind(1, failures);
    update.bind(2, first);
    update.bind(3, reason_code);
    update.bind(4, now_ms);
    update.bind(5, provider);
    update.execute();
    transaction.commit();
    return;
  }
  if (before.state == "open") {
    if (before.indefinite) {
      transaction.commit();
      return;
    }
    auto update = connection.prepare(
        "UPDATE provider_circuit_state SET consecutive_failures=?,"
        "first_failure_at_ms=?,indefinite=?,retry_after_ms=?,"
        "last_error_code=?,revision=revision+1,updated_at_ms=? WHERE "
        "provider=? AND state='open' AND revision=?");
    update.bind(1, failures);
    update.bind(2, first);
    update.bind(3, authentication ? std::int64_t{1} : std::int64_t{0});
    if (authentication)
      update.bind_null(4);
    else
      update.bind(4, now_ms + failure_window_ms);
    update.bind(5, reason_code);
    update.bind(6, now_ms);
    update.bind(7, provider);
    update.bind(8, before.revision);
    update.execute();
    require_changed(connection);
    append(connection, provider, before, "open",
           authentication ? "configuration_failure"
                          : "retryable_failure_while_open",
           transition_id, now_ms);
    transaction.commit();
    return;
  }
  auto update = connection.prepare(
      "UPDATE provider_circuit_state SET state='open',"
      "consecutive_failures=?,first_failure_at_ms=?,opened_at_ms=?,"
      "retry_after_ms=?,indefinite=?,probe_in_flight=0,last_error_code=?,"
      "revision=revision+1,updated_at_ms=? WHERE provider=? AND revision=?");
  update.bind(1, failures);
  update.bind(2, first);
  update.bind(3, now_ms);
  if (authentication)
    update.bind_null(4);
  else
    update.bind(4, now_ms + failure_window_ms);
  update.bind(5, authentication ? std::int64_t{1} : std::int64_t{0});
  update.bind(6, reason_code);
  update.bind(7, now_ms);
  update.bind(8, provider);
  update.bind(9, before.revision);
  update.execute();
  require_changed(connection);
  append(connection, provider, before, "open",
         authentication ? "configuration_failure" : "retryable_threshold",
         transition_id, now_ms);
  transaction.commit();
}

} // namespace sanguinius::persistence
