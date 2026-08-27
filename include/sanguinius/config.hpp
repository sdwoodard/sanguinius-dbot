#pragma once

#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/build_info.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/tarot_catalog.hpp"
#include "sanguinius/tarot_house.hpp"
#include "sanguinius/wagers.hpp"
#include "sanguinius/tts.hpp"
#include "sanguinius/tts_cache.hpp"
#include "sanguinius/voice_input.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace sanguinius {

class ConfigSource {
public:
  virtual ~ConfigSource() = default;

  [[nodiscard]] virtual std::optional<std::string>
  environment(std::string_view name) const = 0;
  [[nodiscard]] virtual std::string
  read_file(const std::filesystem::path &path) const = 0;
};

struct DiscordConfiguration {
  std::string token;
  ServerScopeConfiguration server_scope;
  std::chrono::seconds request_timeout{10};
};

struct DiscordCommandConfiguration {
  std::string token;
  DiscordSnowflake guild_id;
  std::chrono::seconds request_timeout{10};
  bool admin_commands_enabled{};
  bool chronicle_enabled{};
  bool tarot_enabled{};
  bool vox_enabled{};
};

struct AiConfiguration {
  std::string api_key;
  std::string model{"gpt-5.6-luna"};
  std::filesystem::path persona_file{"config/persona.txt"};
  std::string persona;
};

enum class TtsProvider {
  disabled,
  openai,
};

struct TtsConfiguration {
  TtsProvider provider{TtsProvider::disabled};
  std::string model{"tts-1"};
  std::string voice{"onyx"};
  std::filesystem::path cache_directory{"/var/cache/sanguinius/tts"};
  std::filesystem::path ffmpeg_path{"/usr/bin/ffmpeg"};
  std::filesystem::path ffprobe_path{"/usr/bin/ffprobe"};
  std::filesystem::path fallback_directory{
      "/usr/local/share/sanguinius/vox"};
  std::chrono::milliseconds connect_timeout{5'000};
  std::chrono::milliseconds request_timeout{30'000};
  AudioNormalizationLimits normalization_limits;
  TtsCachePolicy cache_policy;
  TtsUsagePolicy usage_policy;
  std::size_t maximum_attempts{2};
  std::size_t maximum_text_scalars{maximum_tts_scalar_count};
};

enum class TranscriptionProvider {
  disabled,
  openai,
};

struct PathConfiguration {
  std::filesystem::path message_log{"logs/messages.log"};
  std::filesystem::path database_file{"state/sanguinius.sqlite3"};
  std::filesystem::path appearance_policy_file{
      "config/appearance-policy-v2.json"};
  std::filesystem::path tarot_deck_file{"config/emperor-tarot-v1.json"};
  std::filesystem::path tarot_house_file{"config/tarot-house-v1.json"};
};

enum class ConfigurationOrigin {
  default_value,
  configured,
};

struct ConfigurationOrigins {
  ConfigurationOrigin discord_request_timeout{
      ConfigurationOrigin::default_value};
  ConfigurationOrigin message_log{ConfigurationOrigin::default_value};
  ConfigurationOrigin database_file{ConfigurationOrigin::default_value};
  ConfigurationOrigin command_prefix{ConfigurationOrigin::default_value};
  ConfigurationOrigin openai_model{ConfigurationOrigin::default_value};
  ConfigurationOrigin persona_file{ConfigurationOrigin::default_value};
  ConfigurationOrigin timezone{ConfigurationOrigin::default_value};
  ConfigurationOrigin appearance_policy_file{
      ConfigurationOrigin::default_value};
  ConfigurationOrigin tarot_deck_file{ConfigurationOrigin::default_value};
  ConfigurationOrigin tarot_house_file{ConfigurationOrigin::default_value};
};

struct Config {
  DiscordConfiguration discord;
  AiConfiguration ai;
  TtsConfiguration tts;
  TranscriptionProvider transcription_provider{TranscriptionProvider::disabled};
  VoiceListeningConfiguration voice_input;
  PathConfiguration paths;
  ControlConfiguration controls;
  FeatureConfiguration features;
  ConfigurationOrigins origins;
  AppearancePolicy appearance_policy;
  TarotPolicy tarot_policy;
  WagerPolicy wager_policy;
  TarotHousePolicy tarot_house_policy;
  std::optional<TarotDeckCatalog> tarot_deck_catalog;
  std::optional<TarotHouseCatalog> tarot_house_catalog;
  std::string command_prefix{"!"};
  std::string timezone{"America/New_York"};

  [[nodiscard]] static Config from_environment();
  [[nodiscard]] static Config from_source(const ConfigSource &source);
};

[[nodiscard]] std::string_view
configuration_origin_name(ConfigurationOrigin origin) noexcept;

[[nodiscard]] std::string_view tts_provider_name(TtsProvider provider) noexcept;
[[nodiscard]] std::string_view
transcription_provider_name(TranscriptionProvider provider) noexcept;

[[nodiscard]] std::string redacted_config_summary(const Config &config,
                                                  const BuildInfo &build);

[[nodiscard]] std::filesystem::path database_file_from_environment();

[[nodiscard]] DiscordCommandConfiguration
discord_command_configuration_from_environment();
[[nodiscard]] DiscordCommandConfiguration
discord_command_configuration_from_source(const ConfigSource &source);

} // namespace sanguinius
