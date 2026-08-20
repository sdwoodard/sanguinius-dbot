#pragma once

#include <cstdint>

namespace sanguinius {

class Random {
public:
  virtual ~Random() = default;

  // Returns a value in [0, upper_exclusive). Implementations must reject zero.
  [[nodiscard]] virtual std::uint64_t uniform(std::uint64_t upper_exclusive) = 0;
};

class SystemRandom final : public Random {
public:
  [[nodiscard]] std::uint64_t uniform(std::uint64_t upper_exclusive) override;
};

} // namespace sanguinius
