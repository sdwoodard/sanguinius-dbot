#pragma once

#include "sanguinius/message_logger.hpp"

#include <dpp/dpp.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

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
  MessageHandler(MessageLogger &logger, std::string command_prefix);
  ~MessageHandler();

  MessageHandler(const MessageHandler &) = delete;
  MessageHandler &operator=(const MessageHandler &) = delete;
  MessageHandler(MessageHandler &&) = delete;
  MessageHandler &operator=(MessageHandler &&) = delete;

  void operator()(const dpp::message_create_t &event) noexcept;

private:
  void run();
  void dispatch_command(const dpp::message_create_t &event) const;
  void send_help(const dpp::message_create_t &event) const;
  void send_repo(const dpp::message_create_t &event) const;
  MessageLogger &logger_;
  std::string command_prefix_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::queue<dpp::message_create_t> command_queue_;
  bool stopping_{false};
  std::thread worker_;
};

} // namespace sanguinius
