#pragma once

#include <cstddef>
#include <optional>
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
  struct JsonSchema {
    std::string name;
    std::string schema;
    bool strict{true};
  };
  std::string instructions;
  std::vector<ConversationMessage> conversation;
  std::size_t max_output_tokens{500};
  std::optional<JsonSchema> json_schema;
};

class OperationCancelled : public std::runtime_error {
public:
  OperationCancelled() : std::runtime_error{"Operation cancelled."} {}
};

class AiRefusal : public std::runtime_error {
public:
  AiRefusal() : std::runtime_error{"The model refused the request."} {}
};

class AiIncompleteResponse : public std::runtime_error {
public:
  AiIncompleteResponse() : std::runtime_error{"The model response was incomplete."} {}
};

class AiClient {
public:
  virtual ~AiClient() = default;

  [[nodiscard]] virtual std::string
  generate(const AiRequest &request, std::stop_token stop_token) const = 0;
};

} // namespace sanguinius
