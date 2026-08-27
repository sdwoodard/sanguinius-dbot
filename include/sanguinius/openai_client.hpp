#pragma once

#include "sanguinius/ai_client.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace sanguinius {

class OpenAiClient final : public AiClient {
public:
  OpenAiClient(std::string api_key, std::string model);

  [[nodiscard]] AiResult generate(
      const AiRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started = {}) const override;

private:
  std::string api_key_;
  std::string model_;
};

namespace openai_client_detail {

inline constexpr std::size_t maximum_response_bytes = 1024 * 1024;

[[nodiscard]] bool append_bounded_response(std::string &destination,
                                           std::string_view chunk,
                                           std::size_t maximum_bytes);
[[nodiscard]] std::string
provider_request_id_from_header(std::string_view header_line);

} // namespace openai_client_detail

} // namespace sanguinius
