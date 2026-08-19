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

class FakeInteractionResponder final : public DiscordInteractionResponder {
public:
  void reply(const InteractionMessage &message,
             const ResponseVisibility visibility,
             DeliveryCallback callback = {}) override {
    {
      const std::scoped_lock lock{mutex_};
      replies_.emplace_back(message, visibility);
    }
    if (callback) {
      callback(reply_result_);
    }
  }

  void defer(const ResponseVisibility visibility,
             DeliveryCallback callback = {}) override {
    {
      const std::scoped_lock lock{mutex_};
      deferrals_.push_back(visibility);
    }
    if (callback) {
      callback(defer_result_);
    }
  }

  void edit_original(const InteractionMessage &message,
                     DeliveryCallback callback = {}) override {
    DeliveryResult result;
    bool hold_completion;
    {
      const std::scoped_lock lock{mutex_};
      edits_.push_back(message);
      result = edit_result_;
      hold_completion = hold_edit_completions_;
      if (callback && hold_completion) {
        pending_edit_completions_.push_back(std::move(callback));
      }
    }
    if (callback && !hold_completion) {
      callback(result);
    }
    changed_.notify_all();
  }

  void follow_up(const InteractionMessage &message,
                 const ResponseVisibility visibility,
                 DeliveryCallback callback = {}) override {
    {
      const std::scoped_lock lock{mutex_};
      follow_ups_.emplace_back(message, visibility);
    }
    if (callback) {
      callback(DeliveryResult::success);
    }
  }

  void show_modal(const ModalPayload &modal,
                  DeliveryCallback callback = {}) override {
    {
      const std::scoped_lock lock{mutex_};
      modals_.push_back(modal);
    }
    if (callback) {
      callback(DeliveryResult::success);
    }
  }

  void set_defer_result(const DeliveryResult result) {
    const std::scoped_lock lock{mutex_};
    defer_result_ = result;
  }
  void set_edit_result(const DeliveryResult result) {
    const std::scoped_lock lock{mutex_};
    edit_result_ = result;
  }
  void hold_edit_completions(const bool hold = true) {
    const std::scoped_lock lock{mutex_};
    hold_edit_completions_ = hold;
  }
  void complete_edit_completions() {
    std::vector<DeliveryCallback> callbacks;
    DeliveryResult result;
    {
      const std::scoped_lock lock{mutex_};
      callbacks.swap(pending_edit_completions_);
      result = edit_result_;
    }
    for (auto &callback : callbacks) {
      callback(result);
    }
  }

  [[nodiscard]] bool
  wait_for_edit_count(const std::size_t count,
                      const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return edits_.size() >= count; });
  }

  [[nodiscard]] std::vector<InteractionMessage> edits() const {
    const std::scoped_lock lock{mutex_};
    return edits_;
  }

  [[nodiscard]] std::vector<std::pair<InteractionMessage, ResponseVisibility>>
  replies() const {
    const std::scoped_lock lock{mutex_};
    return replies_;
  }

  [[nodiscard]] std::vector<ResponseVisibility> deferrals() const {
    const std::scoped_lock lock{mutex_};
    return deferrals_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<std::pair<InteractionMessage, ResponseVisibility>> replies_;
  std::vector<ResponseVisibility> deferrals_;
  std::vector<InteractionMessage> edits_;
  std::vector<std::pair<InteractionMessage, ResponseVisibility>> follow_ups_;
  std::vector<ModalPayload> modals_;
  DeliveryResult reply_result_{DeliveryResult::success};
  DeliveryResult defer_result_{DeliveryResult::success};
  DeliveryResult edit_result_{DeliveryResult::success};
  bool hold_edit_completions_{};
  std::vector<DeliveryCallback> pending_edit_completions_;
};

