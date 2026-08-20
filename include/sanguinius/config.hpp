#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/tarot.hpp"

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
};

struct AiConfiguration {
  std::string api_key;
  std::string model{"gpt-5.4-nano"};
  std::filesystem::path persona_file{"config/persona.txt"};
  std::string persona;
};

struct PathConfiguration {
  std::filesystem::path message_log{"logs/messages.log"};
  std::filesystem::path database_file{"state/sanguinius.sqlite3"};
  std::filesystem::path appearance_policy_file{"config/appearance-policy-v1.json"};
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
  ConfigurationOrigin appearance_policy_file{ConfigurationOrigin::default_value};
};

struct Config {
  DiscordConfiguration discord;
  AiConfiguration ai;
  PathConfiguration paths;
  ControlConfiguration controls;
  FeatureConfiguration features;
  ConfigurationOrigins origins;
  AppearancePolicy appearance_policy;
  TarotPolicy tarot_policy;
  std::string command_prefix{"!"};
  std::string timezone{"America/New_York"};

  [[nodiscard]] static Config from_environment();
  [[nodiscard]] static Config from_source(const ConfigSource &source);
};

[[nodiscard]] std::string_view
configuration_origin_name(ConfigurationOrigin origin) noexcept;

[[nodiscard]] std::string redacted_config_summary(const Config &config,
                                                  const BuildInfo &build);

[[nodiscard]] std::filesystem::path database_file_from_environment();

[[nodiscard]] DiscordCommandConfiguration
discord_command_configuration_from_environment();
[[nodiscard]] DiscordCommandConfiguration
discord_command_configuration_from_source(const ConfigSource &source);

} // namespace sanguinius
