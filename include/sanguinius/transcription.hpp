#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace sanguinius {

inline constexpr std::string_view transcription_model{"gpt-transcribe"};
inline constexpr std::string_view transcription_endpoint{
    "https://api.openai.com/v1/audio/transcriptions"};
inline constexpr std::size_t maximum_transcript_bytes = 1'800;
inline constexpr std::size_t maximum_transcription_pcm_bytes = 2'880'000;
inline constexpr std::int64_t transcription_micro_usd_per_second = 75;

enum class TranscriptionFailureCategory {
  cancelled,
  invalid_request,
  timeout,
  rate_limited,
  provider_rejected,
  provider_unavailable,
  invalid_response,
  oversized_response,
  transport,
  unavailable,
};

class TranscriptionError final : public std::runtime_error {
public:
  TranscriptionError(TranscriptionFailureCategory category,
                     std::string message,
                     std::string provider_request_id = {});

  [[nodiscard]] TranscriptionFailureCategory category() const noexcept;
  [[nodiscard]] const std::string &provider_request_id() const noexcept;

private:
  TranscriptionFailureCategory category_;
  std::string provider_request_id_;
};

struct TranscriptionRequest {
  std::span<const std::byte> pcm;
  std::uint32_t sample_rate{48'000};
  std::uint16_t channels{2};
  std::uint16_t bits_per_sample{16};
  std::chrono::milliseconds timeout{30'000};
};

struct Transcript {
  std::string text;
  std::string provider_request_id;
};

class TranscriptionClient {
public:
  virtual ~TranscriptionClient() = default;
  [[nodiscard]] virtual Transcript
  transcribe(const TranscriptionRequest &request,
             std::stop_token stop_token,
             const std::function<void()> &transmission_started = {}) const = 0;
};

[[nodiscard]] std::int64_t
estimated_transcription_cost_micro_usd(std::int64_t requested_seconds);
[[nodiscard]] bool valid_transcript_text(std::string_view text) noexcept;
[[nodiscard]] std::string
sanitize_transcription_request_id(std::string_view value);
[[nodiscard]] std::array<std::byte, 44>
pcm_wav_header(std::size_t pcm_bytes, std::uint32_t sample_rate = 48'000,
               std::uint16_t channels = 2,
               std::uint16_t bits_per_sample = 16);
[[nodiscard]] const char *transcription_failure_category_name(
    TranscriptionFailureCategory category) noexcept;

} // namespace sanguinius
