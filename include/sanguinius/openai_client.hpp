#pragma once

#include "sanguinius/ai_client.hpp"

#include <string>

namespace sanguinius {

class OpenAiClient final : public AiClient {
public:
  OpenAiClient(std::string api_key, std::string model);

  [[nodiscard]] std::string generate(const AiRequest &request,
                                     std::stop_token stop_token) const override;

private:
  std::string api_key_;
  std::string model_;
};

} // namespace sanguinius
