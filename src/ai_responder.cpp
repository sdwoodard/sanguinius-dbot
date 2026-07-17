#include "sanguinius/ai_responder.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::size_t maximum_queue_size = 64;
constexpr std::size_t history_size = 8;
constexpr std::size_t maximum_context_message_size = 1'200;
constexpr std::size_t maximum_discord_reply_size = 1'900;
constexpr auto discord_request_timeout = std::chrono::seconds{10};

[[nodiscard]] std::string display_name(const dpp::message &message) {
  return message.author.global_name.empty() ? message.author.username
                                            : message.author.global_name;
}

[[nodiscard]] std::string limited(std::string_view text,
                                  const std::size_t maximum_size) {
  if (text.size() <= maximum_size) {
    return std::string{text};
  }

  std::size_t end = maximum_size;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0b1100'0000U) ==
                        0b1000'0000U) {
    --end;
  }
  return std::string{text.substr(0, end)} + "…";
}

[[nodiscard]] std::string context_line(const dpp::message &message) {
  return display_name(message) + ": " +
         limited(message.content, maximum_context_message_size);
}

} // namespace

std::optional<std::string>
prompt_after_bot_mention(std::string_view content,
                         const dpp::snowflake bot_id) {
  const auto first = content.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  content.remove_prefix(first);

  const std::string mention = "<@" + bot_id.str() + '>';
  const std::string nickname_mention = "<@!" + bot_id.str() + '>';
  std::size_t mention_size = 0;
  if (content.starts_with(mention)) {
    mention_size = mention.size();
  } else if (content.starts_with(nickname_mention)) {
    mention_size = nickname_mention.size();
  } else {
    return std::nullopt;
  }

  content.remove_prefix(mention_size);
  const auto prompt_start = content.find_first_not_of(" \t\r\n,:-");
  if (prompt_start == std::string_view::npos) {
    return std::string{};
  }
  return std::string{content.substr(prompt_start)};
}

AiResponder::AiResponder(dpp::cluster &bot, const OpenAiClient &client,
                         std::string persona, const std::size_t worker_count)
    : bot_{bot}, client_{client}, persona_{std::move(persona)} {
  if (worker_count == 0) {
    throw std::invalid_argument{"AI worker count must be at least one."};
  }
  workers_.reserve(worker_count);
  try {
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back(&AiResponder::run, this);
    }
  } catch (...) {
    {
      const std::scoped_lock lock{queue_mutex_};
      stopping_ = true;
    }
    queue_ready_.notify_all();
    for (auto &worker : workers_) {
      worker.join();
    }
    throw;
  }
}

AiResponder::~AiResponder() {
  {
    const std::scoped_lock lock{queue_mutex_};
    stopping_ = true;
  }
  queue_ready_.notify_all();
  for (auto &worker : workers_) {
    worker.join();
  }
}

bool AiResponder::handles(const dpp::message &message) const {
  return !message.author.is_bot() && bot_.me.id != 0 &&
         prompt_after_bot_mention(message.content, bot_.me.id).has_value();
}

bool AiResponder::enqueue(const dpp::message &message) {
  const std::scoped_lock lock{queue_mutex_};
  if (stopping_ || queue_.size() >= maximum_queue_size) {
    return false;
  }
  queue_.push(message);
  queue_ready_.notify_one();
  return true;
}

void AiResponder::run() {
  while (true) {
    dpp::message message;
    {
      std::unique_lock lock{queue_mutex_};
      queue_ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;
      }
      message = std::move(queue_.front());
      queue_.pop();
    }

    try {
      respond(message);
    } catch (const std::exception &error) {
      std::cerr << "AI response failed: " << error.what() << '\n';
      dpp::message failure{message.channel_id,
                           "I could not form a response just now. Please try "
                           "again shortly."};
      failure.set_reference(message.id, message.guild_id, message.channel_id)
          .set_allowed_mentions(false, false, false, false);
      bot_.message_create(failure);
    }
  }
}

