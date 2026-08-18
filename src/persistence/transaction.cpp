#include "sanguinius/persistence/transaction.hpp"

#include <atomic>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] const char *begin_sql(const TransactionMode mode) noexcept {
  switch (mode) {
  case TransactionMode::deferred:
    return "BEGIN DEFERRED";
  case TransactionMode::immediate:
    return "BEGIN IMMEDIATE";
  case TransactionMode::exclusive:
    return "BEGIN EXCLUSIVE";
  }
  return "BEGIN DEFERRED";
}

} // namespace

Transaction::Transaction(SqliteConnection &connection,
                         const TransactionMode mode)
    : connection_{connection} {
  connection_.execute(begin_sql(mode));
}

Transaction::~Transaction() {
  if (active_) {
    try {
      connection_.execute("ROLLBACK");
    } catch (...) {
    }
  }
}

void Transaction::commit() {
  if (!active_) {
    throw DatabaseError{DatabaseErrorCategory::other, 0, 0,
                        "Database transaction is no longer active."};
  }
  connection_.execute("COMMIT");
  active_ = false;
}

void Transaction::rollback() {
  if (!active_) {
    return;
  }
  connection_.execute("ROLLBACK");
  active_ = false;
}

bool Transaction::active() const noexcept { return active_; }

Savepoint::Savepoint(SqliteConnection &connection) : connection_{connection} {
  static std::atomic<std::uint64_t> sequence{0};
  name_ = "sanguinius_savepoint_" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  connection_.execute("SAVEPOINT " + name_);
}

Savepoint::~Savepoint() {
  if (active_) {
    try {
      connection_.execute("ROLLBACK TO " + name_);
      connection_.execute("RELEASE " + name_);
    } catch (...) {
    }
  }
}

void Savepoint::release() {
  if (!active_) {
    throw DatabaseError{DatabaseErrorCategory::other, 0, 0,
                        "Database savepoint is no longer active."};
  }
  connection_.execute("RELEASE " + name_);
  active_ = false;
}

void Savepoint::rollback() {
  if (!active_) {
    return;
  }
  connection_.execute("ROLLBACK TO " + name_);
  connection_.execute("RELEASE " + name_);
  active_ = false;
}

} // namespace sanguinius::persistence
