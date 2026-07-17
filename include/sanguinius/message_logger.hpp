#pragma once

#include <dpp/dpp.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace sanguinius {

class MessageLogger {
public:
  explicit MessageLogger(const std::filesystem::path &path);

  void log(const dpp::message &message);

private:
  [[nodiscard]] static std::string escape(std::string_view value);
  [[nodiscard]] static std::string utc_timestamp();

  std::mutex mutex_;
  std::ofstream stream_;
};

} // namespace sanguinius
