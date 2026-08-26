#include "sanguinius/tts.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::size_t utf8_sequence_length(const unsigned char lead) {
  if (lead <= 0x7FU)
    return 1;
  if (lead >= 0xC2U && lead <= 0xDFU)
    return 2;
  if (lead >= 0xE0U && lead <= 0xEFU)
    return 3;
  if (lead >= 0xF0U && lead <= 0xF4U)
    return 4;
  return 0;
}

[[nodiscard]] bool continuation(const unsigned char value) noexcept {
  return value >= 0x80U && value <= 0xBFU;
}

void validate_sequence(const std::string_view input, const std::size_t offset,
                       const std::size_t length) {
  if (offset + length > input.size())
    throw TtsError{TtsFailureCategory::invalid_request,
                   "TTS text contains truncated UTF-8."};
  const auto lead = static_cast<unsigned char>(input[offset]);
  for (std::size_t index = 1; index < length; ++index) {
    if (!continuation(static_cast<unsigned char>(input[offset + index])))
      throw TtsError{TtsFailureCategory::invalid_request,
                     "TTS text contains invalid UTF-8."};
  }
  if ((length == 3 && lead == 0xE0U &&
       static_cast<unsigned char>(input[offset + 1]) < 0xA0U) ||
      (length == 3 && lead == 0xEDU &&
       static_cast<unsigned char>(input[offset + 1]) > 0x9FU) ||
      (length == 4 && lead == 0xF0U &&
       static_cast<unsigned char>(input[offset + 1]) < 0x90U) ||
      (length == 4 && lead == 0xF4U &&
       static_cast<unsigned char>(input[offset + 1]) > 0x8FU)) {
    throw TtsError{TtsFailureCategory::invalid_request,
                   "TTS text contains non-scalar UTF-8."};
  }
}

[[nodiscard]] std::string hex_digest(const std::span<const std::byte> input) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
      EVP_MD_CTX_new(), &EVP_MD_CTX_free};
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1) {
    throw std::runtime_error{"Unable to initialize the TTS cache digest."};
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1 ||
      length != 32U) {
    throw std::runtime_error{"Unable to finalize the TTS cache digest."};
  }
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < length; ++index)
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  return output.str();
}

} // namespace

TtsError::TtsError(const TtsFailureCategory category, std::string message,
                   const bool retryable,
                   std::optional<std::chrono::milliseconds> retry_after,
                   std::string provider_request_id)
    : std::runtime_error{std::move(message)}, category_{category},
      retryable_{retryable}, retry_after_{retry_after},
      provider_request_id_{std::move(provider_request_id)} {}

TtsFailureCategory TtsError::category() const noexcept { return category_; }
bool TtsError::retryable() const noexcept { return retryable_; }

std::optional<std::chrono::milliseconds> TtsError::retry_after() const noexcept {
  return retry_after_;
}

const std::string &TtsError::provider_request_id() const noexcept {
  return provider_request_id_;
}

NormalizedTtsText normalize_tts_text(const std::string_view input) {
  NormalizedTtsText result;
  result.text.reserve(input.size());
  bool pending_space = false;
  for (std::size_t offset = 0; offset < input.size();) {
    const auto lead = static_cast<unsigned char>(input[offset]);
    if (lead <= 0x7FU) {
      if (lead == 0U || (lead < 0x20U && lead != '\t' && lead != '\n' &&
                         lead != '\r') || lead == 0x7FU) {
        throw TtsError{TtsFailureCategory::invalid_request,
                       "TTS text contains a prohibited control character."};
      }
      if (lead == ' ' || lead == '\t' || lead == '\n' || lead == '\r') {
        pending_space = !result.text.empty();
        ++offset;
        continue;
      }
      if (pending_space) {
        result.text.push_back(' ');
        ++result.scalar_count;
        pending_space = false;
      }
      result.text.push_back(static_cast<char>(lead));
      ++result.scalar_count;
      ++offset;
    } else {
      const auto length = utf8_sequence_length(lead);
      if (length == 0)
        throw TtsError{TtsFailureCategory::invalid_request,
                       "TTS text contains invalid UTF-8."};
      validate_sequence(input, offset, length);
      if (length == 2 && lead == 0xC2U &&
          static_cast<unsigned char>(input[offset + 1]) >= 0x80U &&
          static_cast<unsigned char>(input[offset + 1]) <= 0x9FU) {
        throw TtsError{TtsFailureCategory::invalid_request,
                       "TTS text contains a prohibited control character."};
      }
      if (pending_space) {
        result.text.push_back(' ');
        ++result.scalar_count;
        pending_space = false;
      }
      result.text.append(input.substr(offset, length));
      ++result.scalar_count;
      offset += length;
    }
    if (result.scalar_count > maximum_tts_scalar_count ||
        result.text.size() > maximum_tts_text_bytes) {
      throw TtsError{TtsFailureCategory::invalid_request,
                     "TTS text exceeds the configured line limit."};
    }
  }
  if (result.text.empty())
    throw TtsError{TtsFailureCategory::invalid_request,
                   "TTS text must not be blank."};
  return result;
}

