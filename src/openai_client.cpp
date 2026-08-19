#include "sanguinius/openai_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <array>
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

size_t append_response(const char *data, const size_t size, const size_t count,
                       void *destination) {
  const auto bytes = size * count;
  static_cast<std::string *>(destination)->append(data, bytes);
  return bytes;
}

int cancel_transfer(void *context, curl_off_t, curl_off_t, curl_off_t,
                    curl_off_t) {
  const auto *stop_token = static_cast<const std::stop_token *>(context);
  return stop_token->stop_requested() ? 1 : 0;
}

[[nodiscard]] std::string api_error(const std::string &body,
                                    const long status) {
  try {
    const auto response = nlohmann::json::parse(body);
    if (response.contains("error") && response["error"].contains("message")) {
      return response["error"]["message"].get<std::string>();
    }
  } catch (const nlohmann::json::exception &) {
  }
  return "OpenAI returned HTTP status " + std::to_string(status) + '.';
}

[[nodiscard]] std::string response_text(const std::string &body) {
  const auto response = nlohmann::json::parse(body);
  std::string text;

  if (response.value("status", "") == "incomplete") {
    throw AiIncompleteResponse{};
  }

  if (response.contains("output_text") && response["output_text"].is_string()) {
    return response["output_text"].get<std::string>();
  }

  for (const auto &item : response.value("output", nlohmann::json::array())) {
    if (item.value("type", "") != "message") {
      continue;
    }
    for (const auto &content : item.value("content", nlohmann::json::array())) {
      if (content.value("type", "") == "refusal") {
        throw AiRefusal{};
      }
      if (content.value("type", "") == "output_text") {
        text += content.value("text", "");
      }
    }
  }

  if (text.empty()) {
    throw std::runtime_error{"OpenAI returned no text response."};
  }
  return text;
}

} // namespace

OpenAiClient::OpenAiClient(std::string api_key, std::string model)
    : api_key_{std::move(api_key)}, model_{std::move(model)} {
  static_cast<void>(curl_global());
}

std::string OpenAiClient::generate(const AiRequest &ai_request,
                                   const std::stop_token stop_token) const {
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
  std::string response_body;
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
  curl_easy_setopt(request_handle.get(), CURLOPT_WRITEDATA, &response_body);
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

  const CURLcode result = curl_easy_perform(request_handle.get());
  long status = 0;
  curl_easy_getinfo(request_handle.get(), CURLINFO_RESPONSE_CODE, &status);

  if (result == CURLE_ABORTED_BY_CALLBACK && stop_token.stop_requested()) {
    throw OperationCancelled{};
  }
  if (result != CURLE_OK) {
    const std::string detail = error_buffer[0] == '\0'
                                   ? curl_easy_strerror(result)
                                   : error_buffer.data();
    throw std::runtime_error{"OpenAI request failed: " + detail};
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error{api_error(response_body, status)};
  }

  try {
    return response_text(response_body);
  } catch (const nlohmann::json::exception &error) {
    throw std::runtime_error{"Unable to parse the OpenAI response: " +
                             std::string{error.what()}};
  }
}

} // namespace sanguinius
