#include "sanguinius/openai_tts_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sanguinius {

std::optional<std::chrono::milliseconds>
bounded_retry_after(const std::string_view value) noexcept {
  std::int64_t seconds{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), seconds, 10);
  if (value.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != value.data() + value.size() || seconds < 0)
    return std::nullopt;
  return std::chrono::milliseconds{std::min<std::int64_t>(seconds, 5) * 1'000};
}

namespace {

constexpr std::string_view speech_url{"https://api.openai.com/v1/audio/speech"};

class CurlGlobal final {
public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error{"Unable to initialize libcurl for TTS."};
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

[[nodiscard]] CurlGlobal &curl_global() {
  static CurlGlobal global;
  return global;
}

[[nodiscard]] std::string trim_ascii(std::string value) {
  const auto whitespace = [](const unsigned char character) {
    return std::isspace(character) != 0;
  };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), whitespace));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(),
              value.end());
  return value;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

struct CurlResponseBuffer {
  TtsHttpResponse response;
  std::size_t maximum_body_bytes{};
  bool oversized{};
};

size_t append_body(const char *data, const size_t size, const size_t count,
                   void *context) {
  auto &buffer = *static_cast<CurlResponseBuffer *>(context);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    buffer.oversized = true;
    return 0;
  }
  const auto bytes = size * count;
  if (bytes > buffer.maximum_body_bytes ||
      buffer.response.body.size() > buffer.maximum_body_bytes - bytes) {
    buffer.oversized = true;
    return 0;
  }
  const auto *begin = reinterpret_cast<const std::byte *>(data);
  buffer.response.body.insert(buffer.response.body.end(), begin, begin + bytes);
  return bytes;
}

size_t capture_header(const char *data, const size_t size, const size_t count,
                      void *context) {
  auto &response = static_cast<CurlResponseBuffer *>(context)->response;
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
    return 0;
  const auto bytes = size * count;
  if (bytes > 8'192)
    return 0;
  std::string_view line{data, bytes};
  const auto separator = line.find(':');
  if (separator == std::string_view::npos)
    return bytes;
  const auto name =
      lower_ascii(trim_ascii(std::string{line.substr(0, separator)}));
  const auto value = trim_ascii(std::string{line.substr(separator + 1)});
  if (name == "content-type") {
    const auto semicolon = value.find(';');
    response.content_type = lower_ascii(trim_ascii(value.substr(0, semicolon)));
  } else if (name == "x-request-id" && value.size() <= 256) {
    response.request_id = value;
  } else if (name == "retry-after") {
    response.retry_after = bounded_retry_after(value);
  }
  return bytes;
}

int cancel_transfer(void *context, curl_off_t, curl_off_t, curl_off_t,
                    curl_off_t) {
  return static_cast<const std::stop_token *>(context)->stop_requested() ? 1
                                                                         : 0;
}

[[nodiscard]] bool accepted_content_type(const std::string_view type) {
  return type == "audio/wav" || type == "audio/x-wav" ||
         type == "application/octet-stream";
}

} // namespace

CurlTtsHttpTransport::CurlTtsHttpTransport() {
  static_cast<void>(curl_global());
}

TtsHttpResponse
CurlTtsHttpTransport::post(const TtsHttpRequest &request,
                           const std::stop_token stop_token) const {
  if (stop_token.stop_requested())
    throw TtsError{TtsFailureCategory::cancelled, "TTS request was cancelled."};
  if (request.url != speech_url || request.authorization.empty() ||
      request.json_body.empty() || request.maximum_body_bytes == 0)
    throw TtsError{TtsFailureCategory::invalid_request,
                   "TTS HTTP request configuration is invalid."};

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{
      curl_easy_init(), &curl_easy_cleanup};
  if (!handle)
    throw TtsError{TtsFailureCategory::unavailable,
                   "Unable to create a TTS HTTP request."};
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{
      nullptr, &curl_slist_free_all};
  const auto append_header = [&headers](const std::string &header) {
    auto *updated = curl_slist_append(headers.get(), header.c_str());
    if (updated == nullptr)
      throw TtsError{TtsFailureCategory::unavailable,
                     "Unable to allocate TTS HTTP headers."};
    static_cast<void>(headers.release());
    headers.reset(updated);
  };
  append_header("Content-Type: application/json");
  append_header("Accept: audio/wav");
  append_header(request.authorization);

  CurlResponseBuffer buffer{.response = {},
                            .maximum_body_bytes = request.maximum_body_bytes,
                            .oversized = false};
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, request.json_body.data());
  curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request.json_body.size()));
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_header);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &buffer);
  curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, cancel_transfer);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &stop_token);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(request.connect_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(request.total_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "sanguinius-discord-bot/2");

  const auto result = curl_easy_perform(handle.get());
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE,
                    &buffer.response.status);
  if (result == CURLE_ABORTED_BY_CALLBACK && stop_token.stop_requested())
    throw TtsError{TtsFailureCategory::cancelled, "TTS request was cancelled."};
  if (buffer.oversized)
    throw TtsError{TtsFailureCategory::oversized_media,
                   "TTS response exceeded the encoded media limit."};
  const auto failed_before_response =
      buffer.response.status == 0 && buffer.response.body.empty();
  if (result == CURLE_OPERATION_TIMEDOUT)
    throw TtsError{TtsFailureCategory::timeout, "TTS request timed out.",
                   failed_before_response};
  if (result != CURLE_OK)
    throw TtsError{TtsFailureCategory::transport,
                   "TTS transport failed before a valid response.",
                   failed_before_response};
  return std::move(buffer.response);
}

