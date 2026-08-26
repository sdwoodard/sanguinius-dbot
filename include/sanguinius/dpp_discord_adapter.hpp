#pragma once

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_command_cli.hpp"
#include "sanguinius/discord_interfaces.hpp"

#include <dpp/message.h>
#include <dpp/appcommand.h>

#include <chrono>
#include <memory>
#include <ostream>
#include <string>

namespace sanguinius {

class DppClusterHost;

namespace dpp_adapter_detail {

[[nodiscard]] std::string
durable_public_message_json(const PublicMessageRequest &request,
                            std::string_view provider_nonce);
[[nodiscard]] std::string_view
durable_public_message_base_path() noexcept;
[[nodiscard]] DeliveryResult
classify_http_delivery(bool succeeded, int http_status,
                       bool transport_failed) noexcept;
[[nodiscard]] DiscordId provider_message_id(const dpp::message &message);
[[nodiscard]] ContextMessageSnapshot
context_message_snapshot(const dpp::message &message);
[[nodiscard]] std::vector<dpp::slashcommand>
translate_command_catalog(const CommandCatalog &catalog,
                          dpp::snowflake application_id);
void translate_slash_command(const dpp::command_interaction &command,
                             IncomingInteraction &interaction);

} // namespace dpp_adapter_detail

class DppDiscordAdapter final : public DiscordRuntime {
public:
  DppDiscordAdapter(std::string token, std::chrono::seconds request_timeout,
                    DiscordId guild_id, Diagnostics &diagnostics,
                    bool voice_enabled = false);
  DppDiscordAdapter(std::shared_ptr<DppClusterHost> cluster_host,
                    std::chrono::seconds request_timeout, DiscordId guild_id,
                    Diagnostics &diagnostics);
  ~DppDiscordAdapter() override;

  DppDiscordAdapter(const DppDiscordAdapter &) = delete;
  DppDiscordAdapter &operator=(const DppDiscordAdapter &) = delete;
  DppDiscordAdapter(DppDiscordAdapter &&) = delete;
  DppDiscordAdapter &operator=(DppDiscordAdapter &&) = delete;

  void start(MessageCallback message_callback,
             InteractionCallback interaction_callback,
             CommandCatalog command_catalog) override;
  void stop_accepting() noexcept override;
  void shutdown() noexcept override;

  void show_typing(DiscordId channel_id) override;
  void reply(const ReplyRequest &request) override;
  void send_public(const PublicMessageRequest &request,
                   std::string_view provider_nonce,
                   PublicDeliveryCallback callback = {}) override;
  void edit_public(const PublicMessageEditRequest &request,
                   DeliveryCallback callback = {}) override;
  [[nodiscard]] DiscordRuntimeStatus status() const noexcept override;

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

int run_discord_command_operator(DiscordCommandOperation operation,
                                 std::string token,
                                 std::chrono::seconds request_timeout,
                                 DiscordId guild_id,
                                 const CommandCatalog &catalog,
                                 std::ostream &output, std::ostream &errors);

} // namespace sanguinius
