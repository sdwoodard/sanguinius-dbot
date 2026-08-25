#include "sanguinius/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace sanguinius {
namespace {

class EnvironmentConfigSource final : public ConfigSource {
public:
  std::optional<std::string>
  environment(const std::string_view name) const override {
    const std::string owned_name{name};
    const char *value = std::getenv(owned_name.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string{value};
  }

  std::string read_file(const std::filesystem::path &path) const override {
    std::ifstream stream{path};
    if (!stream) {
      throw std::runtime_error{"Unable to open configured file."};
    }
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
  }
};

[[nodiscard]] bool blank(const std::string_view value) {
  return std::all_of(value.begin(), value.end(),
                     [](const unsigned char character) {
                       return std::isspace(character) != 0;
                     });
}

[[nodiscard]] std::optional<std::string>
optional_nonempty(const ConfigSource &source, const std::string_view variable) {
  const auto value = source.environment(variable);
  if (value.has_value() && value->empty()) {
    throw std::runtime_error{std::string{variable} + " must not be empty."};
  }
  return value;
}

[[nodiscard]] std::string secret_from_file(const ConfigSource &source,
                                           const std::filesystem::path &path,
                                           const std::string_view variable) {
  std::string secret;
  try {
    secret = source.read_file(path);
  } catch (...) {
    throw std::runtime_error{"Unable to read the secret file configured by " +
                             std::string{variable} + "."};
  }
  secret.erase(std::remove_if(secret.begin(), secret.end(),
                              [](const unsigned char character) {
                                return std::isspace(character) != 0;
                              }),
               secret.end());
  if (secret.empty()) {
    throw std::runtime_error{"The secret configured by " +
                             std::string{variable} + " is empty."};
  }
  return secret;
}

[[nodiscard]] std::string text_from_file(const ConfigSource &source,
                                         const std::filesystem::path &path,
                                         const std::string_view variable) {
  try {
    return source.read_file(path);
  } catch (...) {
    throw std::runtime_error{"Unable to read the file configured by " +
                             std::string{variable} + "."};
  }
}

[[nodiscard]] bool optional_boolean(const ConfigSource &source,
                                    const std::string_view variable,
                                    const bool default_value) {
  const auto value = source.environment(variable);
  if (!value.has_value()) {
    return default_value;
  }
  if (*value == "true") {
    return true;
  }
  if (*value == "false") {
    return false;
  }
  throw std::runtime_error{std::string{variable} +
                           " must be exactly true or false."};
}

[[nodiscard]] std::int64_t optional_integer(const ConfigSource &source,
                                            const std::string_view variable,
                                            const std::int64_t default_value,
                                            const std::int64_t minimum,
                                            const std::int64_t maximum) {
  const auto value = source.environment(variable);
  if (!value.has_value())
    return default_value;
  std::int64_t parsed{};
  const auto result =
      std::from_chars(value->data(), value->data() + value->size(), parsed, 10);
  if (value->empty() || result.ec != std::errc{} ||
      result.ptr != value->data() + value->size() || parsed < minimum ||
      parsed > maximum) {
    throw std::runtime_error{
        std::string{variable} + " must be an integer from " +
        std::to_string(minimum) + " through " + std::to_string(maximum) + "."};
  }
  return parsed;
}

[[nodiscard]] DiscordSnowflake
required_snowflake(const ConfigSource &source,
                   const std::string_view variable) {
  const auto value = source.environment(variable);
  if (!value.has_value() || value->empty()) {
    throw std::runtime_error{std::string{variable} + " is required."};
  }
  try {
    const auto parsed = DiscordSnowflake::parse(*value);
    if (!parsed.is_set()) {
      throw std::invalid_argument{"zero"};
    }
    return parsed;
  } catch (...) {
    throw std::runtime_error{std::string{variable} +
                             " must be a nonzero canonical decimal Discord "
                             "snowflake."};
  }
}

[[nodiscard]] std::chrono::seconds
request_timeout(const ConfigSource &source, ConfigurationOrigin &origin) {
  constexpr std::string_view variable{
      "SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS"};
  const auto value = source.environment(variable);
  if (!value.has_value()) {
    return std::chrono::seconds{10};
  }
  origin = ConfigurationOrigin::configured;

  unsigned int parsed{};
  const auto result =
      std::from_chars(value->data(), value->data() + value->size(), parsed, 10);
  if (value->empty() || result.ec != std::errc{} ||
      result.ptr != value->data() + value->size() || parsed < 1U ||
      parsed > 300U) {
    throw std::runtime_error{std::string{variable} +
                             " must be an integer from 1 through 300."};
  }
  return std::chrono::seconds{parsed};
}

[[nodiscard]] AppearanceMode appearance_mode(const ConfigSource &source) {
  constexpr std::string_view variable{"SANGUINIUS_APPEARANCES_MODE"};
  const auto value = source.environment(variable);
  if (!value.has_value()) {
    return AppearanceMode::off;
  }
  if (*value == "off") {
    return AppearanceMode::off;
  }
  if (*value == "dry_run") {
    return AppearanceMode::dry_run;
  }
  if (*value == "live") {
    return AppearanceMode::live;
  }
  throw std::runtime_error{std::string{variable} +
                           " must be exactly off, dry_run, or live."};
}

[[nodiscard]] std::string enabled(const bool value) {
  return value ? "enabled" : "disabled";
}

} // namespace

Config Config::from_environment() {
  const EnvironmentConfigSource source;
  return from_source(source);
}

DiscordCommandConfiguration discord_command_configuration_from_environment() {
  const EnvironmentConfigSource source;
  return discord_command_configuration_from_source(source);
}

DiscordCommandConfiguration
discord_command_configuration_from_source(const ConfigSource &source) {
  DiscordCommandConfiguration config;
  const auto token = optional_nonempty(source, "SANGUINIUS_TOKEN");
  const auto token_file = optional_nonempty(source, "SANGUINIUS_TOKEN_FILE");
  if (token.has_value()) {
    config.token = *token;
  } else if (token_file.has_value()) {
    config.token =
        secret_from_file(source, *token_file, "SANGUINIUS_TOKEN_FILE");
  }
  if (config.token.empty()) {
    throw std::runtime_error{"No Discord token configured. Set "
                             "SANGUINIUS_TOKEN or SANGUINIUS_TOKEN_FILE."};
  }

  config.guild_id = required_snowflake(source, "SANGUINIUS_GUILD_ID");
  ConfigurationOrigin ignored_origin{};
  config.request_timeout = request_timeout(source, ignored_origin);
  config.admin_commands_enabled =
      optional_boolean(source, "SANGUINIUS_ADMIN_COMMANDS_ENABLED", false);
  config.chronicle_enabled =
      optional_boolean(source, "SANGUINIUS_CHRONICLE_ENABLED", false);
  config.tarot_enabled =
      optional_boolean(source, "SANGUINIUS_TAROT_ENABLED", false);
  config.vox_enabled =
      optional_boolean(source, "SANGUINIUS_VOX_ENABLED", false);
  return config;
}

std::filesystem::path database_file_from_environment() {
  constexpr const char *variable = "SANGUINIUS_DATABASE_FILE";
  const char *value = std::getenv(variable);
  if (value == nullptr) {
    return "state/sanguinius.sqlite3";
  }
  if (*value == '\0') {
    throw std::runtime_error{"SANGUINIUS_DATABASE_FILE must not be empty."};
  }
  return value;
}

Config Config::from_source(const ConfigSource &source) {
  Config config;

  const auto token = optional_nonempty(source, "SANGUINIUS_TOKEN");
  const auto token_file = optional_nonempty(source, "SANGUINIUS_TOKEN_FILE");
  if (token.has_value()) {
    config.discord.token = *token;
  } else if (token_file.has_value()) {
    config.discord.token =
        secret_from_file(source, *token_file, "SANGUINIUS_TOKEN_FILE");
  }
  if (config.discord.token.empty()) {
    throw std::runtime_error{"No Discord token configured. Set "
                             "SANGUINIUS_TOKEN or SANGUINIUS_TOKEN_FILE."};
  }

  config.discord.server_scope = ServerScopeConfiguration{
      .guild_id = required_snowflake(source, "SANGUINIUS_GUILD_ID"),
      .primary_channel_id =
          required_snowflake(source, "SANGUINIUS_PRIMARY_CHANNEL_ID"),
      .owner_user_id = required_snowflake(source, "SANGUINIUS_OWNER_USER_ID"),
  };
  config.discord.request_timeout =
      request_timeout(source, config.origins.discord_request_timeout);

  if (const auto log_file = optional_nonempty(source, "SANGUINIUS_LOG_FILE");
      log_file.has_value()) {
    config.paths.message_log = *log_file;
    config.origins.message_log = ConfigurationOrigin::configured;
  }
  if (const auto database_file =
          optional_nonempty(source, "SANGUINIUS_DATABASE_FILE");
      database_file.has_value()) {
    config.paths.database_file = *database_file;
    config.origins.database_file = ConfigurationOrigin::configured;
  }

  if (const auto prefix =
          optional_nonempty(source, "SANGUINIUS_COMMAND_PREFIX");
      prefix.has_value()) {
    config.command_prefix = *prefix;
    config.origins.command_prefix = ConfigurationOrigin::configured;
  }

  const auto api_key = optional_nonempty(source, "OPENAI_API_KEY");
  const auto key_file =
      optional_nonempty(source, "SANGUINIUS_OPENAI_API_KEY_FILE");
  if (api_key.has_value()) {
    config.ai.api_key = *api_key;
  } else if (key_file.has_value()) {
    config.ai.api_key =
        secret_from_file(source, *key_file, "SANGUINIUS_OPENAI_API_KEY_FILE");
  }
  if (config.ai.api_key.empty()) {
    throw std::runtime_error{"No OpenAI API key configured. Set "
                             "OPENAI_API_KEY or "
                             "SANGUINIUS_OPENAI_API_KEY_FILE."};
  }

  if (const auto model = optional_nonempty(source, "SANGUINIUS_OPENAI_MODEL");
      model.has_value()) {
    config.ai.model = *model;
    config.origins.openai_model = ConfigurationOrigin::configured;
  }
  if (const auto persona_file =
          optional_nonempty(source, "SANGUINIUS_PERSONA_FILE");
      persona_file.has_value()) {
    config.ai.persona_file = *persona_file;
    config.origins.persona_file = ConfigurationOrigin::configured;
  }
  config.ai.persona =
      text_from_file(source, config.ai.persona_file, "SANGUINIUS_PERSONA_FILE");
  if (blank(config.ai.persona)) {
    throw std::runtime_error{"The file configured by "
                             "SANGUINIUS_PERSONA_FILE is empty."};
  }

  if (config.command_prefix.size() > 8 ||
      std::any_of(config.command_prefix.begin(), config.command_prefix.end(),
                  [](const unsigned char character) {
                    return std::isspace(character) != 0;
                  })) {
    throw std::runtime_error{
        "SANGUINIUS_COMMAND_PREFIX must be 1-8 non-whitespace characters."};
  }

  config.controls.admin_commands_enabled =
      optional_boolean(source, "SANGUINIUS_ADMIN_COMMANDS_ENABLED", false);
  config.controls.test_mode =
      optional_boolean(source, "SANGUINIUS_TEST_MODE", false);
  config.features.chronicle_enabled =
      optional_boolean(source, "SANGUINIUS_CHRONICLE_ENABLED", false);
  config.features.tarot_enabled =
      optional_boolean(source, "SANGUINIUS_TAROT_ENABLED", false);
  config.tarot_policy.starting_fate = optional_integer(
      source, "SANGUINIUS_TAROT_STARTING_FATE", 100, 1, 1'000'000'000);
  config.tarot_policy.grace_threshold = optional_integer(
      source, "SANGUINIUS_TAROT_GRACE_THRESHOLD", 10, 1, 1'000'000'000);
  config.tarot_policy.grace_target = optional_integer(
      source, "SANGUINIUS_TAROT_GRACE_TARGET", 25, 1, 1'000'000'000);
  config.tarot_policy.grace_cooldown_hours = optional_integer(
      source, "SANGUINIUS_TAROT_GRACE_COOLDOWN_HOURS", 72, 1, 8'760);
  config.tarot_policy.trial_threshold = optional_integer(
      source, "SANGUINIUS_TAROT_TRIAL_THRESHOLD", 50, 1, 1'000'000'000);
  config.tarot_policy.trial_reward_min = optional_integer(
      source, "SANGUINIUS_TAROT_TRIAL_REWARD_MIN", 5, 1, 1'000'000'000);
  config.tarot_policy.trial_reward_max = optional_integer(
      source, "SANGUINIUS_TAROT_TRIAL_REWARD_MAX", 15, 1, 1'000'000'000);
  config.tarot_policy.trial_cooldown_hours = optional_integer(
      source, "SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS", 24, 1, 8'760);
  config.wager_policy.minimum_stake = optional_integer(
      source, "SANGUINIUS_TAROT_WAGER_MINIMUM_STAKE", 1, 1, 100);
  config.wager_policy.maximum_stake = optional_integer(
      source, "SANGUINIUS_TAROT_WAGER_MAXIMUM_STAKE", 100, 1, 100);
  config.wager_policy.offer_expiry_hours = optional_integer(
      source, "SANGUINIUS_TAROT_WAGER_OFFER_EXPIRY_HOURS", 24, 1, 8'760);
  config.wager_policy.default_outcome_hours = optional_integer(
      source, "SANGUINIUS_TAROT_WAGER_DEFAULT_OUTCOME_HOURS", 24, 1, 168);
  config.wager_policy.resolution_grace_hours = optional_integer(
      source, "SANGUINIUS_TAROT_WAGER_RESOLUTION_GRACE_HOURS", 48, 1, 168);
  config.tarot_house_policy.house_enabled =
      optional_boolean(source, "SANGUINIUS_TAROT_HOUSE_ENABLED", true);
  config.tarot_house_policy.integration_enabled =
      optional_boolean(source, "SANGUINIUS_TAROT_INTEGRATION_ENABLED", true);
  config.tarot_house_policy.draw_cooldown_ms =
      optional_integer(source, "SANGUINIUS_TAROT_DRAW_COOLDOWN_HOURS", 24, 1,
                       744) *
      3'600'000;
  config.tarot_house_policy.exposure_cap = optional_integer(
      source, "SANGUINIUS_TAROT_HOUSE_EXPOSURE_CAP", 100, 1, 1'000'000);
  config.tarot_house_policy.profit_cap = optional_integer(
      source, "SANGUINIUS_TAROT_HOUSE_PROFIT_CAP", 20, 1, 1'000);
  try {
    config.tarot_policy.validate();
    config.wager_policy.validate();
    config.tarot_house_policy.validate();
  } catch (const std::invalid_argument &) {
    throw std::runtime_error{
        "Tarot settings require Grace target above threshold and ordered "
        "Trial reward bounds; wager stake bounds and durations must also be "
        "ordered and positive; House exposure, profit, and draw cooldown "
        "must remain within safe bounds."};
  }
  config.features.appearances_mode = appearance_mode(source);
  config.features.vox_enabled =
      optional_boolean(source, "SANGUINIUS_VOX_ENABLED", false);
  config.features.voice_input_enabled =
      optional_boolean(source, "SANGUINIUS_VOICE_INPUT_ENABLED", false);
  if (config.features.voice_input_enabled) {
    throw std::runtime_error{
        "SANGUINIUS_VOICE_INPUT_ENABLED=true is unsupported; Milestone 14 "
        "implements voice output only."};
  }

  if (const auto policy_file =
          optional_nonempty(source, "SANGUINIUS_APPEARANCE_POLICY_FILE")) {
    config.paths.appearance_policy_file = *policy_file;
    config.origins.appearance_policy_file = ConfigurationOrigin::configured;
  }
  config.appearance_policy = parse_appearance_policy(
      text_from_file(source, config.paths.appearance_policy_file,
                     "SANGUINIUS_APPEARANCE_POLICY_FILE"));

  if (const auto deck_file =
          optional_nonempty(source, "SANGUINIUS_TAROT_DECK_FILE")) {
    config.paths.tarot_deck_file = *deck_file;
    config.origins.tarot_deck_file = ConfigurationOrigin::configured;
  }
  if (const auto house_file =
          optional_nonempty(source, "SANGUINIUS_TAROT_HOUSE_FILE")) {
    config.paths.tarot_house_file = *house_file;
    config.origins.tarot_house_file = ConfigurationOrigin::configured;
  }
  if (config.features.tarot_enabled) {
    config.tarot_deck_catalog = parse_tarot_deck_catalog(text_from_file(
        source, config.paths.tarot_deck_file, "SANGUINIUS_TAROT_DECK_FILE"));
    config.tarot_house_catalog = parse_tarot_house_catalog(
        text_from_file(source, config.paths.tarot_house_file,
                       "SANGUINIUS_TAROT_HOUSE_FILE"),
        config.tarot_house_policy.profit_cap);
  }

  if (const auto timezone = optional_nonempty(source, "SANGUINIUS_TIMEZONE")) {
    config.timezone = *timezone;
    config.origins.timezone = ConfigurationOrigin::configured;
  }
  try {
    static_cast<void>(std::chrono::locate_zone(config.timezone));
  } catch (...) {
    throw std::runtime_error{
        "SANGUINIUS_TIMEZONE must name an installed IANA time zone."};
  }

  return config;
}

std::string_view
configuration_origin_name(const ConfigurationOrigin origin) noexcept {
  switch (origin) {
  case ConfigurationOrigin::default_value:
    return "default";
  case ConfigurationOrigin::configured:
    return "configured";
  }
  return "unknown";
}

std::string redacted_config_summary(const Config &config,
                                    const BuildInfo &build) {
  std::ostringstream output;
  output << "Configuration valid\n"
         << "version=" << build.version << '\n'
         << "revision=" << build.revision << '\n'
         << "discord_token=configured\n"
         << "openai_api_key=configured\n"
         << "guild_id=configured\n"
         << "primary_channel_id=configured\n"
         << "owner_user_id=configured\n"
         << "message_log="
         << configuration_origin_name(config.origins.message_log) << '\n'
         << "database_file="
         << configuration_origin_name(config.origins.database_file) << '\n'
         << "command_prefix="
         << configuration_origin_name(config.origins.command_prefix) << '\n'
         << "openai_model="
         << configuration_origin_name(config.origins.openai_model) << '\n'
         << "persona_file="
         << configuration_origin_name(config.origins.persona_file) << '\n'
         << "timezone=" << configuration_origin_name(config.origins.timezone)
         << '\n'
         << "appearance_policy_file="
         << configuration_origin_name(config.origins.appearance_policy_file)
         << '\n'
         << "appearance_policy_version="
         << config.appearance_policy.policy_version << '\n'
         << "tarot_deck_file="
         << configuration_origin_name(config.origins.tarot_deck_file) << '\n'
         << "tarot_house_file="
         << configuration_origin_name(config.origins.tarot_house_file) << '\n'
         << "tarot_deck_version="
         << (config.tarot_deck_catalog
                 ? config.tarot_deck_catalog->version
                 : std::string{emperor_tarot_catalog_version})
         << '\n'
         << "tarot_house_catalog_version="
         << (config.tarot_house_catalog
                 ? config.tarot_house_catalog->version
                 : std::string{tarot_house_catalog_version})
         << '\n'
         << "discord_request_timeout="
         << configuration_origin_name(config.origins.discord_request_timeout)
         << '\n'
         << "admin_commands=" << enabled(config.controls.admin_commands_enabled)
         << '\n'
         << "test_mode=" << enabled(config.controls.test_mode) << '\n'
         << "chronicle=" << enabled(config.features.chronicle_enabled) << '\n'
         << "tarot=" << enabled(config.features.tarot_enabled) << '\n'
         << "tarot_starting_fate=" << config.tarot_policy.starting_fate << '\n'
         << "tarot_grace=" << config.tarot_policy.grace_threshold << "->"
         << config.tarot_policy.grace_target << "/"
         << config.tarot_policy.grace_cooldown_hours << "h\n"
         << "tarot_trial=" << config.tarot_policy.trial_threshold << "/"
         << config.tarot_policy.trial_reward_min << "-"
         << config.tarot_policy.trial_reward_max << "/"
         << config.tarot_policy.trial_cooldown_hours << "h\n"
         << "tarot_wager_stake=" << config.wager_policy.minimum_stake << "-"
         << config.wager_policy.maximum_stake << '\n'
         << "tarot_wager_timing=" << config.wager_policy.offer_expiry_hours
         << "/" << config.wager_policy.default_outcome_hours << "/"
         << config.wager_policy.resolution_grace_hours << "h\n"
         << "tarot_draw_cooldown="
         << config.tarot_house_policy.draw_cooldown_ms / 3'600'000 << "h\n"
         << "tarot_house=" << enabled(config.tarot_house_policy.house_enabled)
         << '\n'
         << "tarot_house_exposure=" << config.tarot_house_policy.exposure_cap
         << '\n'
         << "tarot_house_profit_cap=" << config.tarot_house_policy.profit_cap
         << '\n'
         << "tarot_integration="
         << enabled(config.tarot_house_policy.integration_enabled) << '\n'
         << "appearances="
         << appearance_mode_name(config.features.appearances_mode) << '\n'
         << "vox=" << enabled(config.features.vox_enabled) << '\n'
         << "voice_input=" << enabled(config.features.voice_input_enabled)
         << '\n';
  return output.str();
}

} // namespace sanguinius
