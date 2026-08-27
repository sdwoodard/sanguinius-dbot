#include "sanguinius/transcription.hpp"

#include "sanguinius/appearance_policy.hpp"

#include <limits>
#include <type_traits>
#include <utility>

namespace sanguinius {
namespace {

template <typename Integer>
void little_endian(std::array<std::byte, 44> &header, const std::size_t offset,
                   const Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  const auto converted = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    header[offset + index] = static_cast<std::byte>(
        (converted >> static_cast<unsigned int>(index * 8U)) & 0xffU);
  }
}

} // namespace

TranscriptionError::TranscriptionError(
    const TranscriptionFailureCategory category, std::string message,
    std::string provider_request_id)
    : std::runtime_error{std::move(message)}, category_{category},
      provider_request_id_{std::move(provider_request_id)} {}

TranscriptionFailureCategory TranscriptionError::category() const noexcept {
  return category_;
}

const std::string &TranscriptionError::provider_request_id() const noexcept {
  return provider_request_id_;
}

std::int64_t
estimated_transcription_cost_micro_usd(const std::int64_t requested_seconds) {
  if (requested_seconds < 1 || requested_seconds > 15)
    throw std::invalid_argument{"Transcription duration must be 1-15 seconds."};
  return requested_seconds * transcription_micro_usd_per_second;
}

bool valid_transcript_text(const std::string_view text) noexcept {
  return !text.empty() && text.size() <= maximum_transcript_bytes &&
         valid_utf8(text);
}

std::string sanitize_transcription_request_id(const std::string_view value) {
  if (value.empty() || value.size() > 256)
    return {};
  for (const unsigned char character : value) {
    if (!(character >= 'a' && character <= 'z') &&
        !(character >= 'A' && character <= 'Z') &&
        !(character >= '0' && character <= '9') && character != '-' &&
        character != '_' && character != '.' && character != ':')
      return {};
  }
  return std::string{value};
}

std::array<std::byte, 44> pcm_wav_header(const std::size_t pcm_bytes,
                                         const std::uint32_t sample_rate,
                                         const std::uint16_t channels,
                                         const std::uint16_t bits_per_sample) {
  if (pcm_bytes == 0 || pcm_bytes > std::numeric_limits<std::uint32_t>::max() ||
      sample_rate == 0 || channels == 0 || bits_per_sample != 16 ||
      pcm_bytes % (static_cast<std::size_t>(channels) * 2U) != 0)
    throw std::invalid_argument{"PCM audio cannot be represented as WAV."};
  const auto byte_rate =
      static_cast<std::uint64_t>(sample_rate) * channels * bits_per_sample / 8U;
  if (byte_rate > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument{"PCM WAV byte rate is invalid."};
  std::array<std::byte, 44> header{};
  const auto copy = [&header](const std::size_t offset,
                              const std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index)
      header[offset + index] = static_cast<std::byte>(value[index]);
  };
  copy(0, "RIFF");
  little_endian(header, 4, static_cast<std::uint32_t>(pcm_bytes + 36U));
  copy(8, "WAVE");
  copy(12, "fmt ");
  little_endian(header, 16, std::uint32_t{16});
  little_endian(header, 20, std::uint16_t{1});
  little_endian(header, 22, channels);
  little_endian(header, 24, sample_rate);
  little_endian(header, 28, static_cast<std::uint32_t>(byte_rate));
  little_endian(header, 32,
                static_cast<std::uint16_t>(channels * bits_per_sample / 8U));
  little_endian(header, 34, bits_per_sample);
  copy(36, "data");
  little_endian(header, 40, static_cast<std::uint32_t>(pcm_bytes));
  return header;
}

const char *transcription_failure_category_name(
    const TranscriptionFailureCategory category) noexcept {
  switch (category) {
  case TranscriptionFailureCategory::cancelled:
    return "cancelled";
  case TranscriptionFailureCategory::invalid_request:
    return "invalid_request";
  case TranscriptionFailureCategory::timeout:
    return "timeout";
  case TranscriptionFailureCategory::rate_limited:
    return "rate_limited";
  case TranscriptionFailureCategory::authentication:
    return "authentication";
  case TranscriptionFailureCategory::provider_rejected:
    return "provider_rejected";
  case TranscriptionFailureCategory::provider_unavailable:
    return "provider_unavailable";
  case TranscriptionFailureCategory::invalid_response:
    return "invalid_response";
  case TranscriptionFailureCategory::oversized_response:
    return "oversized_response";
  case TranscriptionFailureCategory::transport:
    return "transport";
  case TranscriptionFailureCategory::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

} // namespace sanguinius
