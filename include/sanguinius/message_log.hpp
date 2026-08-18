#pragma once

#include <string>

namespace sanguinius {

struct LoggedMessage {
  std::string author_username;
  std::string content;
};

class MessageLog {
public:
  virtual ~MessageLog() = default;

  virtual void append(const LoggedMessage &message) = 0;
};

} // namespace sanguinius
