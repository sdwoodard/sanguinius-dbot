#include "sanguinius/message_handler.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::string_view repository_url{
    "https://github.com/sdwoodard/sanguinius-dbot"};
constexpr std::uint32_t repository_embed_color = 0x0E4BEFU;
constexpr std::string_view overload_message{
    "I am handling too many requests right now. Please try again shortly."};

[[nodiscard]] std::string lowercase(const std::string_view value) {
  std::string result{value};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

[[nodiscard]] MessageReference target(const IncomingMessage &message) {
  return MessageReference{message.message_id, message.guild_id,
                          message.channel_id};
}

} // namespace

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

MessageHandler::MessageHandler(MessageLog &message_log,
                               AiResponder &ai_responder,
                               DiscordTextDelivery &delivery,
                               Diagnostics &diagnostics,
                               std::string command_prefix,
                               const std::size_t queue_capacity)
    : message_log_{message_log}, ai_responder_{ai_responder},
      delivery_{delivery}, diagnostics_{diagnostics},
      command_prefix_{std::move(command_prefix)}, worker_{queue_capacity, 1} {}

MessageHandler::~MessageHandler() { stop(); }

void MessageHandler::start() { worker_.start(); }

void MessageHandler::stop() noexcept { worker_.stop(); }

SubmitResult MessageHandler::enqueue(IncomingMessage message) {
  const bool should_notify = actionable(message);
  const auto result =
      worker_.try_submit([this, message](const std::stop_token stop_token) {
        if (stop_token.stop_requested()) {
          return;
        }
        try {
          process(message);
        } catch (const std::exception &error) {
          diagnostics_.emit({DiagnosticSeverity::error, "message.handling",
                             error.what(), message.correlation_id});
        }
      });

  if (result == SubmitResult::full) {
    diagnostics_.emit({DiagnosticSeverity::warning, "message.queue_full",
                       "Incoming message was rejected because the application "
                       "queue is full.",
                       message.correlation_id});
    if (should_notify) {
      send_overload(message);
    }
  }
  return result;
}

void MessageHandler::process(const IncomingMessage &message) const {
  try {
    message_log_.append({message.author_username, message.content});
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "message.logging",
                       error.what(), message.correlation_id});
  }

  if (message.author_is_bot) {
    return;
  }

  if (ai_responder_.handles(message)) {
    if (ai_responder_.enqueue(message) == SubmitResult::full) {
      send_overload(message);
    }
    return;
  }

  const auto command = parse_command(message.content, command_prefix_);
  if (command == Command::help) {
    send_help(message);
  } else if (command == Command::repo) {
    send_repo(message);
  }
}

void MessageHandler::send_help(const IncomingMessage &message) const {
  delivery_.reply({
      .target = target(message),
      .content =
          "Mention me at the beginning of a message to ask me something.\n"
          "Sanguinius also supports two commands:\n`" +
          command_prefix_ + "help` — show this message\n`" + command_prefix_ +
          "repo` — show the source repository",
      .embed = std::nullopt,
      .suppress_mentions = true,
  });
}

void MessageHandler::send_repo(const IncomingMessage &message) const {
  delivery_.reply({
      .target = target(message),
      .content = {},
      .embed =
          EmbedPayload{
              .color = repository_embed_color,
              .title = "Sanguinius source code",
              .url = std::string{repository_url},
              .description = "Build instructions and source code for the "
                             "Sanguinius Discord bot.",
          },
      .suppress_mentions = true,
  });
}

void MessageHandler::send_overload(
    const IncomingMessage &message) const noexcept {
  try {
    delivery_.reply({
        .target = target(message),
        .content = std::string{overload_message},
        .embed = std::nullopt,
        .suppress_mentions = true,
    });
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "discord.delivery",
                       error.what(), message.correlation_id});
  }
}

bool MessageHandler::actionable(const IncomingMessage &message) const {
  return !message.author_is_bot &&
         (ai_responder_.handles(message) ||
          parse_command(message.content, command_prefix_) != Command::none);
}

} // namespace sanguinius
