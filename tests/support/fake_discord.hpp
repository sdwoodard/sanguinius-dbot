#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/discord_interfaces.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeDiscord final : public DiscordRuntime {
public:
  void start(MessageCallback message_callback) override {
    const std::scoped_lock lock{mutex_};
    lifecycle_.push_back("gateway.start");
    callback_ = std::move(message_callback);
    accepting_ = true;
    if (fail_start_) {
      throw std::runtime_error{"scripted gateway startup failure"};
    }
  }

  void stop_accepting() noexcept override {
    try {
      const std::scoped_lock lock{mutex_};
      lifecycle_.push_back("gateway.stop_accepting");
      accepting_ = false;
    } catch (...) {
    }
  }

  void shutdown() noexcept override {
    std::function<void()> observer;
    try {
      {
        const std::scoped_lock lock{mutex_};
        if (shutdown_) {
          return;
        }
        lifecycle_.push_back("gateway.shutdown");
        shutdown_ = true;
        accepting_ = false;
        callback_ = {};
        observer = shutdown_observer_;
      }
      if (observer) {
        observer();
      }
    } catch (...) {
    }
  }

  void show_typing(const DiscordId channel_id) override {
    const std::scoped_lock lock{mutex_};
    typing_channels_.push_back(channel_id);
    changed_.notify_all();
  }

  void reply(const ReplyRequest &request) override {
    const std::scoped_lock lock{mutex_};
    if (delivery_failure_) {
      throw std::runtime_error{"scripted delivery failure"};
    }
    replies_.push_back(request);
    changed_.notify_all();
  }

  [[nodiscard]] std::vector<ConversationEntry>
  recent_messages(const IncomingMessage &, std::size_t,
                  const std::stop_token stop_token) override {
    const std::scoped_lock lock{mutex_};
    if (stop_token.stop_requested()) {
      throw OperationCancelled{};
    }
    if (history_failure_) {
      throw std::runtime_error{"scripted history failure"};
    }
    return recent_;
  }

  [[nodiscard]] ConversationEntry
  message(const DiscordId message_id, DiscordId,
          const std::stop_token stop_token) override {
    const std::scoped_lock lock{mutex_};
    if (stop_token.stop_requested()) {
      throw OperationCancelled{};
    }
    if (reply_context_failure_) {
      throw std::runtime_error{"scripted reply context failure"};
    }
    const auto found = fetched_.find(message_id);
    if (found == fetched_.end()) {
      throw std::runtime_error{"No scripted Discord message."};
    }
    return found->second;
  }

  void emit(IncomingMessage message) {
    MessageCallback callback;
    {
      const std::scoped_lock lock{mutex_};
      if (!accepting_ || !callback_) {
        return;
      }
      callback = callback_;
    }
    callback(std::move(message));
  }

  void set_recent(std::vector<ConversationEntry> recent) {
    const std::scoped_lock lock{mutex_};
    recent_ = std::move(recent);
  }

  void set_fetched(ConversationEntry message) {
    const std::scoped_lock lock{mutex_};
    fetched_.insert_or_assign(message.message_id, std::move(message));
  }

  void fail_history() {
    const std::scoped_lock lock{mutex_};
    history_failure_ = true;
  }

  void fail_reply_context() {
    const std::scoped_lock lock{mutex_};
    reply_context_failure_ = true;
  }

  void fail_start() {
    const std::scoped_lock lock{mutex_};
    fail_start_ = true;
  }

  void set_shutdown_observer(std::function<void()> observer) {
    const std::scoped_lock lock{mutex_};
    shutdown_observer_ = std::move(observer);
  }

  [[nodiscard]] bool
  wait_for_reply_count(const std::size_t count,
                       const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return replies_.size() >= count; });
  }

  [[nodiscard]] std::vector<ReplyRequest> replies() const {
    const std::scoped_lock lock{mutex_};
    return replies_;
  }

  [[nodiscard]] std::vector<DiscordId> typing_channels() const {
    const std::scoped_lock lock{mutex_};
    return typing_channels_;
  }

  [[nodiscard]] std::vector<std::string> lifecycle() const {
    const std::scoped_lock lock{mutex_};
    return lifecycle_;
  }

  [[nodiscard]] bool shutdown_called() const {
    const std::scoped_lock lock{mutex_};
    return shutdown_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  MessageCallback callback_;
  std::vector<ConversationEntry> recent_;
  std::unordered_map<DiscordId, ConversationEntry> fetched_;
  std::vector<ReplyRequest> replies_;
  std::vector<DiscordId> typing_channels_;
  std::vector<std::string> lifecycle_;
  std::function<void()> shutdown_observer_;
  bool accepting_{false};
  bool shutdown_{false};
  bool fail_start_{false};
  bool history_failure_{false};
  bool reply_context_failure_{false};
  bool delivery_failure_{false};
};

[[nodiscard]] inline IncomingMessage
incoming(std::string content, const DiscordId message_id = 100,
         const bool author_is_bot = false) {
  return IncomingMessage{
      .correlation_id = {},
      .bot_user_id = 42,
      .message_id = message_id,
      .guild_id = 10,
      .channel_id = 20,
      .author_username = "test-user",
      .author_display_name = "Test User",
      .content = std::move(content),
      .author_is_bot = author_is_bot,
      .replied_to = std::nullopt,
  };
}

} // namespace sanguinius::test
