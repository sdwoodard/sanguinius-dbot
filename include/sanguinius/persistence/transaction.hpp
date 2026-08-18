#pragma once

#include "sanguinius/persistence/sqlite.hpp"

#include <cstdint>
#include <string>

namespace sanguinius::persistence {

enum class TransactionMode {
  deferred,
  immediate,
  exclusive,
};

class Transaction {
public:
  explicit Transaction(SqliteConnection &connection,
                       TransactionMode mode = TransactionMode::deferred);
  ~Transaction();
  Transaction(const Transaction &) = delete;
  Transaction &operator=(const Transaction &) = delete;
  Transaction(Transaction &&) = delete;
  Transaction &operator=(Transaction &&) = delete;

  void commit();
  void rollback();
  [[nodiscard]] bool active() const noexcept;

private:
  SqliteConnection &connection_;
  bool active_{true};
};

class Savepoint {
public:
  explicit Savepoint(SqliteConnection &connection);
  ~Savepoint();
  Savepoint(const Savepoint &) = delete;
  Savepoint &operator=(const Savepoint &) = delete;
  Savepoint(Savepoint &&) = delete;
  Savepoint &operator=(Savepoint &&) = delete;

  void release();
  void rollback();

private:
  SqliteConnection &connection_;
  std::string name_;
  bool active_{true};
};

} // namespace sanguinius::persistence
