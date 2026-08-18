#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sanguinius {

using DiscordId = std::uint64_t;

struct MessageReference {
  DiscordId message_id{};
  DiscordId guild_id{};
  DiscordId channel_id{};
};

struct IncomingMessage {
  std::string correlation_id;
  DiscordId bot_user_id{};
  DiscordId message_id{};
  DiscordId guild_id{};
  DiscordId channel_id{};
  std::string author_username;
  std::string author_display_name;
  std::string content;
  bool author_is_bot{};
  std::optional<MessageReference> replied_to;
};

struct ConversationEntry {
  DiscordId message_id{};
  std::string author_username;
  std::string author_display_name;
  std::string content;
};

struct EmbedPayload {
  std::uint32_t color{};
  std::string title;
  std::string url;
  std::string description;
};

struct ReplyRequest {
  MessageReference target;
  std::string content;
  std::optional<EmbedPayload> embed;
  bool suppress_mentions{true};
};

} // namespace sanguinius
