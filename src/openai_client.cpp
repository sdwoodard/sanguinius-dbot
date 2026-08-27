#include "sanguinius/openai_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::string_view responses_url{"https://api.openai.com/v1/responses"};

class CurlGlobal {
public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error{"Unable to initialize libcurl."};
    }
  }

  ~CurlGlobal() { curl_global_cleanup(); }
};

[[nodiscard]] CurlGlobal &curl_global() {
  static CurlGlobal global;
  return global;
}

struct ResponseBuffer {
  std::string body;
  std::string request_id;
  bool oversized{};
};

size_t append_response(const char *data, const size_t size, const size_t count,
                       void *destination) {
  auto &buffer = *static_cast<ResponseBuffer *>(destination);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    buffer.oversized = true;
    return 0;
  }
  const auto bytes = size * count;
  if (!openai_client_detail::append_bounded_response(
          buffer.body, std::string_view{data, bytes},
          openai_client_detail::maximum_response_bytes)) {
    buffer.oversized = true;
    return 0;
  }
  return bytes;
}

[[nodiscard]] bool ascii_equal_case_insensitive(const std::string_view left,
                                                const std::string_view right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_character = static_cast<unsigned char>(left[index]);
    const auto right_character = static_cast<unsigned char>(right[index]);
    if (std::tolower(left_character) != std::tolower(right_character))
      return false;
  }
  return true;
}

size_t capture_response_header(const char *data, const size_t size,
                               const size_t count, void *destination) {
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
    return 0;
  const auto bytes = size * count;
  const auto request_id = openai_client_detail::provider_request_id_from_header(
      std::string_view{data, bytes});
  if (!request_id.empty()) {
    auto &buffer = *static_cast<ResponseBuffer *>(destination);
    buffer.request_id = request_id;
  }
  return bytes;
}

int cancel_transfer(void *context, curl_off_t, curl_off_t, curl_off_t,
                    curl_off_t) {
  const auto *stop_token = static_cast<const std::stop_token *>(context);
  return stop_token->stop_requested() ? 1 : 0;
}

[[nodiscard]] AiResult
response_result(const std::string &body,
                const std::string_view header_request_id) {
  const auto response = nlohmann::json::parse(body);
  std::string text;
  auto request_id = sanitize_ai_provider_request_id(header_request_id);
  if (request_id.empty())
    request_id =
        sanitize_ai_provider_request_id(response.value("id", std::string{}));

  if (response.value("status", "") == "incomplete") {
    throw AiIncompleteResponse{request_id};
  }

  if (response.contains("output_text") && response["output_text"].is_string()) {
    text = response["output_text"].get<std::string>();
  }
  if (text.empty()) {
    for (const auto &item : response.value("output", nlohmann::json::array())) {
      if (item.value("type", "") != "message")
        continue;
      for (const auto &content :
           item.value("content", nlohmann::json::array())) {
        if (content.value("type", "") == "refusal")
          throw AiRefusal{request_id};
        if (content.value("type", "") == "output_text")
          text += content.value("text", "");
      }
    }
  }
  if (text.empty())
    throw std::runtime_error{"OpenAI returned no text response."};
  const auto usage = response.value("usage", nlohmann::json::object());
  return AiResult{
      .text = std::move(text),
      .provider_request_id = std::move(request_id),
      .input_tokens = usage.value("input_tokens", std::size_t{}),
      .output_tokens = usage.value("output_tokens", std::size_t{}),
  };
}

} // namespace

bool openai_client_detail::append_bounded_response(
    std::string &destination, const std::string_view chunk,
    const std::size_t maximum_bytes) {
  if (destination.size() > maximum_bytes ||
      chunk.size() > maximum_bytes - destination.size())
    return false;
  destination.append(chunk);
  return true;
}

std::string openai_client_detail::provider_request_id_from_header(
    const std::string_view header_line) {
  const auto separator = header_line.find(':');
  if (separator == std::string_view::npos ||
      !ascii_equal_case_insensitive(header_line.substr(0, separator),
                                    "x-request-id"))
    return {};
  auto value = header_line.substr(separator + 1);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    value.remove_prefix(1);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                            value.back() == ' ' || value.back() == '\t'))
    value.remove_suffix(1);
  return sanitize_ai_provider_request_id(value);
}

OpenAiClient::OpenAiClient(std::string api_key, std::string model)
    : api_key_{std::move(api_key)}, model_{std::move(model)} {
  static_cast<void>(curl_global());
}

