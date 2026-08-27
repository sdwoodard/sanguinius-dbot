#include "sanguinius/ai_client.hpp"

#include <algorithm>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] const char *safe_message(const AiProviderErrorCategory category) {
  switch (category) {
  case AiProviderErrorCategory::timeout:
    return "AI provider request timed out.";
  case AiProviderErrorCategory::rate_limited:
    return "AI provider rate limit reached.";
  case AiProviderErrorCategory::authentication:
    return "AI provider authentication failed.";
  case AiProviderErrorCategory::server:
    return "AI provider server failure.";
  case AiProviderErrorCategory::invalid_request:
    return "AI provider rejected the request.";
  case AiProviderErrorCategory::invalid_response:
    return "AI provider returned an invalid response.";
  case AiProviderErrorCategory::transport:
    return "AI provider transport failed.";
  }
  return "AI provider failed.";
}

} // namespace

std::string sanitize_ai_provider_request_id(const std::string_view value) {
  if (value.empty() || value.size() > 128 ||
      !std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' ||
               character == '-' || character == '.';
      }))
    return {};
  return std::string{value};
}

const char *ai_purpose_name(const AiPurpose purpose) noexcept {
  switch (purpose) {
  case AiPurpose::direct:
    return "direct";
  case AiPurpose::chronicle_summary:
    return "chronicle_summary";
  case AiPurpose::appearance:
    return "appearance";
  case AiPurpose::vox_narration:
    return "vox_narration";
  case AiPurpose::vox_session:
    return "vox_session";
  }
  return "direct";
}

const char *ai_priority_name(const AiPriority priority) noexcept {
  switch (priority) {
  case AiPriority::direct:
    return "direct";
  case AiPriority::explicit_feature:
    return "explicit";
  case AiPriority::optional:
    return "optional";
  }
  return "direct";
}

AiProviderError::AiProviderError(const AiProviderErrorCategory category,
                                 std::string provider_request_id)
    : std::runtime_error{safe_message(category)}, category_{category},
      provider_request_id_{
          sanitize_ai_provider_request_id(provider_request_id)} {}

AiProviderErrorCategory AiProviderError::category() const noexcept {
  return category_;
}

const std::string &AiProviderError::provider_request_id() const noexcept {
  return provider_request_id_;
}

AiRefusal::AiRefusal(std::string provider_request_id)
    : std::runtime_error{"The model refused the request."},
      provider_request_id_{
          sanitize_ai_provider_request_id(provider_request_id)} {}

const std::string &AiRefusal::provider_request_id() const noexcept {
  return provider_request_id_;
}

AiIncompleteResponse::AiIncompleteResponse(std::string provider_request_id)
    : std::runtime_error{"The model response was incomplete."},
      provider_request_id_{
          sanitize_ai_provider_request_id(provider_request_id)} {}

const std::string &AiIncompleteResponse::provider_request_id() const noexcept {
  return provider_request_id_;
}

} // namespace sanguinius
