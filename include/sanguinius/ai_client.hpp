#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

struct ConversationMessage {
  std::string role;
  std::string content;
};

enum class AiPurpose {
  direct,
  chronicle_summary,
  appearance,
  vox_narration,
  vox_session,
};

enum class AiPriority {
  direct,
  explicit_feature,
  optional,
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
  AiPurpose purpose{AiPurpose::direct};
  AiPriority priority{AiPriority::direct};
  std::optional<std::string> requester_user_id;
  std::string idempotency_key;
};

[[nodiscard]] const char *ai_purpose_name(AiPurpose purpose) noexcept;
[[nodiscard]] const char *ai_priority_name(AiPriority priority) noexcept;

struct AiResult {
  std::string text;
  std::string provider_request_id;
  std::size_t input_tokens{};
  std::size_t output_tokens{};
};

[[nodiscard]] std::string
sanitize_ai_provider_request_id(std::string_view value);

enum class AiProviderErrorCategory {
  timeout,
  rate_limited,
  authentication,
  server,
  invalid_request,
  invalid_response,
  transport,
};

class AiProviderError : public std::runtime_error {
public:
  AiProviderError(AiProviderErrorCategory category,
                  std::string provider_request_id = {});
  [[nodiscard]] AiProviderErrorCategory category() const noexcept;
  [[nodiscard]] const std::string &provider_request_id() const noexcept;

private:
  AiProviderErrorCategory category_;
  std::string provider_request_id_;
};

class OperationCancelled : public std::runtime_error {
public:
  OperationCancelled() : std::runtime_error{"Operation cancelled."} {}
};

class AiRefusal : public std::runtime_error {
public:
  explicit AiRefusal(std::string provider_request_id = {});
  [[nodiscard]] const std::string &provider_request_id() const noexcept;

private:
  std::string provider_request_id_;
};

class AiIncompleteResponse : public std::runtime_error {
public:
  explicit AiIncompleteResponse(std::string provider_request_id = {});
  [[nodiscard]] const std::string &provider_request_id() const noexcept;

private:
  std::string provider_request_id_;
};

class AiClient {
public:
  virtual ~AiClient() = default;

  [[nodiscard]] virtual AiResult
  generate(const AiRequest &request, std::stop_token stop_token,
           const std::function<void()> &transmission_started = {}) const = 0;
};

} // namespace sanguinius