std::int64_t estimated_tts_cost_micro_usd(const std::size_t scalar_count) {
  if (scalar_count > static_cast<std::size_t>(
                         std::numeric_limits<std::int64_t>::max() /
                         tts_micro_usd_per_character)) {
    throw std::overflow_error{"TTS cost estimate overflowed."};
  }
  return static_cast<std::int64_t>(scalar_count) *
         tts_micro_usd_per_character;
}

bool wav_media_signature(const std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < 12)
    return false;
  constexpr std::array riff{std::byte{'R'}, std::byte{'I'}, std::byte{'F'},
                            std::byte{'F'}};
  constexpr std::array wave{std::byte{'W'}, std::byte{'A'}, std::byte{'V'},
                            std::byte{'E'}};
  return std::equal(riff.begin(), riff.end(), bytes.begin()) &&
         std::equal(wave.begin(), wave.end(), bytes.begin() + 8);
}

std::string tts_cache_key(const NormalizedTtsText &text,
                          const TtsRequest &request) {
  std::ostringstream canonical;
  canonical << "sanguinius-tts-cache-v2\n" << request.provider << '\n'
            << request.model << '\n' << request.voice << '\n'
            << static_cast<int>(request.response_format) << '\n' << request.speed
            << "\n48000\n2\n16\n" << text.text;
  const auto serialized = canonical.str();
  return hex_digest(std::as_bytes(std::span{serialized.data(),
                                            serialized.size()}));
}

std::string sha256_hex(const std::span<const std::byte> bytes) {
  return hex_digest(bytes);
}

std::vector<std::byte> validated_pcm_bytes(const PcmAudio &audio) {
  if (audio.sample_rate != 48'000 || audio.channels != 2 ||
      audio.bits_per_sample != 16 || audio.samples.empty() ||
      audio.samples.size() % 2 != 0 ||
      audio.samples.size() * sizeof(std::int16_t) > maximum_tts_pcm_bytes)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "PCM is not a bounded D++ audio stream."};
  std::vector<std::byte> bytes;
  bytes.reserve(audio.samples.size() * sizeof(std::int16_t));
  for (const auto sample : audio.samples) {
    const auto bits = static_cast<std::uint16_t>(sample);
    bytes.push_back(static_cast<std::byte>(bits & 0xFFU));
    bytes.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
  }
  return bytes;
}

const char *
tts_failure_category_name(const TtsFailureCategory category) noexcept {
  switch (category) {
  case TtsFailureCategory::cancelled:
    return "cancelled";
  case TtsFailureCategory::invalid_request:
    return "invalid_request";
  case TtsFailureCategory::budget_exhausted:
    return "budget_exhausted";
  case TtsFailureCategory::transport:
    return "transport";
  case TtsFailureCategory::timeout:
    return "timeout";
  case TtsFailureCategory::rate_limited:
    return "rate_limited";
  case TtsFailureCategory::provider_rejected:
    return "provider_rejected";
  case TtsFailureCategory::provider_unavailable:
    return "provider_unavailable";
  case TtsFailureCategory::invalid_media:
    return "invalid_media";
  case TtsFailureCategory::oversized_media:
    return "oversized_media";
  case TtsFailureCategory::decoder_failed:
    return "decoder_failed";
  case TtsFailureCategory::cache_failed:
    return "cache_failed";
  case TtsFailureCategory::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

} // namespace sanguinius
