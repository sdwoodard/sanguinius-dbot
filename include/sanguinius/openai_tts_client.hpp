#pragma once

#include "sanguinius/tts.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

[[nodiscard]] std::optional<std::chrono::milliseconds>
bounded_retry_after(std::string_view value) noexcept;

struct TtsHttpRequest {
  std::string url;
  std::string authorization;
  std::string json_body;
  std::chrono::milliseconds connect_timeout{5'000};
  std::chrono::milliseconds total_timeout{30'000};
  std::size_t maximum_body_bytes{maximum_tts_encoded_bytes};
};

struct TtsHttpResponse {
  long status{};
  std::string content_type;
  std::string request_id;
  std::optional<std::chrono::milliseconds> retry_after;
  std::vector<std::byte> body;
};

class TtsHttpTransport {
public:
  virtual ~TtsHttpTransport() = default;
  [[nodiscard]] virtual TtsHttpResponse
  post(const TtsHttpRequest &request, std::stop_token stop_token) const = 0;
};

class CurlTtsHttpTransport final : public TtsHttpTransport {
public:
  CurlTtsHttpTransport();
  [[nodiscard]] TtsHttpResponse
  post(const TtsHttpRequest &request,
       std::stop_token stop_token) const override;
};

struct OpenAiTtsClientConfiguration {
  std::string endpoint{"https://api.openai.com/v1/audio/speech"};
  std::chrono::milliseconds connect_timeout{5'000};
  std::chrono::milliseconds total_timeout{30'000};
  std::size_t maximum_body_bytes{maximum_tts_encoded_bytes};
};

class OpenAiTtsClient final : public TextToSpeechClient {
public:
  explicit OpenAiTtsClient(
      std::string api_key,
      std::shared_ptr<const TtsHttpTransport> transport = nullptr,
      OpenAiTtsClientConfiguration configuration = {});

  [[nodiscard]] SynthesizedAudio
  synthesize(const TtsRequest &request,
             std::stop_token stop_token) const override;

private:
  std::string api_key_;
  std::shared_ptr<const TtsHttpTransport> transport_;
  OpenAiTtsClientConfiguration configuration_;
};

} // namespace sanguinius
