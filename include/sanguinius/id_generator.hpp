#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace sanguinius {

class IdGenerator {
public:
  virtual ~IdGenerator() = default;

  // Correlation IDs are process-local observability values, not security
  // tokens or stable persistent identifiers.
  [[nodiscard]] virtual std::string next_id() = 0;
};

class ProcessIdGenerator final : public IdGenerator {
public:
  ProcessIdGenerator();

  [[nodiscard]] std::string next_id() override;

private:
  std::uint64_t prefix_;
  std::atomic<std::uint64_t> counter_{0};
};

} // namespace sanguinius
