#include "sanguinius/openai_transcription_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace sanguinius {

namespace transcription_http_detail {

MultipartContract multipart_contract(const std::string_view model) noexcept {
  return {.part_count = 2,
          .model_field_name = "model",
          .model_value = model,
          .file_field_name = "file",
          .file_name = "window.wav",
          .file_content_type = "audio/wav"};
}

WavStream::WavStream(const std::span<const std::byte> pcm,
                     const std::uint32_t sample_rate,
                     const std::uint16_t channels,
                     const std::uint16_t bits_per_sample)
    : header_{
          pcm_wav_header(pcm.size(), sample_rate, channels, bits_per_sample)},
      pcm_{pcm} {}

std::size_t WavStream::size() const noexcept {
  return header_.size() + pcm_.size();
}

std::size_t WavStream::read(const std::span<std::byte> destination) noexcept {
  if (destination.empty() || offset_ >= size())
    return 0;
  const auto copied = std::min(destination.size(), size() - offset_);
  const auto header_remaining =
      offset_ < header_.size() ? header_.size() - offset_ : 0U;
  const auto header_copied = std::min(copied, header_remaining);
  if (header_copied != 0)
    std::memcpy(destination.data(), header_.data() + offset_, header_copied);
  if (header_copied < copied) {
    const auto pcm_offset = offset_ + header_copied - header_.size();
    std::memcpy(destination.data() + header_copied, pcm_.data() + pcm_offset,
                copied - header_copied);
  }
  offset_ += copied;
  return copied;
}

bool WavStream::seek(const std::size_t offset) noexcept {
  if (offset > size())
    return false;
  offset_ = offset;
  return true;
}

} // namespace transcription_http_detail

