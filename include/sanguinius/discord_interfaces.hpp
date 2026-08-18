#pragma once

#include "sanguinius/discord_types.hpp"

#include <cstddef>
#include <functional>
#include <stop_token>
#include <vector>

namespace sanguinius {

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

  virtual ~DiscordGateway() = default;

  virtual void start(MessageCallback message_callback) = 0;
  virtual void stop_accepting() noexcept = 0;
  virtual void shutdown() noexcept = 0;
};

class DiscordRuntime : public DiscordGateway,
                       public DiscordTextDelivery,
                       public DiscordConversation {
public:
  ~DiscordRuntime() override = default;
};

} // namespace sanguinius
