#pragma once

#include <cstddef>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace sanguinius {

struct ConversationMessage {
  std::string role;
  std::string content;
};

struct AiRequest {
  std::string instructions;
  std::vector<ConversationMessage> conversation;
  std::size_t max_output_tokens{500};
};

class OperationCancelled : public std::runtime_error {
public:
  OperationCancelled() : std::runtime_error{"Operation cancelled."} {}
};

class AiClient {
public:
  virtual ~AiClient() = default;

  [[nodiscard]] virtual std::string
  generate(const AiRequest &request, std::stop_token stop_token) const = 0;
};

} // namespace sanguinius
