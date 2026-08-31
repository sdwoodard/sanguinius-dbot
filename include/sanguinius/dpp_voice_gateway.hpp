#pragma once

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/voice_gateway.hpp"

#include <memory>

namespace sanguinius {

class DppClusterHost;

namespace dpp_voice_gateway_detail {

[[nodiscard]] VoiceEvent translate_ready(const VoiceGatewaySnapshot &binding,
                                         DiscordSnowflake observed_channel_id,
                                         bool client_ready, bool dave_active);
[[nodiscard]] bool defer_until_dave(bool client_ready,
                                    bool dave_active) noexcept;
[[nodiscard]] bool may_replace_binding(const VoiceGatewaySnapshot &binding,
                                       std::string_view session_id) noexcept;
[[nodiscard]] bool
matches_voice_session(std::string_view expected_session_id,
                      std::string_view observed_session_id) noexcept;
[[nodiscard]] bool
matches_voice_client(const void *current_voice_client,
                     const void *observed_voice_client) noexcept;
[[nodiscard]] std::vector<std::size_t>
pcm_chunk_sizes(std::size_t byte_count,
                std::size_t maximum_chunk_bytes = 11'520);
[[nodiscard]] std::string
voice_state_update_payload(DiscordSnowflake guild_id,
                           DiscordSnowflake channel_id, bool self_deaf);

} // namespace dpp_voice_gateway_detail

class DppVoiceGateway final : public VoiceGateway {
public:
  DppVoiceGateway(std::shared_ptr<DppClusterHost> cluster_host,
                  Diagnostics &diagnostics);
  ~DppVoiceGateway() override;

  DppVoiceGateway(const DppVoiceGateway &) = delete;
  DppVoiceGateway &operator=(const DppVoiceGateway &) = delete;

  void start(EventCallback callback) override;
  void resolve_member_channel(DiscordSnowflake guild_id,
                              DiscordSnowflake user_id,
                              ResolveCallback callback) override;
  [[nodiscard]] VoiceGatewaySubmit
  connect(const VoiceConnectRequest &request) override;
  [[nodiscard]] VoiceGatewaySubmit
  disconnect(std::string_view session_id) override;
  void release_binding(std::string_view session_id) noexcept override;
  [[nodiscard]] VoiceGatewaySubmit
  stop_audio(std::string_view session_id) override;
  [[nodiscard]] VoiceGatewaySubmit send_pcm(std::string_view session_id,
                                            const PcmAudio &audio,
                                            std::string_view marker) override;
  [[nodiscard]] VoiceGatewaySubmit
  set_receive_enabled(std::string_view session_id, bool enabled) override;
  [[nodiscard]] VoiceGatewaySnapshot
  snapshot(std::string_view session_id) const noexcept override;
  void shutdown() noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