class FakeDiscord final : public DiscordRuntime {
public:
  void start(MessageCallback message_callback,
             InteractionCallback interaction_callback,
             CommandCatalog command_catalog) override {
    const std::scoped_lock lock{mutex_};
    lifecycle_.push_back("gateway.start");
    callback_ = std::move(message_callback);
    interaction_callback_ = std::move(interaction_callback);
    command_catalog_ = std::move(command_catalog);
    accepting_ = true;
    status_ = {.ready = ready_on_start_,
               .command_registration = CommandRegistrationState::synchronized,
               .command_catalog_version = command_catalog_.version};
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
        interaction_callback_ = {};
        status_.ready = false;
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

  void send_public(const PublicMessageRequest &request,
                   const std::string_view provider_nonce,
                   PublicDeliveryCallback callback = {}) override {
    PublicDeliveryReceipt receipt;
    bool hold_callback{};
    {
      const std::scoped_lock lock{mutex_};
      const auto existing = public_nonces_.find(std::string{provider_nonce});
      if (existing != public_nonces_.end()) {
        receipt = {DeliveryResult::success, existing->second};
      } else {
        const auto result = public_delivery_results_.empty()
                                ? public_delivery_result_
                                : public_delivery_results_.front();
        if (!public_delivery_results_.empty()) {
          public_delivery_results_.erase(public_delivery_results_.begin());
        }
        receipt.result = result;
        if (result == DeliveryResult::success) {
          const DiscordId message_id{next_public_message_id_++};
          public_messages_.push_back(request);
          public_nonces_.emplace(std::string{provider_nonce}, message_id);
          if (omit_next_success_message_id_) {
            omit_next_success_message_id_ = false;
          } else {
            receipt.provider_message_id = message_id;
          }
        } else if (result == DeliveryResult::unknown_outcome &&
                   accept_unknown_delivery_) {
          const DiscordId message_id{next_public_message_id_++};
          public_messages_.push_back(request);
          public_nonces_.emplace(std::string{provider_nonce}, message_id);
        }
      }
      hold_callback = hold_public_callbacks_ && static_cast<bool>(callback);
      if (hold_callback) {
        held_public_callbacks_.emplace_back(std::move(callback), receipt);
      }
      changed_.notify_all();
    }
    if (callback && !hold_callback) {
      callback(receipt);
    }
  }

  [[nodiscard]] DiscordRuntimeStatus status() const noexcept override {
    try {
      const std::scoped_lock lock{mutex_};
      return status_;
    } catch (...) {
      return {};
    }
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

  void emit(IncomingInteraction interaction) {
    InteractionCallback callback;
    {
      const std::scoped_lock lock{mutex_};
      if (!accepting_ || !interaction_callback_) {
        return;
      }
      callback = interaction_callback_;
    }
    callback(std::move(interaction));
  }

  void set_public_delivery_result(const DeliveryResult result) {
    const std::scoped_lock lock{mutex_};
    public_delivery_result_ = result;
  }

  void set_public_delivery_results(std::vector<DeliveryResult> results) {
    const std::scoped_lock lock{mutex_};
    public_delivery_results_ = std::move(results);
  }

  void accept_unknown_delivery(const bool value = true) {
    const std::scoped_lock lock{mutex_};
    accept_unknown_delivery_ = value;
  }

  void omit_next_success_message_id() {
    const std::scoped_lock lock{mutex_};
    omit_next_success_message_id_ = true;
  }

  void hold_public_callbacks(const bool value = true) {
    const std::scoped_lock lock{mutex_};
    hold_public_callbacks_ = value;
  }

  void set_ready_on_start(const bool ready) {
    const std::scoped_lock lock{mutex_};
    ready_on_start_ = ready;
  }

  void release_public_callbacks() {
    std::vector<std::pair<PublicDeliveryCallback, PublicDeliveryReceipt>> held;
    {
      const std::scoped_lock lock{mutex_};
      held.swap(held_public_callbacks_);
    }
    for (auto &[callback, receipt] : held) {
      callback(std::move(receipt));
    }
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

  [[nodiscard]] bool
  wait_for_public_message_count(const std::size_t count,
                                const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, count] {
      return public_messages_.size() >= count;
    });
  }

  [[nodiscard]] bool wait_for_held_public_callback_count(
      const std::size_t count, const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, count] {
      return held_public_callbacks_.size() >= count;
    });
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

  [[nodiscard]] std::vector<PublicMessageRequest> public_messages() const {
    const std::scoped_lock lock{mutex_};
    return public_messages_;
  }

  [[nodiscard]] CommandCatalog command_catalog() const {
    const std::scoped_lock lock{mutex_};
    return command_catalog_;
  }

  [[nodiscard]] bool shutdown_called() const {
    const std::scoped_lock lock{mutex_};
    return shutdown_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  MessageCallback callback_;
  InteractionCallback interaction_callback_;
  CommandCatalog command_catalog_;
  DiscordRuntimeStatus status_;
  std::vector<ConversationEntry> recent_;
  std::unordered_map<DiscordId, ConversationEntry> fetched_;
  std::vector<ReplyRequest> replies_;
  std::vector<PublicMessageRequest> public_messages_;
  std::unordered_map<std::string, DiscordId> public_nonces_;
  std::vector<std::pair<PublicDeliveryCallback, PublicDeliveryReceipt>>
      held_public_callbacks_;
  std::vector<DiscordId> typing_channels_;
  std::vector<std::string> lifecycle_;
  std::function<void()> shutdown_observer_;
  bool accepting_{false};
  bool shutdown_{false};
  bool fail_start_{false};
  bool history_failure_{false};
  bool reply_context_failure_{false};
  bool delivery_failure_{false};
  DeliveryResult public_delivery_result_{DeliveryResult::success};
  std::vector<DeliveryResult> public_delivery_results_;
  bool accept_unknown_delivery_{};
  bool omit_next_success_message_id_{};
  bool hold_public_callbacks_{};
  bool ready_on_start_{true};
  std::uint64_t next_public_message_id_{1'000};
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
      .author_user_id = 30,
      .author_username = "test-user",
      .author_display_name = "Test User",
      .content = std::move(content),
      .author_is_bot = author_is_bot,
      .replied_to = std::nullopt,
  };
}

[[nodiscard]] inline IncomingInteraction
interaction(std::shared_ptr<DiscordInteractionResponder> responder,
            const InteractionKind kind = InteractionKind::slash_command,
            const DiscordId interaction_id = 200, const DiscordId guild_id = 10,
            const DiscordId channel_id = 20, const DiscordId user_id = 30) {
  return IncomingInteraction{
      .correlation_id = {},
      .interaction_id = interaction_id,
      .guild_id = guild_id,
      .channel_id = channel_id,
      .user_id = user_id,
      .username = "test-user",
      .display_name = "Test User",
      .kind = kind,
      .command_name = {},
      .subcommand_name = {},
      .command_options = {},
      .custom_id = {},
      .selected_values = {},
      .modal_fields = {},
      .context_message = std::nullopt,
      .responder = std::move(responder),
  };
}

} // namespace sanguinius::test
