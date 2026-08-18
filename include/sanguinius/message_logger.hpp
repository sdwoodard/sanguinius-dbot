#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/message_log.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace sanguinius {

class MessageLogger final : public MessageLog {
public:
  MessageLogger(const std::filesystem::path &path, const Clock &clock);

  void append(const LoggedMessage &message) override;

private:
  [[nodiscard]] static std::string escape(std::string_view value);
  [[nodiscard]] std::string eastern_timestamp() const;

  const Clock &clock_;
  std::mutex mutex_;
  std::ofstream stream_;
};

} // namespace sanguinius