void AiResponder::respond(const dpp::message &message) const {
  bot_.channel_typing(message.channel_id);
  std::vector<dpp::message> recent;
  try {
    recent = recent_messages(message);
  } catch (const std::exception &error) {
    std::cerr << "Discord history context unavailable: " << error.what()
              << '\n';
  }

  std::optional<dpp::message> replied;
  try {
    replied = replied_message(message, recent);
  } catch (const std::exception &error) {
    std::cerr << "Discord reply context unavailable: " << error.what() << '\n';
  }

  const auto prompt = create_prompt(message, recent, replied);
  const std::vector<ConversationMessage> conversation{{"user", prompt}};
  auto response = client_.generate(persona_, conversation);
  response = limited(response, maximum_discord_reply_size);

  dpp::message reply{message.channel_id, response};
  reply.set_reference(message.id, message.guild_id, message.channel_id)
      .set_allowed_mentions(false, false, false, false);
  bot_.message_create(reply);
}

std::vector<dpp::message>
AiResponder::recent_messages(const dpp::message &message) const {
  auto result = std::make_shared<std::promise<dpp::message_map>>();
  auto future = result->get_future();
  bot_.messages_get(message.channel_id, 0, message.id, 0, history_size,
                    [result](const dpp::confirmation_callback_t &confirmation) {
                      try {
                        if (confirmation.is_error()) {
                          throw std::runtime_error{
                              confirmation.get_error().human_readable};
                        }
                        result->set_value(confirmation.get<dpp::message_map>());
                      } catch (...) {
                        result->set_exception(std::current_exception());
                      }
                    });

  if (future.wait_for(discord_request_timeout) != std::future_status::ready) {
    throw std::runtime_error{"Timed out retrieving recent Discord messages."};
  }

  auto message_map = future.get();
  std::vector<dpp::message> messages;
  messages.reserve(message_map.size());
  for (auto &[id, recent] : message_map) {
    static_cast<void>(id);
    if (!recent.content.empty()) {
      messages.push_back(std::move(recent));
    }
  }
  std::ranges::sort(messages, {}, &dpp::message::id);
  return messages;
}

std::optional<dpp::message>
AiResponder::replied_message(const dpp::message &message,
                             const std::vector<dpp::message> &recent) const {
  const auto reply_id = message.message_reference.message_id;
  if (reply_id == 0) {
    return std::nullopt;
  }

  if (const auto found = std::ranges::find(recent, reply_id, &dpp::message::id);
      found != recent.end()) {
    return *found;
  }

  auto result = std::make_shared<std::promise<dpp::message>>();
  auto future = result->get_future();
  const auto channel_id = message.message_reference.channel_id == 0
                              ? message.channel_id
                              : message.message_reference.channel_id;
  bot_.message_get(reply_id, channel_id,
                   [result](const dpp::confirmation_callback_t &confirmation) {
                     try {
                       if (confirmation.is_error()) {
                         throw std::runtime_error{
                             confirmation.get_error().human_readable};
                       }
                       result->set_value(confirmation.get<dpp::message>());
                     } catch (...) {
                       result->set_exception(std::current_exception());
                     }
                   });

  if (future.wait_for(discord_request_timeout) != std::future_status::ready) {
    throw std::runtime_error{"Timed out retrieving the replied-to message."};
  }
  return future.get();
}

std::string
AiResponder::create_prompt(const dpp::message &message,
                           const std::vector<dpp::message> &recent,
                           const std::optional<dpp::message> &replied) const {
  std::string prompt{
      "Discord conversation context follows. It may be unrelated; use only "
      "what is relevant.\n"};
  if (!recent.empty()) {
    prompt += "\nRecent messages (oldest first):\n";
    for (const auto &item : recent) {
      prompt += context_line(item) + '\n';
    }
  }
  if (replied.has_value()) {
    prompt += "\nMessage explicitly being replied to:\n" +
              context_line(*replied) + '\n';
  }

  const auto user_prompt =
      prompt_after_bot_mention(message.content, bot_.me.id).value_or("");
  prompt += "\nLatest request from " + display_name(message) + ":\n" +
            (user_prompt.empty() ? "Please respond to the conversation."
                                 : user_prompt);
  return prompt;
}

} // namespace sanguinius
