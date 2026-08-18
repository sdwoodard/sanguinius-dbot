#include "sanguinius/callback_fence.hpp"

#include <exception>

namespace sanguinius {

bool CallbackFence::invoke(const std::function<void()> &callback) {
  {
    const std::scoped_lock lock{mutex_};
    if (!accepting_) {
      return false;
    }
    ++active_;
  }

  try {
    callback();
  } catch (...) {
    finish();
    throw;
  }
  finish();
  return true;
}

void CallbackFence::close_and_wait() noexcept {
  try {
    std::unique_lock lock{mutex_};
    accepting_ = false;
    changed_.wait(lock, [this] { return active_ == 0; });
  } catch (...) {
    std::terminate();
  }
}

void CallbackFence::finish() noexcept {
  try {
    const std::scoped_lock lock{mutex_};
    --active_;
    if (active_ == 0) {
      changed_.notify_all();
    }
  } catch (...) {
    std::terminate();
  }
}

} // namespace sanguinius
