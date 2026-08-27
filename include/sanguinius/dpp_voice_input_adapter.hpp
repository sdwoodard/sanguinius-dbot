#pragma once

#include "sanguinius/voice_input.hpp"

#include <memory>

namespace sanguinius {

class DppClusterHost;

namespace dpp_voice_input_adapter_detail {

enum class ReceiveClientDisposition {
  accept,
  ignore,
  connection_changed,
};

[[nodiscard]] ReceiveClientDisposition
classify_receive_client(const void *expected_client,
                        const void *received_client,
                        DiscordSnowflake expected_guild_id,
                        DiscordSnowflake received_guild_id) noexcept;

} // namespace dpp_voice_input_adapter_detail

class DppVoiceInputAdapter final : public VoiceInputAdapter {
public:
  DppVoiceInputAdapter(std::shared_ptr<DppClusterHost> cluster_host,
                       VoiceGateway &gateway, Diagnostics &diagnostics,
                       bool configured_enabled);
  ~DppVoiceInputAdapter() override;

  DppVoiceInputAdapter(const DppVoiceInputAdapter &) = delete;
  DppVoiceInputAdapter &operator=(const DppVoiceInputAdapter &) = delete;

  [[nodiscard]] VoiceInputCapability capability() const noexcept override;
  void start(AudioCallback audio_callback,
             EventCallback event_callback) override;
  [[nodiscard]] VoiceInputPresence
  preflight(const VoiceInputArmRequest &request) const override;
  [[nodiscard]] bool enable_transport(const VoiceInputArmRequest &request,
                                      std::stop_token stop_token) override;
  [[nodiscard]] bool arm(const VoiceInputArmRequest &request) override;
  void disarm() noexcept override;
  [[nodiscard]] bool disable_transport() noexcept override;
  void shutdown() noexcept override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
