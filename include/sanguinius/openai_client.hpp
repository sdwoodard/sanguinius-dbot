#pragma once

#include <string>
#include <vector>

namespace sanguinius {

struct ConversationMessage {
  std::string role;
  std::string content;
};

class OpenAiClient {
public:
  OpenAiClient(std::string api_key, std::string model);

  [[nodiscard]] std::string
  generate(std::string_view instructions,
           const std::vector<ConversationMessage> &conversation) const;

private:
  std::string api_key_;
  std::string model_;
};

} // namespace sanguinius
