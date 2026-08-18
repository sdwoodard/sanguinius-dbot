#include "sanguinius/message_logger.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

namespace sanguinius {

MessageLogger::MessageLogger(const std::filesystem::path &path,
                             const Clock &clock)
    : clock_{clock} {
  try {
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path());
    }

    stream_.open(path, std::ios::app);
    if (!stream_) {
      throw std::runtime_error{"message log open failed"};
    }

    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
  } catch (...) {
    throw std::runtime_error{
        "Unable to initialize the message log configured by "
        "SANGUINIUS_LOG_FILE."};
  }
}

void MessageLogger::append(const LoggedMessage &message) {
  std::scoped_lock lock{mutex_};

  stream_ << eastern_timestamp() << " author=\""
          << escape(message.author_username) << "\" message=\""
          << escape(message.content) << "\"\n";
  stream_.flush();
  if (!stream_) {
    throw std::runtime_error{"Failed to write to the Discord message log."};
  }
}

std::string MessageLogger::escape(const std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());

  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }

  return escaped;
}

std::string MessageLogger::eastern_timestamp() const {
  static const auto *new_york = std::chrono::locate_zone("America/New_York");
  const std::chrono::zoned_time timestamp{new_york, clock_.now()};
  return std::format("{:%FT%T%Ez}", timestamp);
}

} // namespace sanguinius
