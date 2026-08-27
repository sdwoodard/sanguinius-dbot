#pragma once

#include "sanguinius/transcription.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace sanguinius {

namespace transcription_http_detail {

struct MultipartContract {
  std::size_t part_count{};
  std::string_view model_field_name;
  std::string_view model_value;
  std::string_view file_field_name;
  std::string_view file_name;
  std::string_view file_content_type;
};

[[nodiscard]] MultipartContract
multipart_contract(std::string_view model) noexcept;

class WavStream final {
public:
  WavStream(std::span<const std::byte> pcm, std::uint32_t sample_rate,
            std::uint16_t channels, std::uint16_t bits_per_sample);

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t read(std::span<std::byte> destination) noexcept;
  [[nodiscard]] bool seek(std::size_t offset) noexcept;

private:
  std::array<std::byte, 44> header_;
  std::span<const std::byte> pcm_;
  std::size_t offset_{};
};

} // namespace transcription_http_detail

struct TranscriptionHttpRequest {
  std::string url;
  std::string authorization;
  std::string model;
  std::span<const std::byte> pcm;
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::uint16_t bits_per_sample{};
  std::chrono::milliseconds connect_timeout{5'000};
  std::chrono::milliseconds total_timeout{30'000};
  std::size_t maximum_response_bytes{64U * 1024U};
};

struct TranscriptionHttpResponse {
  long status{};
  std::string body;
  std::string request_id;
};

class TranscriptionHttpTransport {
public:
  virtual ~TranscriptionHttpTransport() = default;
  [[nodiscard]] virtual TranscriptionHttpResponse post_wav_multipart(
      const TranscriptionHttpRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started) const = 0;
};

class CurlTranscriptionHttpTransport final : public TranscriptionHttpTransport {
public:
  [[nodiscard]] TranscriptionHttpResponse post_wav_multipart(
      const TranscriptionHttpRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started) const override;
};

struct OpenAiTranscriptionClientConfiguration {
  std::string endpoint{transcription_endpoint};
  std::string model{transcription_model};
  std::chrono::milliseconds connect_timeout{5'000};
  std::chrono::milliseconds total_timeout{30'000};
  std::size_t maximum_response_bytes{64U * 1024U};
};

class OpenAiTranscriptionClient final : public TranscriptionClient {
public:
  explicit OpenAiTranscriptionClient(
      std::string api_key,
      std::shared_ptr<TranscriptionHttpTransport> transport = nullptr,
      OpenAiTranscriptionClientConfiguration configuration = {});

  [[nodiscard]] Transcript transcribe(
      const TranscriptionRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started = {}) const override;

private:
  std::string api_key_;
  std::shared_ptr<TranscriptionHttpTransport> transport_;
  OpenAiTranscriptionClientConfiguration configuration_;
};

[[nodiscard]] Transcript
parse_transcription_response(std::string_view body,
                             std::string provider_request_id = {});

} // namespace sanguinius
