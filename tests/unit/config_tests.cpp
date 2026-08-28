#include "sanguinius/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class FakeConfigSource final : public sanguinius::ConfigSource {
public:
  FakeConfigSource() {
    values = {
        {"SANGUINIUS_TOKEN", "DISCORD_SECRET_SENTINEL"},
        {"OPENAI_API_KEY", "OPENAI_SECRET_SENTINEL"},
        {"SANGUINIUS_GUILD_ID", "18446744073709551615"},
        {"SANGUINIUS_PRIMARY_CHANNEL_ID", "9223372036854775808"},
        {"SANGUINIUS_OWNER_USER_ID", "123456789012345678"},
    };
    files.emplace("config/persona.txt", "PERSONA_SECRET_SENTINEL");
    const auto policy_path = std::filesystem::path{__FILE__}
                                 .parent_path()
                                 .parent_path()
                                 .parent_path() /
                             "config/appearance-policy-v2.json";
    std::ifstream policy{policy_path};
    files.emplace("config/appearance-policy-v2.json",
                  std::string{std::istreambuf_iterator<char>{policy},
                              std::istreambuf_iterator<char>{}});
  }

  std::optional<std::string>
  environment(const std::string_view name) const override {
    const auto found = values.find(std::string{name});
    if (found == values.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::string read_file(const std::filesystem::path &path) const override {
    read_paths.push_back(path);
    const auto found = files.find(path);
    if (found == files.end()) {
      throw std::runtime_error{"scripted unreadable path"};
    }
    return found->second;
  }

  std::map<std::string, std::string, std::less<>> values;
  std::map<std::filesystem::path, std::string> files;
  mutable std::vector<std::filesystem::path> read_paths;
};

class TemporaryUnusedPath {
public:
  TemporaryUnusedPath() {
    static std::atomic<unsigned long long> sequence{0};
    const auto unique = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("sanguinius-unused-database-" + std::to_string(unique) + "-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
  }

  ~TemporaryUnusedPath() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] bool contains(const std::string_view value,
                            const std::string_view fragment) {
  return value.find(fragment) != std::string_view::npos;
}

[[nodiscard]] std::string config_error(const FakeConfigSource &source) {
  try {
    static_cast<void>(sanguinius::Config::from_source(source));
  } catch (const std::exception &error) {
    return error.what();
  }
  return {};
}

} // namespace

TEST_CASE("typed configuration loads safe defaults and full snowflakes",
          "[config]") {
  FakeConfigSource source;
  const auto config = sanguinius::Config::from_source(source);

  REQUIRE(config.discord.token == "DISCORD_SECRET_SENTINEL");
  REQUIRE(config.discord.server_scope.guild_id.str() == "18446744073709551615");
  REQUIRE(config.discord.server_scope.primary_channel_id.str() ==
          "9223372036854775808");
  REQUIRE(config.discord.server_scope.owner_user_id.str() ==
          "123456789012345678");
  REQUIRE(config.discord.request_timeout == std::chrono::seconds{10});
  REQUIRE(config.paths.message_log == "logs/messages.log");
  REQUIRE(config.paths.database_file == "state/sanguinius.sqlite3");
  REQUIRE(config.command_prefix == "!");
  REQUIRE(config.timezone == "America/New_York");
  REQUIRE(config.ai.model == "gpt-5.6-luna");
  REQUIRE(config.origins.discord_request_timeout ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.message_log ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.database_file ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.command_prefix ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.openai_model ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.persona_file ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE(config.origins.timezone ==
          sanguinius::ConfigurationOrigin::default_value);
  REQUIRE_FALSE(config.controls.admin_commands_enabled);
  REQUIRE_FALSE(config.controls.test_mode);
  REQUIRE_FALSE(config.features.chronicle_enabled);
  REQUIRE_FALSE(config.features.tarot_enabled);
  REQUIRE(config.tarot_policy.starting_fate == 100);
  REQUIRE(config.tarot_policy.grace_threshold == 10);
  REQUIRE(config.tarot_policy.grace_target == 25);
  REQUIRE(config.tarot_policy.grace_cooldown_hours == 72);
  REQUIRE(config.tarot_policy.trial_threshold == 50);
  REQUIRE(config.tarot_policy.trial_reward_min == 5);
  REQUIRE(config.tarot_policy.trial_reward_max == 15);
  REQUIRE(config.tarot_policy.trial_cooldown_hours == 24);
  REQUIRE(config.wager_policy.minimum_stake == 1);
  REQUIRE(config.wager_policy.maximum_stake == 100);
  REQUIRE(config.wager_policy.offer_expiry_hours == 24);
  REQUIRE(config.wager_policy.default_outcome_hours == 24);
  REQUIRE(config.wager_policy.resolution_grace_hours == 48);
  REQUIRE(config.features.appearances_mode == sanguinius::AppearanceMode::off);
  REQUIRE_FALSE(config.features.vox_enabled);
  REQUIRE_FALSE(config.features.vox_narration_enabled);
  REQUIRE_FALSE(config.features.voice_input_enabled);
  REQUIRE(source.read_paths ==
          std::vector<std::filesystem::path>{
              "config/persona.txt", "config/appearance-policy-v2.json"});
}

TEST_CASE("Discord command configuration does not load application services",
          "[config][discord-commands]") {
  FakeConfigSource source;
  source.values.erase("OPENAI_API_KEY");
  source.values.erase("SANGUINIUS_PRIMARY_CHANNEL_ID");
  source.values.erase("SANGUINIUS_OWNER_USER_ID");
  source.values["SANGUINIUS_ADMIN_COMMANDS_ENABLED"] = "true";
  source.values["SANGUINIUS_TEST_MODE"] = "true";
  source.values["SANGUINIUS_CHRONICLE_ENABLED"] = "true";
  source.values["SANGUINIUS_TAROT_ENABLED"] = "true";
  source.values["SANGUINIUS_VOX_ENABLED"] = "true";
  source.values["SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS"] = "17";
  source.files.clear();

  const auto config =
      sanguinius::discord_command_configuration_from_source(source);
  REQUIRE(config.token == "DISCORD_SECRET_SENTINEL");
  REQUIRE(config.guild_id.str() == "18446744073709551615");
  REQUIRE(config.request_timeout == std::chrono::seconds{17});
  REQUIRE(config.admin_commands_enabled);
  REQUIRE(config.test_mode);
  REQUIRE(config.chronicle_enabled);
  REQUIRE(config.tarot_enabled);
  REQUIRE(config.vox_enabled);
  REQUIRE(source.read_paths.empty());
}

TEST_CASE("Tarot configuration parses confirmed overrides and relationships",
          "[config][tarot]") {
  FakeConfigSource source;
  source.values["SANGUINIUS_TAROT_STARTING_FATE"] = "200";
  source.values["SANGUINIUS_TAROT_GRACE_THRESHOLD"] = "12";
  source.values["SANGUINIUS_TAROT_GRACE_TARGET"] = "30";
  source.values["SANGUINIUS_TAROT_GRACE_COOLDOWN_HOURS"] = "48";
  source.values["SANGUINIUS_TAROT_TRIAL_THRESHOLD"] = "60";
  source.values["SANGUINIUS_TAROT_TRIAL_REWARD_MIN"] = "6";
  source.values["SANGUINIUS_TAROT_TRIAL_REWARD_MAX"] = "12";
  source.values["SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS"] = "36";
  source.values["SANGUINIUS_TAROT_WAGER_MINIMUM_STAKE"] = "2";
  source.values["SANGUINIUS_TAROT_WAGER_MAXIMUM_STAKE"] = "75";
  source.values["SANGUINIUS_TAROT_WAGER_OFFER_EXPIRY_HOURS"] = "12";
  source.values["SANGUINIUS_TAROT_WAGER_DEFAULT_OUTCOME_HOURS"] = "72";
  source.values["SANGUINIUS_TAROT_WAGER_RESOLUTION_GRACE_HOURS"] = "36";
  const auto config = sanguinius::Config::from_source(source);
  REQUIRE(config.tarot_policy.starting_fate == 200);
  REQUIRE(config.tarot_policy.grace_threshold == 12);
  REQUIRE(config.tarot_policy.grace_target == 30);
  REQUIRE(config.tarot_policy.grace_cooldown_hours == 48);
  REQUIRE(config.tarot_policy.trial_threshold == 60);
  REQUIRE(config.tarot_policy.trial_reward_min == 6);
  REQUIRE(config.tarot_policy.trial_reward_max == 12);
  REQUIRE(config.tarot_policy.trial_cooldown_hours == 36);
  REQUIRE(config.wager_policy.minimum_stake == 2);
  REQUIRE(config.wager_policy.maximum_stake == 75);
  REQUIRE(config.wager_policy.offer_expiry_hours == 12);
  REQUIRE(config.wager_policy.default_outcome_hours == 72);
  REQUIRE(config.wager_policy.resolution_grace_hours == 36);

  source.values["SANGUINIUS_TAROT_GRACE_TARGET"] = "12";
  REQUIRE(contains(config_error(source), "Grace target above threshold"));
  source.values["SANGUINIUS_TAROT_GRACE_TARGET"] = "30";
  source.values["SANGUINIUS_TAROT_TRIAL_REWARD_MIN"] = "13";
  REQUIRE(contains(config_error(source), "ordered Trial reward bounds"));
  source.values["SANGUINIUS_TAROT_TRIAL_REWARD_MIN"] = "6";
  source.values["SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS"] = "8761";
  REQUIRE(
      contains(config_error(source), "SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS"));
  source.values["SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS"] = "36";
  source.values["SANGUINIUS_TAROT_WAGER_MINIMUM_STAKE"] = "76";
  REQUIRE(contains(config_error(source), "wager stake bounds"));
  source.values["SANGUINIUS_TAROT_WAGER_MINIMUM_STAKE"] = "2";
  source.values["SANGUINIUS_TAROT_WAGER_RESOLUTION_GRACE_HOURS"] = "169";
  REQUIRE(contains(config_error(source),
                   "SANGUINIUS_TAROT_WAGER_RESOLUTION_GRACE_HOURS"));
}

TEST_CASE("enabled Tarot configuration validates both catalog files",
          "[config][tarot][catalog]") {
  const auto repository =
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
  const auto read_fixture = [](const std::filesystem::path &path) {
    std::ifstream input{path};
    REQUIRE(input.good());
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  };

  FakeConfigSource source;
  source.values["SANGUINIUS_TAROT_ENABLED"] = "true";
  source.values["SANGUINIUS_TAROT_DECK_FILE"] = "DECK_PATH_SENTINEL";
  source.values["SANGUINIUS_TAROT_HOUSE_FILE"] = "HOUSE_PATH_SENTINEL";
  source.files["DECK_PATH_SENTINEL"] =
      read_fixture(repository / "config/emperor-tarot-v1.json");
  source.files["HOUSE_PATH_SENTINEL"] =
      read_fixture(repository / "config/tarot-house-v1.json");

  const auto config = sanguinius::Config::from_source(source);
  REQUIRE(config.tarot_deck_catalog.has_value());
  REQUIRE(config.tarot_house_catalog.has_value());
  REQUIRE(config.tarot_deck_catalog->cards.size() == 22);

  source.files.erase("DECK_PATH_SENTINEL");
  const auto missing_error = config_error(source);
  REQUIRE(contains(missing_error, "SANGUINIUS_TAROT_DECK_FILE"));
  REQUIRE_FALSE(contains(missing_error, "DECK_PATH_SENTINEL"));

  source.files["DECK_PATH_SENTINEL"] = "{}";
  REQUIRE_FALSE(config_error(source).empty());
}

TEST_CASE("typed configuration parses strict controls features and duration",
          "[config]") {
  FakeConfigSource source;
  source.values["SANGUINIUS_ADMIN_COMMANDS_ENABLED"] = "true";
  source.values["SANGUINIUS_TEST_MODE"] = "true";
  source.values["SANGUINIUS_CHRONICLE_ENABLED"] = "true";
  source.values["SANGUINIUS_TAROT_ENABLED"] = "false";
  source.values["SANGUINIUS_APPEARANCES_MODE"] = "dry_run";
  source.values["SANGUINIUS_VOX_ENABLED"] = "true";
  source.values["SANGUINIUS_VOX_NARRATION_ENABLED"] = "true";
  source.values["SANGUINIUS_TTS_PROVIDER"] = "openai";
  source.values["SANGUINIUS_VOICE_INPUT_ENABLED"] = "false";
  source.values["SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS"] = "300";
  source.values["SANGUINIUS_DATABASE_FILE"] = "DATABASE_PATH_SENTINEL";
  source.values["SANGUINIUS_LOG_FILE"] = "LOG_PATH_SENTINEL";
  source.values["SANGUINIUS_COMMAND_PREFIX"] = "?";
  source.values["SANGUINIUS_OPENAI_MODEL"] = "configured-model";
  source.values["SANGUINIUS_PERSONA_FILE"] = "configured-persona";
  source.values["SANGUINIUS_TIMEZONE"] = "UTC";
  source.files["configured-persona"] = "configured persona";
  source.values["SANGUINIUS_APPEARANCE_POLICY_FILE"] = "configured-policy";
  source.files["configured-policy"] =
      source.files["config/appearance-policy-v2.json"];

  const auto config = sanguinius::Config::from_source(source);
  REQUIRE(config.controls.admin_commands_enabled);
  REQUIRE(config.controls.test_mode);
  REQUIRE(config.features.chronicle_enabled);
  REQUIRE_FALSE(config.features.tarot_enabled);
  REQUIRE(config.features.appearances_mode ==
          sanguinius::AppearanceMode::dry_run);
  REQUIRE(config.features.vox_enabled);
  REQUIRE(config.features.vox_narration_enabled);
  REQUIRE_FALSE(config.features.voice_input_enabled);
  REQUIRE(config.discord.request_timeout == std::chrono::seconds{300});
  REQUIRE(config.paths.database_file == "DATABASE_PATH_SENTINEL");
  REQUIRE(config.paths.message_log == "LOG_PATH_SENTINEL");
  REQUIRE(config.command_prefix == "?");
  REQUIRE(config.ai.model == "configured-model");
  REQUIRE(config.ai.persona_file == "configured-persona");
  REQUIRE(config.timezone == "UTC");
  REQUIRE(config.origins.discord_request_timeout ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.message_log ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.database_file ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.command_prefix ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.openai_model ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.persona_file ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(config.origins.timezone ==
          sanguinius::ConfigurationOrigin::configured);
  REQUIRE(source.read_paths.size() == 2);
}

TEST_CASE("configuration rejects missing invalid and zero scope IDs",
          "[config]") {
  for (const std::string variable :
       {"SANGUINIUS_GUILD_ID", "SANGUINIUS_PRIMARY_CHANNEL_ID",
        "SANGUINIUS_OWNER_USER_ID"}) {
    FakeConfigSource missing;
    missing.values.erase(variable);
    const auto missing_error = config_error(missing);
    REQUIRE(contains(missing_error, variable));
    REQUIRE(contains(missing_error, "required"));

    FakeConfigSource invalid;
    invalid.values[variable] = "INVALID_ID_SENTINEL";
    const auto invalid_error = config_error(invalid);
    REQUIRE(contains(invalid_error, variable));
    REQUIRE_FALSE(contains(invalid_error, "INVALID_ID_SENTINEL"));

    FakeConfigSource zero;
    zero.values[variable] = "0";
    REQUIRE(contains(config_error(zero), "nonzero"));
  }
}

TEST_CASE("configuration rejects noncanonical booleans and durations",
          "[config]") {
  constexpr std::string_view boolean_variables[]{
      "SANGUINIUS_ADMIN_COMMANDS_ENABLED",
      "SANGUINIUS_TEST_MODE",
      "SANGUINIUS_CHRONICLE_ENABLED",
      "SANGUINIUS_TAROT_ENABLED",
      "SANGUINIUS_VOX_ENABLED",
      "SANGUINIUS_VOX_NARRATION_ENABLED",
      "SANGUINIUS_VOICE_INPUT_ENABLED",
  };
  for (const auto variable : boolean_variables) {
    for (const std::string invalid :
         {"", "TRUE", "False", "yes", "1", " true"}) {
      FakeConfigSource source;
      source.values[std::string{variable}] = invalid;
      const auto error = config_error(source);
      REQUIRE(contains(error, variable));
      REQUIRE(contains(error, "exactly true or false"));
    }
  }

  for (const std::string invalid :
       {"", "0", "301", "-1", "+1", "1.5", "10s", " 10", "42949672960"}) {
    FakeConfigSource source;
    source.values["SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS"] = invalid;
    REQUIRE(contains(config_error(source), "integer from 1 through 300"));
  }

  constexpr std::string_view nonempty_variables[]{
      "SANGUINIUS_TOKEN",         "SANGUINIUS_TOKEN_FILE",
      "OPENAI_API_KEY",           "SANGUINIUS_OPENAI_API_KEY_FILE",
      "SANGUINIUS_LOG_FILE",      "SANGUINIUS_COMMAND_PREFIX",
      "SANGUINIUS_OPENAI_MODEL",  "SANGUINIUS_PERSONA_FILE",
      "SANGUINIUS_DATABASE_FILE", "SANGUINIUS_TIMEZONE",
  };
  for (const auto variable : nonempty_variables) {
    FakeConfigSource source;
    source.values[std::string{variable}] = "";
    const auto error = config_error(source);
    REQUIRE(contains(error, variable));
    REQUIRE(contains(error, "must not be empty"));
  }
}

TEST_CASE("Vox narration requires voice output", "[config][vox][narration]") {
  FakeConfigSource source;
  source.values["SANGUINIUS_VOX_NARRATION_ENABLED"] = "true";
  const auto error = config_error(source);
  REQUIRE(contains(error, "SANGUINIUS_VOX_NARRATION_ENABLED"));
  REQUIRE(contains(error, "requires Vox output"));
}

TEST_CASE("Vox narration requires generated TTS",
          "[config][vox][narration][tts]") {
  FakeConfigSource source;
  source.values["SANGUINIUS_VOX_ENABLED"] = "true";
  source.values["SANGUINIUS_VOX_NARRATION_ENABLED"] = "true";
  const auto error = config_error(source);
  REQUIRE(contains(error, "SANGUINIUS_VOX_NARRATION_ENABLED"));
  REQUIRE(contains(error, "OpenAI TTS provider"));
}

TEST_CASE("voice input configuration is explicit and disabled by default",
          "[config][vox][privacy][transcription]") {
  FakeConfigSource source;
  source.values["SANGUINIUS_VOICE_INPUT_ENABLED"] = "true";
  const auto error = config_error(source);
  REQUIRE(contains(error, "SANGUINIUS_VOICE_INPUT_ENABLED"));
  REQUIRE(contains(error, "requires Vox output"));

  source.values["SANGUINIUS_VOX_ENABLED"] = "true";
  source.values["SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED"] = "true";
  source.values["SANGUINIUS_TRANSCRIPTION_PROVIDER"] = "openai";
  source.values["SANGUINIUS_TRANSCRIPTION_MODEL"] = "gpt-transcribe";
  source.values["SANGUINIUS_TRANSCRIPTION_REQUEST_TIMEOUT_MS"] = "1";
  const auto configured = sanguinius::Config::from_source(source);
  REQUIRE(configured.voice_input.enabled);
  REQUIRE(configured.voice_input.guild_consent_attested);
  REQUIRE(configured.voice_input.provider_enabled);
  REQUIRE(configured.voice_input.model == "gpt-transcribe");
  REQUIRE(configured.voice_input.request_timeout ==
          std::chrono::milliseconds{1});
  REQUIRE(configured.voice_input.usage_policy.rolling_day_windows == 50);
  REQUIRE(configured.voice_input.usage_policy.rolling_day_micro_usd == 250'000);
  REQUIRE(configured.voice_input.usage_policy.calendar_month_micro_usd ==
          5'000'000);
  REQUIRE(configured.transcription_provider ==
          sanguinius::TranscriptionProvider::openai);

  for (const auto &[variable, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"SANGUINIUS_VOICE_INPUT_ROLLING_DAY_WINDOWS", "51"},
           {"SANGUINIUS_VOICE_INPUT_ROLLING_DAY_MICRO_USD", "250001"},
           {"SANGUINIUS_VOICE_INPUT_MONTHLY_MICRO_USD", "5000001"}}) {
    FakeConfigSource invalid;
    invalid.values[variable] = value;
    REQUIRE(contains(config_error(invalid), variable));
  }

  source.values["SANGUINIUS_TRANSCRIPTION_MODEL"] = "unsupported-model";
  REQUIRE(sanguinius::Config::from_source(source).voice_input.model ==
          "unsupported-model");
}

TEST_CASE("TTS configuration is fixed-contract and permits only lower limits",
          "[config][vox][tts]") {
  FakeConfigSource defaults;
  const auto default_config = sanguinius::Config::from_source(defaults);
  REQUIRE(default_config.tts.provider == sanguinius::TtsProvider::disabled);
  REQUIRE(default_config.tts.model == "tts-1");
  REQUIRE(default_config.tts.voice == "onyx");
  REQUIRE(default_config.tts.usage_policy.rolling_day_attempts == 100);
  REQUIRE(default_config.tts.usage_policy.rolling_day_micro_usd == 500'000);
  REQUIRE(default_config.tts.usage_policy.calendar_month_micro_usd ==
          10'000'000);

  FakeConfigSource lowered;
  lowered.values["SANGUINIUS_TTS_PROVIDER"] = "openai";
  lowered.values["SANGUINIUS_TTS_MODEL"] = "tts-1";
  lowered.values["SANGUINIUS_TTS_VOICE"] = "onyx";
  lowered.values["SANGUINIUS_TTS_CACHE_DIRECTORY"] = "/tmp/tts-cache";
  lowered.values["SANGUINIUS_FFMPEG_PATH"] = "/usr/bin/ffmpeg";
  lowered.values["SANGUINIUS_FFPROBE_PATH"] = "/usr/bin/ffprobe";
  lowered.values["SANGUINIUS_TTS_FALLBACK_DIRECTORY"] = "/tmp/tts-assets";
  lowered.values["SANGUINIUS_TTS_ROLLING_DAY_ATTEMPTS"] = "7";
  lowered.values["SANGUINIUS_TTS_ROLLING_DAY_MICRO_USD"] = "50000";
  lowered.values["SANGUINIUS_TTS_MONTHLY_MICRO_USD"] = "1000000";
  lowered.values["SANGUINIUS_TTS_CACHE_MAXIMUM_MIB"] = "64";
  const auto configured = sanguinius::Config::from_source(lowered);
  REQUIRE(configured.tts.provider == sanguinius::TtsProvider::openai);
  REQUIRE(configured.tts.usage_policy.rolling_day_attempts == 7);
  REQUIRE(configured.tts.cache_policy.maximum_bytes == 64U * 1024U * 1024U);

  for (const auto &[variable, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"SANGUINIUS_TTS_PROVIDER", "unknown"},
           {"SANGUINIUS_TTS_MODEL", "tts-1-hd"},
           {"SANGUINIUS_TTS_VOICE", "alloy"},
           {"SANGUINIUS_TTS_CACHE_DIRECTORY", "relative/cache"},
           {"SANGUINIUS_TTS_ROLLING_DAY_ATTEMPTS", "101"},
           {"SANGUINIUS_TTS_ROLLING_DAY_MICRO_USD", "500001"},
           {"SANGUINIUS_TTS_MONTHLY_MICRO_USD", "10000001"},
           {"SANGUINIUS_TTS_CACHE_MAXIMUM_MIB", "129"}}) {
    FakeConfigSource invalid;
    invalid.values[variable] = value;
    REQUIRE(contains(config_error(invalid), variable));
  }
}

TEST_CASE("configuration validates appearance mode database and persona",
          "[config]") {
  for (const std::string valid : {"off", "dry_run", "live"}) {
    FakeConfigSource source;
    source.values["SANGUINIUS_APPEARANCES_MODE"] = valid;
    REQUIRE_NOTHROW(sanguinius::Config::from_source(source));
  }
  FakeConfigSource bad_mode;
  bad_mode.values["SANGUINIUS_APPEARANCES_MODE"] = "DRY_RUN_SENTINEL";
  const auto mode_error = config_error(bad_mode);
  REQUIRE_FALSE(contains(mode_error, "DRY_RUN_SENTINEL"));

  FakeConfigSource invalid_timezone;
  invalid_timezone.values["SANGUINIUS_TIMEZONE"] = "INVALID_TIMEZONE_SENTINEL";
  const auto timezone_error = config_error(invalid_timezone);
  REQUIRE(contains(timezone_error, "SANGUINIUS_TIMEZONE"));
  REQUIRE_FALSE(contains(timezone_error, "INVALID_TIMEZONE_SENTINEL"));

  FakeConfigSource empty_database;
  empty_database.values["SANGUINIUS_DATABASE_FILE"] = "";
  REQUIRE(contains(config_error(empty_database), "SANGUINIUS_DATABASE_FILE"));

  TemporaryUnusedPath unused_database_root;
  const auto database_path = unused_database_root.path() / "state.sqlite3";
  REQUIRE_FALSE(std::filesystem::exists(unused_database_root.path()));
  FakeConfigSource database_placeholder;
  database_placeholder.values["SANGUINIUS_DATABASE_FILE"] =
      database_path.string();
  const auto database_config =
      sanguinius::Config::from_source(database_placeholder);
  REQUIRE(database_config.paths.database_file == database_path);
  REQUIRE_FALSE(std::filesystem::exists(unused_database_root.path()));

  FakeConfigSource blank_persona;
  blank_persona.files["config/persona.txt"] = " \n\t";
  REQUIRE(contains(config_error(blank_persona), "SANGUINIUS_PERSONA_FILE"));

  FakeConfigSource unreadable_persona;
  unreadable_persona.files.clear();
  const auto persona_error = config_error(unreadable_persona);
  REQUIRE(contains(persona_error, "SANGUINIUS_PERSONA_FILE"));
  REQUIRE_FALSE(contains(persona_error, "config/persona.txt"));
}

TEST_CASE("direct secrets retain precedence and file secrets are trimmed",
          "[config]") {
  FakeConfigSource direct;
  direct.values["SANGUINIUS_TOKEN_FILE"] = "token-file";
  direct.values["SANGUINIUS_OPENAI_API_KEY_FILE"] = "key-file";
  direct.files["token-file"] = "wrong-token";
  direct.files["key-file"] = "wrong-key";
  const auto direct_config = sanguinius::Config::from_source(direct);
  REQUIRE(direct_config.discord.token == "DISCORD_SECRET_SENTINEL");
  REQUIRE(direct_config.ai.api_key == "OPENAI_SECRET_SENTINEL");

  FakeConfigSource files;
  files.values.erase("SANGUINIUS_TOKEN");
  files.values.erase("OPENAI_API_KEY");
  files.values["SANGUINIUS_TOKEN_FILE"] = "token-file";
  files.values["SANGUINIUS_OPENAI_API_KEY_FILE"] = "key-file";
  files.files["token-file"] = "  token-value\n";
  files.files["key-file"] = "key-value\n";
  const auto file_config = sanguinius::Config::from_source(files);
  REQUIRE(file_config.discord.token == "token-value");
  REQUIRE(file_config.ai.api_key == "key-value");
}

TEST_CASE("redacted config summary excludes secrets IDs paths and persona",
          "[config][redaction]") {
  FakeConfigSource defaults;
  const auto default_config = sanguinius::Config::from_source(defaults);
  const auto default_summary = sanguinius::redacted_config_summary(
      default_config, {"safe-version", "safe-revision"});
  REQUIRE(contains(default_summary, "message_log=default"));
  REQUIRE(contains(default_summary, "database_file=default"));
  REQUIRE(contains(default_summary, "command_prefix=default"));
  REQUIRE(contains(default_summary, "openai_model=default"));
  REQUIRE(contains(default_summary, "persona_file=default"));
  REQUIRE(contains(default_summary, "discord_request_timeout=default"));
  REQUIRE(contains(default_summary, "timezone=default"));

  FakeConfigSource source;
  source.values["SANGUINIUS_DATABASE_FILE"] = "DATABASE_PATH_SENTINEL";
  source.values["SANGUINIUS_LOG_FILE"] = "LOG_PATH_SENTINEL";
  source.values["SANGUINIUS_COMMAND_PREFIX"] = "?";
  source.values["SANGUINIUS_OPENAI_MODEL"] = "MODEL_VALUE_SENTINEL";
  source.values["SANGUINIUS_PERSONA_FILE"] = "PERSONA_PATH_SENTINEL";
  source.values["SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS"] = "42";
  source.values["SANGUINIUS_TIMEZONE"] = "UTC";
  source.files["PERSONA_PATH_SENTINEL"] = "PERSONA_SECRET_SENTINEL";
  const auto config = sanguinius::Config::from_source(source);
  const auto summary = sanguinius::redacted_config_summary(
      config, {"safe-version", "safe-revision"});

  REQUIRE(contains(summary, "Configuration valid"));
  REQUIRE(contains(summary, "discord_token=configured"));
  REQUIRE(contains(summary, "guild_id=configured"));
  REQUIRE(contains(summary, "message_log=configured"));
  REQUIRE(contains(summary, "database_file=configured"));
  REQUIRE(contains(summary, "command_prefix=configured"));
  REQUIRE(contains(summary, "openai_model=configured"));
  REQUIRE(contains(summary, "persona_file=configured"));
  REQUIRE(contains(summary, "discord_request_timeout=configured"));
  REQUIRE(contains(summary, "timezone=configured"));
  REQUIRE(contains(summary, "test_mode=disabled"));
  constexpr std::string_view forbidden[]{
      "DISCORD_SECRET_SENTINEL", "OPENAI_SECRET_SENTINEL",
      "PERSONA_SECRET_SENTINEL", "DATABASE_PATH_SENTINEL",
      "18446744073709551615",    "9223372036854775808",
      "123456789012345678",      "LOG_PATH_SENTINEL",
      "MODEL_VALUE_SENTINEL",    "PERSONA_PATH_SENTINEL",
  };
  for (const auto value : forbidden) {
    REQUIRE_FALSE(contains(summary, value));
  }
}

TEST_CASE("configuration file errors redact configured secret paths",
          "[config][redaction]") {
  FakeConfigSource token_file;
  token_file.values.erase("SANGUINIUS_TOKEN");
  token_file.values["SANGUINIUS_TOKEN_FILE"] =
      "TOKEN_FILE_PATH_SECRET_SENTINEL";
  const auto token_error = config_error(token_file);
  REQUIRE(contains(token_error, "SANGUINIUS_TOKEN_FILE"));
  REQUIRE_FALSE(contains(token_error, "TOKEN_FILE_PATH_SECRET_SENTINEL"));

  FakeConfigSource key_file;
  key_file.values.erase("OPENAI_API_KEY");
  key_file.values["SANGUINIUS_OPENAI_API_KEY_FILE"] =
      "KEY_FILE_PATH_SECRET_SENTINEL";
  const auto key_error = config_error(key_file);
  REQUIRE(contains(key_error, "SANGUINIUS_OPENAI_API_KEY_FILE"));
  REQUIRE_FALSE(contains(key_error, "KEY_FILE_PATH_SECRET_SENTINEL"));
}
