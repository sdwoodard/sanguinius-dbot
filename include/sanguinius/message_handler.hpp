#pragma once

#include "sanguinius/ai_responder.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/message_log.hpp"
#include "sanguinius/owner_admin.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace sanguinius {

enum class Command {
  none,
  help,
  repo,
};

[[nodiscard]] Command parse_command(std::string_view content,
                                    std::string_view prefix);

class MessageHandler {
public:
  MessageHandler(MessageLog &message_log, AiResponder &ai_responder,
                 DiscordTextDelivery &delivery, Diagnostics &diagnostics,
                 OwnerAdminService &owner_admin, std::string command_prefix,
                 std::size_t queue_capacity = 64);
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
  void send_help(const IncomingMessage &message) const;
  void send_repo(const IncomingMessage &message) const;
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
  BoundedExecutor worker_;
};

} // namespace sanguinius
