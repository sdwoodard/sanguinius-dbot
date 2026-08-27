#pragma once

#include "sanguinius/voice_gateway.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::size_t maximum_tts_scalar_count = 350;
inline constexpr std::size_t maximum_tts_text_bytes = 1'400;
inline constexpr std::size_t maximum_tts_encoded_bytes = 5U * 1024U * 1024U;
inline constexpr std::size_t maximum_tts_pcm_bytes = 3'840'000;
inline constexpr std::int64_t maximum_tts_duration_ms = 20'000;
inline constexpr std::int64_t tts_micro_usd_per_character = 15;

enum class AudioFormat {
  wav,
};

enum class TtsFailureCategory {
  cancelled,
  invalid_request,
  budget_exhausted,
  transport,
  timeout,
  rate_limited,
  authentication,
  provider_rejected,
  provider_unavailable,
  circuit_open,
  invalid_media,
  oversized_media,
  decoder_failed,
  cache_failed,
  unavailable,
};

struct NormalizedTtsText {
  std::string text;
  std::size_t scalar_count{};
};

struct TtsRequest {
  std::string text;
  std::string provider{"openai"};
  std::string model{"tts-1"};
  std::string voice{"onyx"};
  AudioFormat response_format{AudioFormat::wav};
  double speed{1.0};
  std::chrono::milliseconds timeout{30'000};
};

struct SynthesizedAudio {
  std::vector<std::byte> bytes;
  AudioFormat format{AudioFormat::wav};
  std::string content_type;
  std::string provider_request_id;
};

struct AudioNormalizationLimits {
  std::int64_t maximum_duration_ms{maximum_tts_duration_ms};
  std::size_t maximum_output_bytes{maximum_tts_pcm_bytes};
  std::chrono::milliseconds probe_timeout{5'000};
  std::chrono::milliseconds decode_timeout{10'000};
};

struct NormalizedAudio {
  PcmAudio pcm;
  std::int64_t duration_ms{};
};

class TtsError final : public std::runtime_error {
public:
  TtsError(TtsFailureCategory category, std::string message,
           bool retryable = false,
           std::optional<std::chrono::milliseconds> retry_after = std::nullopt,
           std::string provider_request_id = {});

  [[nodiscard]] TtsFailureCategory category() const noexcept;
  [[nodiscard]] bool retryable() const noexcept;
  [[nodiscard]] std::optional<std::chrono::milliseconds>
  retry_after() const noexcept;
  [[nodiscard]] const std::string &provider_request_id() const noexcept;

private:
  TtsFailureCategory category_;
  bool retryable_{};
  std::optional<std::chrono::milliseconds> retry_after_;
  std::string provider_request_id_;
};

class TextToSpeechClient {
public:
  virtual ~TextToSpeechClient() = default;
  [[nodiscard]] virtual SynthesizedAudio
  synthesize(const TtsRequest &request, std::stop_token stop_token) const = 0;
  virtual void provider_response_validated() const {}
  virtual void
  provider_response_rejected(TtsFailureCategory /*category*/) const {}
  [[nodiscard]] virtual std::string provider_circuit_state() const {
    return "closed";
  }
};

class AudioNormalizer {
public:
  virtual ~AudioNormalizer() = default;
  [[nodiscard]] virtual NormalizedAudio
  normalize(const SynthesizedAudio &audio,
            const AudioNormalizationLimits &limits,
            std::stop_token stop_token) const = 0;
};

[[nodiscard]] NormalizedTtsText normalize_tts_text(std::string_view input);
[[nodiscard]] std::int64_t
estimated_tts_cost_micro_usd(std::size_t scalar_count);
[[nodiscard]] bool
wav_media_signature(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string tts_cache_key(const NormalizedTtsText &text,
                                        const TtsRequest &request);
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> validated_pcm_bytes(const PcmAudio &audio);
[[nodiscard]] const char *
tts_failure_category_name(TtsFailureCategory category) noexcept;

} // namespace sanguinius
