#pragma once

#include "sanguinius/random.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeRandom final : public Random {
public:
  explicit FakeRandom(std::vector<std::uint64_t> values)
      : values_{std::move(values)} {}

  [[nodiscard]] std::uint64_t
  uniform(const std::uint64_t upper_exclusive) override {
    if (next_ >= values_.size() || upper_exclusive == 0 ||
        values_[next_] >= upper_exclusive)
      throw std::runtime_error{
          "Fake random value is unavailable or out of range."};
    return values_[next_++];
  }

  [[nodiscard]] std::size_t call_count() const noexcept { return next_; }

private:
  std::vector<std::uint64_t> values_;
  std::size_t next_{};
};

} // namespace sanguinius::test
