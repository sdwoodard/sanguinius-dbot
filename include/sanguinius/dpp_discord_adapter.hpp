#pragma once

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace sanguinius {

class DppDiscordAdapter final : public DiscordRuntime {
public:
  DppDiscordAdapter(std::string token, std::chrono::seconds request_timeout,
                    Diagnostics &diagnostics);
  ~DppDiscordAdapter() override;

  DppDiscordAdapter(const DppDiscordAdapter &) = delete;
  DppDiscordAdapter &operator=(const DppDiscordAdapter &) = delete;
  DppDiscordAdapter(DppDiscordAdapter &&) = delete;
  DppDiscordAdapter &operator=(DppDiscordAdapter &&) = delete;

  void start(MessageCallback message_callback) override;
  void stop_accepting() noexcept override;
  void shutdown() noexcept override;

  void show_typing(DiscordId channel_id) override;
  void reply(const ReplyRequest &request) override;

  [[nodiscard]] std::vector<ConversationEntry>
  recent_messages(const IncomingMessage &message, std::size_t limit,
                  std::stop_token stop_token) override;
  [[nodiscard]] ConversationEntry message(DiscordId message_id,
                                          DiscordId channel_id,
                                          std::stop_token stop_token) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
