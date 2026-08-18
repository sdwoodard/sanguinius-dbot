#pragma once

#include "sanguinius/message_log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace sanguinius::test {

class FakeMessageLog final : public MessageLog {
public:
  void append(const LoggedMessage &message) override {
    std::unique_lock lock{mutex_};
    messages_.push_back(message);
    entered_ = true;
    changed_.notify_all();
    if (failure_) {
      throw std::runtime_error{"scripted message log failure"};
    }
    changed_.wait(lock, [this] { return !blocked_ || released_; });
  }

  void fail() {
    const std::scoped_lock lock{mutex_};
    failure_ = true;
  }

  void block() {
    const std::scoped_lock lock{mutex_};
    blocked_ = true;
    released_ = false;
    entered_ = false;
  }

  void release() {
    {
      const std::scoped_lock lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool
  wait_until_entered(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  [[nodiscard]] bool
  wait_for_count(const std::size_t count,
                 const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return messages_.size() >= count; });
  }

  [[nodiscard]] std::vector<LoggedMessage> messages() const {
    const std::scoped_lock lock{mutex_};
    return messages_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<LoggedMessage> messages_;
  bool blocked_{false};
  bool released_{false};
  bool entered_{false};
  bool failure_{false};
};

} // namespace sanguinius::test
