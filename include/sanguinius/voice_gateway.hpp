#pragma once

#include "sanguinius/snowflake.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

enum class VoiceResolveStatus {
  ready,
  no_voice,
  unsupported_channel,
  permission_denied,
  channel_full,
  unavailable,
};

struct VoiceResolvedChannel {
  VoiceResolveStatus status{VoiceResolveStatus::unavailable};
  DiscordSnowflake channel_id;
  std::size_t human_count{};
  std::string failure_category;
};

struct VoiceConnectRequest {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake member_user_id;
  std::uint64_t generation{1};
  bool validate_member_channel{true};
};

enum class VoiceEventKind {
  ready,
  disconnected,
  bot_moved,
  occupancy_changed,
  track_marker,
  error,
};

struct VoiceEvent {
  VoiceEventKind kind{VoiceEventKind::error};
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  std::uint64_t generation{};
  std::size_t human_count{};
  bool dave_active{};
  std::string marker;
  std::string failure_category;
};

struct VoiceGatewaySnapshot {
  bool bound{};
  bool connected{};
  bool ready{};
  bool dave_active{};
  bool marker_completed{};
  bool bot_moved{};
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake observed_channel_id;
  std::uint64_t generation{};
  std::size_t human_count{};
};

enum class VoiceGatewaySubmit {
  accepted,
  unavailable,
  invalid,
};

struct PcmAudio {
  std::uint32_t sample_rate{48'000};
  std::uint16_t channels{2};
  std::uint16_t bits_per_sample{16};
  std::vector<std::int16_t> samples;
};

class VoiceGateway {
public:
  using ResolveCallback = std::function<void(VoiceResolvedChannel)>;
  using EventCallback = std::function<void(VoiceEvent)>;

  virtual ~VoiceGateway() = default;
  virtual void start(EventCallback callback) = 0;
  virtual void resolve_member_channel(DiscordSnowflake guild_id,
                                      DiscordSnowflake user_id,
                                      ResolveCallback callback) = 0;
  [[nodiscard]] virtual VoiceGatewaySubmit
  connect(const VoiceConnectRequest &request) = 0;
  [[nodiscard]] virtual VoiceGatewaySubmit
  disconnect(std::string_view session_id) = 0;
  virtual void release_binding(std::string_view session_id) noexcept = 0;
  [[nodiscard]] virtual VoiceGatewaySubmit
  stop_audio(std::string_view session_id) = 0;
  [[nodiscard]] virtual VoiceGatewaySubmit
  send_pcm(std::string_view session_id, const PcmAudio &audio,
           std::string_view marker) = 0;
  [[nodiscard]] virtual VoiceGatewaySnapshot
  snapshot(std::string_view session_id) const noexcept = 0;
  virtual void shutdown() noexcept = 0;
};

[[nodiscard]] const char *
voice_resolve_status_name(VoiceResolveStatus status) noexcept;

} // namespace sanguinius
