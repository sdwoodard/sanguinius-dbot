#include "sanguinius/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace sanguinius {
namespace {

[[nodiscard]] std::string environment_value(const char *name) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream{path};
  if (!stream) {
    throw std::runtime_error{"Unable to open file: " + path.string()};
  }

  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string read_secret(const std::filesystem::path &path) {
  auto secret = read_file(path);
  secret.erase(std::remove_if(secret.begin(), secret.end(),
                              [](const unsigned char character) {
                                return std::isspace(character) != 0;
                              }),
               secret.end());
  return secret;
}

} // namespace

Config Config::from_environment() {
  Config config;
  config.token = environment_value("SANGUINIUS_TOKEN");

  if (config.token.empty()) {
    const auto token_file = environment_value("SANGUINIUS_TOKEN_FILE");
    if (!token_file.empty()) {
      config.token = read_secret(token_file);
    }
  }

  if (config.token.empty()) {
    throw std::runtime_error{"No Discord token configured. Set "
                             "SANGUINIUS_TOKEN or SANGUINIUS_TOKEN_FILE."};
  }

  if (const auto log_file = environment_value("SANGUINIUS_LOG_FILE");
      !log_file.empty()) {
    config.message_log = log_file;
  }

  if (const auto prefix = environment_value("SANGUINIUS_COMMAND_PREFIX");
      !prefix.empty()) {
    config.command_prefix = prefix;
  }

  config.openai_api_key = environment_value("OPENAI_API_KEY");
  if (config.openai_api_key.empty()) {
    const auto key_file = environment_value("SANGUINIUS_OPENAI_API_KEY_FILE");
    if (!key_file.empty()) {
      config.openai_api_key = read_secret(key_file);
    }
  }
  if (config.openai_api_key.empty()) {
    throw std::runtime_error{"No OpenAI API key configured. Set OPENAI_API_KEY "
                             "or SANGUINIUS_OPENAI_API_KEY_FILE."};
  }

  if (const auto model = environment_value("SANGUINIUS_OPENAI_MODEL");
      !model.empty()) {
    config.openai_model = model;
  }

  if (const auto persona_file = environment_value("SANGUINIUS_PERSONA_FILE");
      !persona_file.empty()) {
    config.persona_file = persona_file;
  }
  config.persona = read_file(config.persona_file);
  if (std::all_of(config.persona.begin(), config.persona.end(),
                  [](const unsigned char character) {
                    return std::isspace(character) != 0;
                  })) {
    throw std::runtime_error{"Persona file is empty: " +
                             config.persona_file.string()};
  }

  if (config.command_prefix.size() > 8 ||
      std::any_of(config.command_prefix.begin(), config.command_prefix.end(),
                  [](const unsigned char character) {
                    return std::isspace(character) != 0;
                  })) {
    throw std::runtime_error{
        "SANGUINIUS_COMMAND_PREFIX must be 1-8 non-whitespace characters."};
  }

  return config;
}

} // namespace sanguinius
