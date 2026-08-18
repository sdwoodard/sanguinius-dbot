#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>

namespace sanguinius {

class CallbackFence {
public:
  CallbackFence() = default;

  CallbackFence(const CallbackFence &) = delete;
  CallbackFence &operator=(const CallbackFence &) = delete;

  [[nodiscard]] bool invoke(const std::function<void()> &callback);
  void close_and_wait() noexcept;

private:
  void finish() noexcept;

  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t active_{};
  bool accepting_{true};
};

} // namespace sanguinius