namespace {

class CurlGlobal final {
public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error{
          "Unable to initialize libcurl for transcription."};
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

[[nodiscard]] CurlGlobal &curl_global() {
  static CurlGlobal instance;
  return instance;
}

struct ResponseBuffer {
  std::string body;
  std::string request_id;
  std::size_t maximum{};
  bool oversized{};
};

struct TransferProgress {
  const std::stop_token *stop_token{};
  const std::function<void()> *transmission_started{};
  bool transmission_observed{};
};

void note_transmission(TransferProgress &progress) noexcept {
  if (progress.transmission_observed)
    return;
  progress.transmission_observed = true;
  try {
    if (progress.transmission_started && *progress.transmission_started)
      (*progress.transmission_started)();
  } catch (...) {
  }
}

std::size_t read_wav(char *destination, const std::size_t size,
                     const std::size_t count, void *context) {
  if (size == 0 || count == 0 || context == nullptr)
    return 0;
  if (count > std::numeric_limits<std::size_t>::max() / size)
    return CURL_READFUNC_ABORT;
  auto &stream = *static_cast<transcription_http_detail::WavStream *>(context);
  return stream.read(
      {reinterpret_cast<std::byte *>(destination), size * count});
}

int seek_wav(void *context, const curl_off_t offset, const int origin) {
  if (context == nullptr || origin != SEEK_SET || offset < 0)
    return CURL_SEEKFUNC_CANTSEEK;
  auto &stream = *static_cast<transcription_http_detail::WavStream *>(context);
  const auto requested = static_cast<std::uint64_t>(offset);
  if (requested > stream.size())
    return CURL_SEEKFUNC_FAIL;
  return stream.seek(static_cast<std::size_t>(requested)) ? CURL_SEEKFUNC_OK
                                                          : CURL_SEEKFUNC_FAIL;
}

std::size_t append_response(char *data, const std::size_t size,
                            const std::size_t count, void *context) {
  if (context == nullptr || size == 0 || count == 0)
    return 0;
  if (count > std::numeric_limits<std::size_t>::max() / size)
    return 0;
  auto &response = *static_cast<ResponseBuffer *>(context);
  const auto bytes = size * count;
  if (bytes > response.maximum ||
      response.body.size() > response.maximum - bytes) {
    response.oversized = true;
    return 0;
  }
  response.body.append(data, bytes);
  return bytes;
}

std::size_t capture_header(char *data, const std::size_t size,
                           const std::size_t count, void *context) {
  if (context == nullptr || size == 0 || count == 0)
    return 0;
  if (count > std::numeric_limits<std::size_t>::max() / size)
    return 0;
  const auto bytes = size * count;
  std::string_view line{data, bytes};
  constexpr std::string_view prefix{"x-request-id:"};
  if (line.size() >= prefix.size() &&
      std::equal(prefix.begin(), prefix.end(), line.begin(),
                 [](const char left, const char right) {
                   return static_cast<char>(std::tolower(
                              static_cast<unsigned char>(right))) == left;
                 })) {
    line.remove_prefix(prefix.size());
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line.remove_prefix(1);
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
      line.remove_suffix(1);
    static_cast<ResponseBuffer *>(context)->request_id =
        sanitize_transcription_request_id(line);
  }
  return bytes;
}

int monitor_transfer(void *context, curl_off_t, curl_off_t,
                     const curl_off_t upload_total,
                     const curl_off_t upload_now) {
  if (context == nullptr)
    return 1;
  auto &progress = *static_cast<TransferProgress *>(context);
  if (upload_total > 0 && upload_now > 0)
    note_transmission(progress);
  return progress.stop_token && progress.stop_token->stop_requested() ? 1 : 0;
}

[[nodiscard]] TranscriptionFailureCategory http_category(const long status) {
  if (status == 408)
    return TranscriptionFailureCategory::timeout;
  if (status == 429)
    return TranscriptionFailureCategory::rate_limited;
  if (status >= 500)
    return TranscriptionFailureCategory::provider_unavailable;
  return TranscriptionFailureCategory::provider_rejected;
}

} // namespace

TranscriptionHttpResponse CurlTranscriptionHttpTransport::post_wav_multipart(
    const TranscriptionHttpRequest &request, const std::stop_token stop_token,
    const std::function<void()> &transmission_started) const {
  static_cast<void>(curl_global());
  if (request.url != transcription_endpoint ||
      request.model != transcription_model || request.authorization.empty() ||
      request.pcm.empty() || request.maximum_response_bytes == 0)
    throw TranscriptionError{TranscriptionFailureCategory::invalid_request,
                             "Invalid transcription HTTP request."};

  const auto contract =
      transcription_http_detail::multipart_contract(request.model);
  transcription_http_detail::WavStream stream{request.pcm, request.sample_rate,
                                              request.channels,
                                              request.bits_per_sample};
  const auto body_size = stream.size();
  if (body_size >
      static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max()))
    throw TranscriptionError{TranscriptionFailureCategory::invalid_request,
                             "Transcription audio is too large."};

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{
      curl_easy_init(), &curl_easy_cleanup};
  if (!handle)
    throw TranscriptionError{TranscriptionFailureCategory::transport,
                             "Unable to create a transcription request."};
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{
      nullptr, &curl_slist_free_all};
  auto *updated =
      curl_slist_append(headers.get(), request.authorization.c_str());
  if (updated == nullptr)
    throw TranscriptionError{TranscriptionFailureCategory::transport,
                             "Unable to allocate transcription headers."};
  headers.release();
  headers.reset(updated);
  std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime{
      curl_mime_init(handle.get()), &curl_mime_free};
  if (!mime)
    throw TranscriptionError{
        TranscriptionFailureCategory::transport,
        "Unable to allocate transcription multipart data."};
  auto *model_part = curl_mime_addpart(mime.get());
  auto *file_part = curl_mime_addpart(mime.get());
  if (contract.part_count != 2 || model_part == nullptr ||
      file_part == nullptr ||
      curl_mime_name(model_part, contract.model_field_name.data()) !=
          CURLE_OK ||
      curl_mime_data(model_part, contract.model_value.data(),
                     contract.model_value.size()) != CURLE_OK ||
      curl_mime_name(file_part, contract.file_field_name.data()) != CURLE_OK ||
      curl_mime_filename(file_part, contract.file_name.data()) != CURLE_OK ||
      curl_mime_type(file_part, contract.file_content_type.data()) !=
          CURLE_OK ||
      curl_mime_data_cb(file_part, static_cast<curl_off_t>(body_size), read_wav,
                        seek_wav, nullptr, &stream) != CURLE_OK)
    throw TranscriptionError{
        TranscriptionFailureCategory::transport,
        "Unable to configure transcription multipart data."};

