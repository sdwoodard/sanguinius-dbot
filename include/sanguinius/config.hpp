#pragma once

#include <filesystem>
#include <string>

namespace sanguinius {

struct Config {
  std::string token;
  std::filesystem::path message_log{"logs/messages.log"};
  std::string command_prefix{"!"};

  [[nodiscard]] static Config from_environment();
};

} // namespace sanguinius
