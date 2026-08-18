#include "sanguinius/dpp_discord_adapter.hpp"

#include "sanguinius/ai_client.hpp"

#include <dpp/dpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr auto cancellation_poll_interval = std::chrono::milliseconds{25};

constexpr std::uint32_t gateway_intents =
    dpp::i_guilds | dpp::i_guild_messages | dpp::i_message_content;
static_assert((gateway_intents & dpp::i_direct_messages) == 0U);
static_assert((gateway_intents & dpp::i_guild_voice_states) == 0U);

[[nodiscard]] DiscordId id(const dpp::snowflake value) {
  return DiscordId{static_cast<std::uint64_t>(value)};
}

[[nodiscard]] ConversationEntry
conversation_entry(const dpp::message &message) {
  return ConversationEntry{
      .message_id = id(message.id),
      .author_username = message.author.username,
      .author_display_name = message.author.global_name,
      .content = message.content,
  };
}

[[nodiscard]] IncomingMessage incoming_message(const dpp::message &message,
                                               const DiscordId bot_user_id) {
  IncomingMessage incoming{
      .correlation_id = {},
      .bot_user_id = bot_user_id,
      .message_id = id(message.id),
      .guild_id = id(message.guild_id),
      .channel_id = id(message.channel_id),
      .author_user_id = id(message.author.id),
      .author_username = message.author.username,
      .author_display_name = message.author.global_name,
      .content = message.content,
      .author_is_bot = message.author.is_bot(),
      .replied_to = std::nullopt,
  };

  if (message.message_reference.message_id != 0) {
    incoming.replied_to = MessageReference{
        .message_id = id(message.message_reference.message_id),
        .guild_id = id(message.message_reference.guild_id),
        .channel_id = id(message.message_reference.channel_id),
    };
  }
  return incoming;
}

[[nodiscard]] DiagnosticSeverity severity(const dpp::loglevel level) {
  switch (level) {
  case dpp::ll_trace:
  case dpp::ll_debug:
    return DiagnosticSeverity::debug;
  case dpp::ll_info:
    return DiagnosticSeverity::info;
  case dpp::ll_warning:
    return DiagnosticSeverity::warning;
  case dpp::ll_error:
  case dpp::ll_critical:
    return DiagnosticSeverity::error;
  }
  return DiagnosticSeverity::info;
}

template <typename Value>
Value wait_for_discord(std::future<Value> &future,
                       const std::stop_token stop_token,
                       const std::chrono::seconds request_timeout,
                       const std::string_view timeout_message) {
  const auto deadline = std::chrono::steady_clock::now() + request_timeout;
  while (future.wait_for(cancellation_poll_interval) !=
         std::future_status::ready) {
    if (stop_token.stop_requested()) {
      throw OperationCancelled{};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error{std::string{timeout_message}};
    }
  }
  if (stop_token.stop_requested()) {
    throw OperationCancelled{};
  }
  return future.get();
}

} // namespace

class DppDiscordAdapter::Impl {
public:
  Impl(std::string token, const std::chrono::seconds request_timeout,
       Diagnostics &diagnostics)
      : bot_{std::move(token), gateway_intents}, diagnostics_{diagnostics},
        request_timeout_{request_timeout} {}

  void start(MessageCallback message_callback) {
    if (started_.exchange(true)) {
      throw std::logic_error{"Discord gateway may only be started once."};
    }

    message_callback_ = std::move(message_callback);
    log_handle_ = bot_.on_log([this](const dpp::log_t &event) {
      diagnostics_.emit({severity(event.severity), "dpp", event.message, {}});
    });
    ready_handle_ = bot_.on_ready([this](const dpp::ready_t &) {
      diagnostics_.emit({DiagnosticSeverity::info,
                         "discord.ready",
                         "Sanguinius is connected as " + bot_.me.username + ".",
                         {}});
    });
    accepting_.store(true);
    message_handle_ =
        bot_.on_message_create([this](const dpp::message_create_t &event) {
          if (!accepting_.load()) {
            return;
          }
          try {
            message_callback_(incoming_message(event.msg, id(bot_.me.id)));
          } catch (const std::exception &error) {
            diagnostics_.emit({DiagnosticSeverity::error,
                               "discord.message_translation",
                               error.what(),
                               {}});
          } catch (...) {
            diagnostics_.emit({DiagnosticSeverity::error,
                               "discord.message_translation",
                               "Unknown Discord message translation failure.",
                               {}});
          }
        });

    bot_.start(dpp::st_return);
  }

  void stop_accepting() noexcept {
    accepting_.store(false);
    try {
      if (message_handle_ != 0) {
        static_cast<void>(bot_.on_message_create.detach(message_handle_));
        message_handle_ = 0;
      }
    } catch (...) {
    }
  }

