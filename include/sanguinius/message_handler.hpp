#pragma once

#include "sanguinius/ai_responder.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/message_log.hpp"
#include "sanguinius/owner_admin.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace sanguinius {

class MessageHandler {
public:
  MessageHandler(
      MessageLog &message_log, AiResponder &ai_responder,
      DiscordTextDelivery &delivery, Diagnostics &diagnostics,
      OwnerAdminService &owner_admin, std::string command_prefix,
      std::size_t queue_capacity = 64,
      std::function<void(const IncomingMessage &)> observer = {},
      std::function<std::string(const HealthSnapshot &)> health_renderer = {});
  ~MessageHandler();

  MessageHandler(const MessageHandler &) = delete;
  MessageHandler &operator=(const MessageHandler &) = delete;
  MessageHandler(MessageHandler &&) = delete;
  MessageHandler &operator=(MessageHandler &&) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult enqueue(IncomingMessage message);
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void process(const IncomingMessage &message) const;
  void send_health(const IncomingMessage &message,
                   const HealthSnapshot &snapshot) const;
  void send_overload(const IncomingMessage &message) const noexcept;
  [[nodiscard]] bool actionable(const IncomingMessage &message) const;

  MessageLog &message_log_;
  AiResponder &ai_responder_;
  DiscordTextDelivery &delivery_;
  Diagnostics &diagnostics_;
  OwnerAdminService &owner_admin_;
  std::string command_prefix_;
  std::function<void(const IncomingMessage &)> observer_;
  std::function<std::string(const HealthSnapshot &)> health_renderer_;
  BoundedExecutor worker_;
};

} // namespace sanguinius