  ResponseBuffer response{.body = {},
                          .request_id = {},
                          .maximum = request.maximum_response_bytes,
                          .oversized = false};
  TransferProgress progress{.stop_token = &stop_token,
                            .transmission_started = &transmission_started,
                            .transmission_observed = false};
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  curl_easy_setopt(handle.get(), CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(handle.get(), CURLOPT_MIMEPOST, mime.get());
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_header);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, monitor_transfer);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(request.connect_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(request.total_timeout.count()));
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "sanguinius-discord-bot/3");

  if (stop_token.stop_requested())
    throw TranscriptionError{TranscriptionFailureCategory::cancelled,
                             "Transcription was cancelled."};
  const auto result = curl_easy_perform(handle.get());
  long status{};
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
  if (status != 0)
    note_transmission(progress);
  if (response.oversized)
    throw TranscriptionError{TranscriptionFailureCategory::oversized_response,
                             "Transcription response exceeded its size limit.",
                             response.request_id};
  if (result != CURLE_OK) {
    const auto category = stop_token.stop_requested()
                              ? TranscriptionFailureCategory::cancelled
                          : result == CURLE_OPERATION_TIMEDOUT
                              ? TranscriptionFailureCategory::timeout
                              : TranscriptionFailureCategory::transport;
    throw TranscriptionError{category, "Transcription transport failed.",
                             response.request_id};
  }
  return {.status = status,
          .body = std::move(response.body),
          .request_id = std::move(response.request_id)};
}

OpenAiTranscriptionClient::OpenAiTranscriptionClient(
    std::string api_key, std::shared_ptr<TranscriptionHttpTransport> transport,
    OpenAiTranscriptionClientConfiguration configuration)
    : api_key_{std::move(api_key)}, transport_{std::move(transport)},
      configuration_{std::move(configuration)} {
  if (api_key_.empty())
    throw std::invalid_argument{"OpenAI transcription requires an API key."};
  if (configuration_.endpoint != transcription_endpoint ||
      configuration_.model != transcription_model ||
      configuration_.connect_timeout.count() < 1 ||
      configuration_.total_timeout.count() < 1 ||
      configuration_.maximum_response_bytes < maximum_transcript_bytes)
    throw std::invalid_argument{
        "OpenAI transcription fixed configuration is invalid."};
  configuration_.connect_timeout =
      std::min(configuration_.connect_timeout, configuration_.total_timeout);
  if (!transport_)
    transport_ = std::make_shared<CurlTranscriptionHttpTransport>();
}

Transcript OpenAiTranscriptionClient::transcribe(
    const TranscriptionRequest &request, const std::stop_token stop_token,
    const std::function<void()> &transmission_started) const {
  if (request.pcm.empty() ||
      request.pcm.size() > maximum_transcription_pcm_bytes ||
      request.pcm.size() % 4U != 0 || request.sample_rate != 48'000 ||
      request.channels != 2 || request.bits_per_sample != 16 ||
      request.timeout.count() < 1)
    throw TranscriptionError{TranscriptionFailureCategory::invalid_request,
                             "Invalid transcription audio."};
  if (stop_token.stop_requested())
    throw TranscriptionError{TranscriptionFailureCategory::cancelled,
                             "Transcription was cancelled."};
  const auto response = transport_->post_wav_multipart(
      {.url = configuration_.endpoint,
       .authorization = "Authorization: Bearer " + api_key_,
       .model = configuration_.model,
       .pcm = request.pcm,
       .sample_rate = request.sample_rate,
       .channels = request.channels,
       .bits_per_sample = request.bits_per_sample,
       .connect_timeout = configuration_.connect_timeout,
       .total_timeout = std::min(configuration_.total_timeout, request.timeout),
       .maximum_response_bytes = configuration_.maximum_response_bytes},
      stop_token, transmission_started);
  auto request_id = sanitize_transcription_request_id(response.request_id);
  if (response.status < 200 || response.status >= 300)
    throw TranscriptionError{http_category(response.status),
                             "Transcription provider rejected the request.",
                             std::move(request_id)};
  return parse_transcription_response(response.body, std::move(request_id));
}

Transcript parse_transcription_response(std::string_view body,
                                        std::string provider_request_id) {
  try {
    const auto json = nlohmann::json::parse(body);
    if (!json.is_object() || !json.contains("text") ||
        !json.at("text").is_string())
      throw TranscriptionError{TranscriptionFailureCategory::invalid_response,
                               "Transcription provider response omitted text.",
                               std::move(provider_request_id)};
    auto text = json.at("text").get<std::string>();
    if (!valid_transcript_text(text))
      throw TranscriptionError{TranscriptionFailureCategory::invalid_response,
                               "Transcription provider returned invalid text.",
                               std::move(provider_request_id)};
    return {.text = std::move(text),
            .provider_request_id = std::move(provider_request_id)};
  } catch (const TranscriptionError &) {
    throw;
  } catch (...) {
    throw TranscriptionError{TranscriptionFailureCategory::invalid_response,
                             "Unable to parse transcription response.",
                             std::move(provider_request_id)};
  }
}

} // namespace sanguinius
