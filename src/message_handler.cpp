#include "sanguinius/message_handler.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::string_view repository_url{
    "https://github.com/sdwoodard/sanguinius-dbot"};

[[nodiscard]] std::string lowercase(std::string_view value) {
  std::string result{value};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

} // namespace

MessageHandler::MessageHandler(MessageLogger &logger,
                               std::string command_prefix)
    : logger_{logger}, command_prefix_{std::move(command_prefix)},
      worker_{&MessageHandler::run, this} {}

MessageHandler::~MessageHandler() {
  {
    const std::scoped_lock lock{queue_mutex_};
    stopping_ = true;
  }
  queue_ready_.notify_one();
  worker_.join();
}

Command parse_command(const std::string_view content,
                      const std::string_view prefix) {
  if (!content.starts_with(prefix)) {
    return Command::none;
  }

  const auto start = prefix.size();
  const auto end = content.find_first_of(" \t\r\n", start);
  const auto name = lowercase(
      content.substr(start, end == std::string_view::npos ? end : end - start));

  if (name == "help") {
    return Command::help;
  }
  if (name == "repo") {
    return Command::repo;
  }
  return Command::none;
}

void MessageHandler::operator()(const dpp::message_create_t &event) noexcept {
  try {
    logger_.log(event.msg);
  } catch (const std::exception &error) {
    std::cerr << "Message logging failed: " << error.what() << '\n';
  }

  if (event.msg.author.is_bot() ||
      parse_command(event.msg.content, command_prefix_) == Command::none) {
    return;
  }

  try {
    {
      const std::scoped_lock lock{queue_mutex_};
      if (stopping_) {
        return;
      }
      command_queue_.push(event);
    }
    queue_ready_.notify_one();
  } catch (const std::exception &error) {
    std::cerr << "Unable to queue command: " << error.what() << '\n';
  }
}

void MessageHandler::run() {
  while (true) {
    dpp::message_create_t event;
    {
      std::unique_lock lock{queue_mutex_};
      queue_ready_.wait(
          lock, [this] { return stopping_ || !command_queue_.empty(); });

      if (command_queue_.empty()) {
        if (stopping_) {
          return;
        }
        continue;
      }

      event = std::move(command_queue_.front());
      command_queue_.pop();
    }

    try {
      dispatch_command(event);
    } catch (const std::exception &error) {
      std::cerr << "Command handling failed: " << error.what() << '\n';
    }
  }
}

void MessageHandler::dispatch_command(
    const dpp::message_create_t &event) const {
  const auto command = parse_command(event.msg.content, command_prefix_);
  if (command == Command::help) {
    send_help(event);
  } else if (command == Command::repo) {
    send_repo(event);
  }
}

void MessageHandler::send_help(const dpp::message_create_t &event) const {
  event.reply("Sanguinius supports two commands:\n"
              "`" +
              command_prefix_ +
              "help` — show this message\n"
              "`" +
              command_prefix_ + "repo` — show the source repository");
}

void MessageHandler::send_repo(const dpp::message_create_t &event) const {
  dpp::embed embed;
  embed.set_color(dpp::colors::sti_blue)
      .set_title("Sanguinius source code")
      .set_url(std::string{repository_url})
      .set_description(
          "Build instructions and source code for the Sanguinius Discord bot.");

  event.reply(dpp::message{}.add_embed(embed));
}

} // namespace sanguinius
