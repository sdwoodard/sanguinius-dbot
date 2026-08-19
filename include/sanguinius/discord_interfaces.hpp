#pragma once

#include "sanguinius/discord_types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>
#include <vector>

namespace sanguinius {

using DeliveryCallback = std::function<void(DeliveryResult)>;
using PublicDeliveryCallback = std::function<void(PublicDeliveryReceipt)>;

class DiscordInteractionResponder {
public:
  virtual ~DiscordInteractionResponder() = default;

  virtual void reply(const InteractionMessage &message,
                     ResponseVisibility visibility,
                     DeliveryCallback callback = {}) = 0;
  virtual void defer(ResponseVisibility visibility,
                     DeliveryCallback callback = {}) = 0;
  virtual void edit_original(const InteractionMessage &message,
                             DeliveryCallback callback = {}) = 0;
  virtual void follow_up(const InteractionMessage &message,
                         ResponseVisibility visibility,
                         DeliveryCallback callback = {}) = 0;
  virtual void show_modal(const ModalPayload &modal,
                          DeliveryCallback callback = {}) = 0;
};

class DiscordTextDelivery {
public:
  virtual ~DiscordTextDelivery() = default;

  virtual void show_typing(DiscordId channel_id) = 0;
  virtual void reply(const ReplyRequest &request) = 0;
};

class DiscordConversation {
public:
  virtual ~DiscordConversation() = default;

  [[nodiscard]] virtual std::vector<ConversationEntry>
  recent_messages(const IncomingMessage &message, std::size_t limit,
                  std::stop_token stop_token) = 0;
  [[nodiscard]] virtual ConversationEntry
  message(DiscordId message_id, DiscordId channel_id,
          std::stop_token stop_token) = 0;
};

class DiscordGateway {
public:
  using MessageCallback = std::function<void(IncomingMessage)>;
  using InteractionCallback = std::function<void(IncomingInteraction)>;

  virtual ~DiscordGateway() = default;

  virtual void start(MessageCallback message_callback,
                     InteractionCallback interaction_callback,
                     CommandCatalog command_catalog) = 0;
  virtual void stop_accepting() noexcept = 0;
  /**
   * Stop the gateway and fence every callback previously accepted by this
   * runtime. On return, callbacks have either completed or are permanently
   * suppressed.
   */
  virtual void shutdown() noexcept = 0;
};

class DiscordPublicDelivery {
public:
  virtual ~DiscordPublicDelivery() = default;

  virtual void send_public(const PublicMessageRequest &request,
                           std::string_view provider_nonce,
                           PublicDeliveryCallback callback = {}) = 0;
};

class DiscordStatusProvider {
public:
  virtual ~DiscordStatusProvider() = default;
  [[nodiscard]] virtual DiscordRuntimeStatus status() const noexcept = 0;
};

class DiscordRuntime : public DiscordGateway,
                       public DiscordTextDelivery,
                       public DiscordConversation,
                       public DiscordPublicDelivery,
                       public DiscordStatusProvider {
public:
  ~DiscordRuntime() override = default;
};

} // namespace sanguinius
