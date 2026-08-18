#pragma once

#include "sanguinius/persistent_id.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakePersistentIdGenerator final : public PersistentIdGenerator {
public:
  explicit FakePersistentIdGenerator(std::vector<std::string> ids)
      : ids_{std::move(ids)} {}

  [[nodiscard]] std::string next_id() override {
    if (next_ >= ids_.size()) {
      throw std::runtime_error{"No deterministic persistent ID remains."};
    }
    return ids_[next_++];
  }

private:
  std::vector<std::string> ids_;
  std::size_t next_{};
};

} // namespace sanguinius::test
