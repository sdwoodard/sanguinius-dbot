#pragma once

#include "sanguinius/id_generator.hpp"
#include "sanguinius/persistent_id.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeIdGenerator final : public IdGenerator {
public:
  explicit FakeIdGenerator(std::vector<std::string> ids = {"test-id"})
      : ids_{std::move(ids)} {}

  [[nodiscard]] std::string next_id() override {
    const std::scoped_lock lock{mutex_};
    if (next_ < ids_.size()) {
      return ids_[next_++];
    }
    return "test-id-" + std::to_string(++fallback_);
  }

private:
  std::mutex mutex_;
  std::vector<std::string> ids_;
  std::size_t next_{0};
  std::size_t fallback_{0};
};

class FakePersistentIdGenerator final : public PersistentIdGenerator {
public:
  explicit FakePersistentIdGenerator(
      std::vector<std::string> ids = {"00000000-0000-4000-8000-000000000001"})
      : ids_{std::move(ids)} {}

  [[nodiscard]] std::string next_id() override {
    const std::scoped_lock lock{mutex_};
    if (next_ < ids_.size()) {
      return ids_[next_++];
    }
    ++fallback_;
    std::string suffix = std::to_string(fallback_);
    suffix.insert(suffix.begin(), 12U - suffix.size(), '0');
    return "00000000-0000-4000-8000-" + suffix;
  }

private:
  std::mutex mutex_;
  std::vector<std::string> ids_;
  std::size_t next_{0};
  std::size_t fallback_{1};
};

} // namespace sanguinius::test