AiResult OpenAiClient::generate(
    const AiRequest &ai_request, const std::stop_token stop_token,
    const std::function<void()> &transmission_started) const {
  if (stop_token.stop_requested()) {
    throw OperationCancelled{};
  }

  nlohmann::json input = nlohmann::json::array();
  for (const auto &[role, content] : ai_request.conversation) {
    input.push_back({{"role", role}, {"content", content}});
  }

  nlohmann::json request = {
      {"model", model_},
      {"instructions", ai_request.instructions},
      {"input", std::move(input)},
      {"max_output_tokens", ai_request.max_output_tokens},
      {"store", false},
  };
  if (ai_request.json_schema) {
    request["text"]["format"] = {
        {"type", "json_schema"},
        {"name", ai_request.json_schema->name},
        {"schema", nlohmann::json::parse(ai_request.json_schema->schema)},
        {"strict", ai_request.json_schema->strict},
    };
  }
  const std::string request_body = request.dump();
  ResponseBuffer response;
  std::array<char, CURL_ERROR_SIZE> error_buffer{};

  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request_handle{
      curl_easy_init(), &curl_easy_cleanup};
  if (!request_handle) {
    throw std::runtime_error{"Unable to create an OpenAI HTTP request."};
  }

  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{
      nullptr, &curl_slist_free_all};
  const auto add_header = [&headers](const char *header) {
    curl_slist *updated = curl_slist_append(headers.get(), header);
    if (updated == nullptr) {
      throw std::runtime_error{"Unable to allocate OpenAI HTTP headers."};
    }
    static_cast<void>(headers.release());
    headers.reset(updated);
  };
  add_header("Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key_;
  add_header(authorization.c_str());

  curl_easy_setopt(request_handle.get(), CURLOPT_URL, responses_url.data());
  curl_easy_setopt(request_handle.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(request_handle.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(request_handle.get(), CURLOPT_POSTFIELDS,
                   request_body.data());
  curl_easy_setopt(request_handle.get(), CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request_body.size()));
  curl_easy_setopt(request_handle.get(), CURLOPT_WRITEFUNCTION,
                   append_response);
  curl_easy_setopt(request_handle.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(request_handle.get(), CURLOPT_HEADERFUNCTION,
                   capture_response_header);
  curl_easy_setopt(request_handle.get(), CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(request_handle.get(), CURLOPT_ERRORBUFFER,
                   error_buffer.data());
  curl_easy_setopt(request_handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(request_handle.get(), CURLOPT_XFERINFOFUNCTION,
                   cancel_transfer);
  curl_easy_setopt(request_handle.get(), CURLOPT_XFERINFODATA, &stop_token);
  curl_easy_setopt(request_handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(request_handle.get(), CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(request_handle.get(), CURLOPT_USERAGENT,
                   "sanguinius-discord-bot/2");

  if (stop_token.stop_requested())
    throw OperationCancelled{};
  if (transmission_started)
    transmission_started();
  const CURLcode result = curl_easy_perform(request_handle.get());
  long status = 0;
  curl_easy_getinfo(request_handle.get(), CURLINFO_RESPONSE_CODE, &status);

  if (result == CURLE_ABORTED_BY_CALLBACK && stop_token.stop_requested()) {
    throw OperationCancelled{};
  }
  if (response.oversized)
    throw AiProviderError{AiProviderErrorCategory::invalid_response,
                          response.request_id};
  if (result != CURLE_OK)
    throw AiProviderError{result == CURLE_OPERATION_TIMEDOUT
                              ? AiProviderErrorCategory::timeout
                              : AiProviderErrorCategory::transport,
                          response.request_id};
  if (status < 200 || status >= 300) {
    throw AiProviderError{
        status == 401 || status == 403 ? AiProviderErrorCategory::authentication
        : status == 429                ? AiProviderErrorCategory::rate_limited
        : status >= 500                ? AiProviderErrorCategory::server
                        : AiProviderErrorCategory::invalid_request,
        response.request_id};
  }

  try {
    return response_result(response.body, response.request_id);
  } catch (const AiRefusal &) {
    throw;
  } catch (const AiIncompleteResponse &) {
    throw;
  } catch (...) {
    throw AiProviderError{AiProviderErrorCategory::invalid_response,
                          response.request_id};
  }
}

} // namespace sanguinius