OpenAiTtsClient::OpenAiTtsClient(
    std::string api_key, std::shared_ptr<const TtsHttpTransport> transport,
    OpenAiTtsClientConfiguration configuration)
    : api_key_{std::move(api_key)}, transport_{std::move(transport)},
      configuration_{std::move(configuration)} {
  if (api_key_.empty() ||
      configuration_.endpoint != "https://api.openai.com/v1/audio/speech" ||
      configuration_.connect_timeout <= std::chrono::milliseconds::zero() ||
      configuration_.total_timeout <= std::chrono::milliseconds::zero() ||
      configuration_.maximum_body_bytes == 0 ||
      configuration_.maximum_body_bytes > maximum_tts_encoded_bytes)
    throw std::invalid_argument{"OpenAI TTS requires an API key."};
  if (!transport_)
    transport_ = std::make_shared<CurlTtsHttpTransport>();
}

SynthesizedAudio
OpenAiTtsClient::synthesize(const TtsRequest &request,
                            const std::stop_token stop_token) const {
  if (stop_token.stop_requested())
    throw TtsError{TtsFailureCategory::cancelled, "TTS request was cancelled."};
  if (request.provider != "openai" || request.model != "tts-1" ||
      request.voice != "onyx" || request.response_format != AudioFormat::wav ||
      request.speed != 1.0 ||
      request.timeout <= std::chrono::milliseconds::zero())
    throw TtsError{
        TtsFailureCategory::invalid_request,
        "The configured TTS model, voice, or media format is not allowed."};
  const auto normalized = normalize_tts_text(request.text);
  const auto json = nlohmann::json{{"model", request.model},
                                   {"input", normalized.text},
                                   {"voice", request.voice},
                                   {"response_format", "wav"},
                                   {"speed", request.speed}};
  const auto response = transport_->post(
      {.url = configuration_.endpoint,
       .authorization = "Authorization: Bearer " + api_key_,
       .json_body = json.dump(),
       .connect_timeout = configuration_.connect_timeout,
       .total_timeout = std::min(configuration_.total_timeout, request.timeout),
       .maximum_body_bytes = configuration_.maximum_body_bytes},
      stop_token);

  if (response.status < 200 || response.status >= 300) {
    const auto category =
        response.status == 401 || response.status == 403
            ? TtsFailureCategory::authentication
        : response.status == 429 ? TtsFailureCategory::rate_limited
        : response.status == 408 ? TtsFailureCategory::timeout
        : response.status >= 500 ? TtsFailureCategory::provider_unavailable
                                 : TtsFailureCategory::provider_rejected;
    const bool retryable = response.status == 408 || response.status == 429 ||
                           response.status >= 500;
    throw TtsError{category, "The TTS provider request failed.", retryable,
                   response.retry_after, response.request_id};
  }
  if (!accepted_content_type(response.content_type) ||
      !wav_media_signature(response.body)) {
    throw TtsError{TtsFailureCategory::invalid_media,
                   "TTS provider returned media that is not a valid WAV file.",
                   false, std::nullopt, response.request_id};
  }
  if (response.body.empty() || response.body.size() > maximum_tts_encoded_bytes)
    throw TtsError{TtsFailureCategory::oversized_media,
                   "TTS provider returned an invalid media size.", false,
                   std::nullopt, response.request_id};
  return {.bytes = response.body,
          .format = AudioFormat::wav,
          .content_type = response.content_type,
          .provider_request_id = response.request_id};
}

} // namespace sanguinius
