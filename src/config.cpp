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

[[nodiscard]] std::string read_token_file(const std::filesystem::path &path) {
  std::ifstream stream{path};
  if (!stream) {
    throw std::runtime_error{"Unable to open token file: " + path.string()};
  }

  std::string token{std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{}};
  token.erase(std::remove_if(token.begin(), token.end(),
                             [](const unsigned char character) {
                               return std::isspace(character) != 0;
                             }),
              token.end());
  return token;
}

} // namespace

Config Config::from_environment() {
  Config config;
  config.token = environment_value("SANGUINIUS_TOKEN");

  if (config.token.empty()) {
    const auto token_file = environment_value("SANGUINIUS_TOKEN_FILE");
    if (!token_file.empty()) {
      config.token = read_token_file(token_file);
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
