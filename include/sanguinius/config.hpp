#pragma once

#include <filesystem>
#include <string>

namespace sanguinius {

struct Config {
  std::string token;
  std::filesystem::path message_log{"logs/messages.log"};
  std::string command_prefix{"!"};
  std::string openai_api_key;
  std::string openai_model{"gpt-5.4-nano"};
  std::filesystem::path persona_file{"config/persona.txt"};
  std::string persona;

  [[nodiscard]] static Config from_environment();
};

} // namespace sanguinius
