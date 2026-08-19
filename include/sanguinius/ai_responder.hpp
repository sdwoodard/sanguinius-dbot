#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/prompt_compiler.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

[[nodiscard]] std::optional<std::string>
prompt_after_bot_mention(std::string_view content, DiscordId bot_id);

class AiResponder {
public:
  AiResponder(const AiClient &client, DiscordConversation &conversation,
              DiscordTextDelivery &delivery, Diagnostics &diagnostics,
              std::string persona, std::size_t queue_capacity = 64,
              std::size_t worker_count = 2,
              RelationshipService *relationships = nullptr,
              FeatureConfiguration features = {});
  ~AiResponder();

  AiResponder(const AiResponder &) = delete;
  AiResponder &operator=(const AiResponder &) = delete;
  AiResponder(AiResponder &&) = delete;
  AiResponder &operator=(AiResponder &&) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] bool handles(const IncomingMessage &message) const;
  [[nodiscard]] SubmitResult enqueue(IncomingMessage message);
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void respond(const IncomingMessage &message,
               std::stop_token stop_token) const;
  [[nodiscard]] std::optional<ConversationEntry>
  replied_message(const IncomingMessage &message,
                  const std::vector<ConversationEntry> &recent,
                  std::stop_token stop_token) const;
  const AiClient &client_;
  DiscordConversation &conversation_;
  DiscordTextDelivery &delivery_;
  Diagnostics &diagnostics_;
  PromptCompiler compiler_;
  RelationshipService *relationships_{};
  FeatureConfiguration features_;
  BoundedExecutor workers_;
};

} // namespace sanguinius
