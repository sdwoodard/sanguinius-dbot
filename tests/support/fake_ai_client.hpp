#pragma once

#include "sanguinius/ai_client.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeAiClient final : public AiClient {
public:
  [[nodiscard]] std::string
  generate(const AiRequest &request,
           const std::stop_token stop_token) const override {
    std::unique_lock lock{mutex_};
    requests_.push_back(request);
    entered_ = true;
    changed_.notify_all();

    std::stop_callback notify_on_stop{stop_token,
                                      [this] { changed_.notify_all(); }};
    changed_.wait(lock, [this, stop_token] {
      return !blocked_ || released_ || stop_token.stop_requested();
    });
    if (stop_token.stop_requested()) {
      cancelled_ = true;
      changed_.notify_all();
      throw OperationCancelled{};
    }
    if (failure_) {
      throw std::runtime_error{failure_message_};
    }
    return response_;
  }

  void set_response(std::string response) {
    const std::scoped_lock lock{mutex_};
    response_ = std::move(response);
  }

  void fail(std::string message = "scripted AI failure") {
    const std::scoped_lock lock{mutex_};
    failure_ = true;
    failure_message_ = std::move(message);
  }

  void block() {
    const std::scoped_lock lock{mutex_};
    blocked_ = true;
    released_ = false;
    entered_ = false;
  }

  void release() const {
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
  wait_for_request_count(const std::size_t count,
                         const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return requests_.size() >= count; });
  }

  [[nodiscard]] std::vector<AiRequest> requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

  [[nodiscard]] bool cancelled() const {
    const std::scoped_lock lock{mutex_};
    return cancelled_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  mutable std::vector<AiRequest> requests_;
  mutable std::string response_{"scripted response"};
  mutable std::string failure_message_{"scripted AI failure"};
  mutable bool failure_{false};
  mutable bool blocked_{false};
  mutable bool released_{false};
  mutable bool entered_{false};
  mutable bool cancelled_{false};
};

} // namespace sanguinius::test
