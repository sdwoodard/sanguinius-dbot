#include "sanguinius/message_logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace sanguinius {

MessageLogger::MessageLogger(const std::filesystem::path &path) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  stream_.open(path, std::ios::app);
  if (!stream_) {
    throw std::runtime_error{"Unable to open message log: " + path.string()};
  }

  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
}

void MessageLogger::log(const dpp::message &message) {
  std::scoped_lock lock{mutex_};

  stream_ << utc_timestamp() << " message_id=" << message.id
          << " guild_id=" << message.guild_id
          << " channel_id=" << message.channel_id
          << " author_id=" << message.author.id << " author=\""
          << escape(message.author.username) << '"' << " bot=" << std::boolalpha
          << message.author.is_bot() << " content=\"" << escape(message.content)
          << '"';

  for (const auto &attachment : message.attachments) {
    stream_ << " attachment=\"" << escape(attachment.filename) << '|'
            << escape(attachment.url) << '"';
  }

  stream_ << '\n';
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

std::string MessageLogger::utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif

  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

} // namespace sanguinius