  void shutdown() noexcept {
    if (shutdown_.exchange(true)) {
      return;
    }
    stop_accepting();
    try {
      bot_.shutdown();
    } catch (const std::exception &error) {
      diagnostics_.emit(
          {DiagnosticSeverity::error, "discord.shutdown", error.what(), {}});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "discord.shutdown",
                         "Unknown Discord shutdown failure.",
                         {}});
    }

    try {
      if (ready_handle_ != 0) {
        static_cast<void>(bot_.on_ready.detach(ready_handle_));
        ready_handle_ = 0;
      }
      if (log_handle_ != 0) {
        static_cast<void>(bot_.on_log.detach(log_handle_));
        log_handle_ = 0;
      }
    } catch (...) {
    }
  }

  dpp::cluster bot_;
  Diagnostics &diagnostics_;
  std::chrono::seconds request_timeout_;
  MessageCallback message_callback_;
  std::atomic<bool> started_{false};
  std::atomic<bool> accepting_{false};
  std::atomic<bool> shutdown_{false};
  dpp::event_handle log_handle_{};
  dpp::event_handle ready_handle_{};
  dpp::event_handle message_handle_{};
};

DppDiscordAdapter::DppDiscordAdapter(std::string token,
                                     const std::chrono::seconds request_timeout,
                                     Diagnostics &diagnostics)
    : impl_{std::make_unique<Impl>(std::move(token), request_timeout,
                                   diagnostics)} {}

DppDiscordAdapter::~DppDiscordAdapter() { shutdown(); }

void DppDiscordAdapter::start(MessageCallback message_callback) {
  impl_->start(std::move(message_callback));
}

void DppDiscordAdapter::stop_accepting() noexcept { impl_->stop_accepting(); }

void DppDiscordAdapter::shutdown() noexcept { impl_->shutdown(); }

void DppDiscordAdapter::show_typing(const DiscordId channel_id) {
  impl_->bot_.channel_typing(dpp::snowflake{channel_id.value()});
}

void DppDiscordAdapter::reply(const ReplyRequest &request) {
  dpp::message reply{dpp::snowflake{request.target.channel_id.value()},
                     request.content};
  if (request.embed.has_value()) {
    dpp::embed embed;
    embed.set_color(request.embed->color)
        .set_title(request.embed->title)
        .set_url(request.embed->url)
        .set_description(request.embed->description);
    reply.add_embed(embed);
  }
  reply.set_reference(dpp::snowflake{request.target.message_id.value()},
                      dpp::snowflake{request.target.guild_id.value()},
                      dpp::snowflake{request.target.channel_id.value()});
  if (request.suppress_mentions) {
    reply.set_allowed_mentions(false, false, false, false);
  }
  impl_->bot_.message_create(reply);
}

std::vector<ConversationEntry>
DppDiscordAdapter::recent_messages(const IncomingMessage &message,
                                   const std::size_t limit,
                                   const std::stop_token stop_token) {
  auto result = std::make_shared<std::promise<dpp::message_map>>();
  auto future = result->get_future();
  impl_->bot_.messages_get(
      dpp::snowflake{message.channel_id.value()}, 0,
      dpp::snowflake{message.message_id.value()}, 0, limit,
      [result](const dpp::confirmation_callback_t &confirmation) {
        try {
          if (confirmation.is_error()) {
            throw std::runtime_error{confirmation.get_error().human_readable};
          }
          result->set_value(confirmation.get<dpp::message_map>());
        } catch (...) {
          try {
            result->set_exception(std::current_exception());
          } catch (...) {
          }
        }
      });

  auto message_map =
      wait_for_discord(future, stop_token, impl_->request_timeout_,
                       "Timed out retrieving recent Discord messages.");
  std::vector<ConversationEntry> messages;
  messages.reserve(message_map.size());
  for (auto &[message_id, recent] : message_map) {
    static_cast<void>(message_id);
    if (!recent.content.empty()) {
      messages.push_back(conversation_entry(recent));
    }
  }
  std::ranges::sort(messages, {}, &ConversationEntry::message_id);
  return messages;
}

ConversationEntry DppDiscordAdapter::message(const DiscordId message_id,
                                             const DiscordId channel_id,
                                             const std::stop_token stop_token) {
  auto result = std::make_shared<std::promise<dpp::message>>();
  auto future = result->get_future();
  impl_->bot_.message_get(
      dpp::snowflake{message_id.value()}, dpp::snowflake{channel_id.value()},
      [result](const dpp::confirmation_callback_t &confirmation) {
        try {
          if (confirmation.is_error()) {
            throw std::runtime_error{confirmation.get_error().human_readable};
          }
          result->set_value(confirmation.get<dpp::message>());
        } catch (...) {
          try {
            result->set_exception(std::current_exception());
          } catch (...) {
          }
        }
      });

  const auto fetched =
      wait_for_discord(future, stop_token, impl_->request_timeout_,
                       "Timed out retrieving the replied-to message.");
  return conversation_entry(fetched);
}

} // namespace sanguinius
