#include "sanguinius/openai_transcription_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

class FakeTransport final : public sanguinius::TranscriptionHttpTransport {
public:
  sanguinius::TranscriptionHttpResponse response;
  mutable std::optional<sanguinius::TranscriptionHttpRequest> request;
  mutable std::size_t calls{};
  bool mark_transmitted{true};
  std::optional<sanguinius::TranscriptionFailureCategory> failure;

  sanguinius::TranscriptionHttpResponse post_wav_multipart(
      const sanguinius::TranscriptionHttpRequest &value, std::stop_token,
      const std::function<void()> &transmission_started) const override {
    ++calls;
    request = value;
    if (mark_transmitted && transmission_started)
      transmission_started();
    if (failure)
      throw sanguinius::TranscriptionError{*failure,
                                           "Injected transport failure."};
    return response;
  }
};

} // namespace

TEST_CASE("production transcription multipart has exactly two streamed parts",
          "[voice-input][openai][contract][multipart]") {
  const auto contract =
      sanguinius::transcription_http_detail::multipart_contract(
          "gpt-transcribe");
  REQUIRE(contract.part_count == 2);
  REQUIRE(contract.model_field_name == "model");
  REQUIRE(contract.model_value == "gpt-transcribe");
  REQUIRE(contract.file_field_name == "file");
  REQUIRE(contract.file_name == "window.wav");
  REQUIRE(contract.file_content_type == "audio/wav");

  const std::array<std::byte, 12> pcm{
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
      std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
      std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
  const auto header = sanguinius::pcm_wav_header(pcm.size(), 48'000, 2, 16);
  sanguinius::transcription_http_detail::WavStream stream{pcm, 48'000, 2, 16};
  REQUIRE(stream.size() == header.size() + pcm.size());

  std::vector<std::byte> streamed(stream.size());
  std::array<std::byte, 13> chunk{};
  std::size_t offset{};
  while (const auto copied = stream.read(chunk)) {
    std::copy_n(chunk.begin(), copied,
                streamed.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += copied;
  }
  REQUIRE(offset == streamed.size());
  REQUIRE(std::equal(header.begin(), header.end(), streamed.begin()));
  REQUIRE(std::equal(pcm.begin(), pcm.end(),
                     streamed.begin() +
                         static_cast<std::ptrdiff_t>(header.size())));
  REQUIRE(stream.read(chunk) == 0);
  REQUIRE(stream.seek(header.size()));
  REQUIRE(stream.read(chunk) == pcm.size());
  REQUIRE(std::equal(pcm.begin(), pcm.end(), chunk.begin()));
  REQUIRE_FALSE(stream.seek(stream.size() + 1));
}

TEST_CASE("OpenAI transcription request uses only the fixed file endpoint",
          "[voice-input][openai][contract]") {
  auto transport = std::make_shared<FakeTransport>();
  transport->response = {
      .status = 200,
      .body = R"({"text":"The red star answers.","languages":["en"]})",
      .request_id = "transcription-request"};
  sanguinius::OpenAiTranscriptionClient client{"secret-fixture", transport};
  std::array<std::byte, 192> pcm{};
  const auto result = client.transcribe({.pcm = pcm,
                                         .sample_rate = 48'000,
                                         .channels = 2,
                                         .bits_per_sample = 16,
                                         .timeout = std::chrono::seconds{5}},
                                        std::stop_token{});
  REQUIRE(result.text == "The red star answers.");
  REQUIRE(result.provider_request_id == "transcription-request");
  REQUIRE(transport->request.has_value());
  REQUIRE(transport->request->url ==
          "https://api.openai.com/v1/audio/transcriptions");
  REQUIRE(transport->request->model == "gpt-transcribe");
  REQUIRE(transport->request->authorization ==
          "Authorization: Bearer secret-fixture");
  REQUIRE(transport->request->pcm.data() == pcm.data());
  REQUIRE(transport->request->pcm.size() == pcm.size());
}

TEST_CASE("OpenAI transcription rejects provider and content failures",
          "[voice-input][openai][failure]") {
  auto transport = std::make_shared<FakeTransport>();
  sanguinius::OpenAiTranscriptionClient client{"secret-fixture", transport};
  std::array<std::byte, 192> pcm{};
  const auto request =
      sanguinius::TranscriptionRequest{.pcm = pcm,
                                       .sample_rate = 48'000,
                                       .channels = 2,
                                       .bits_per_sample = 16,
                                       .timeout = std::chrono::seconds{5}};

  transport->response = {.status = 429,
                         .body = R"({"error":{"message":"limited"}})",
                         .request_id = "limited"};
  try {
    static_cast<void>(client.transcribe(request, std::stop_token{}));
    FAIL("expected provider failure");
  } catch (const sanguinius::TranscriptionError &error) {
    REQUIRE(error.category() ==
            sanguinius::TranscriptionFailureCategory::rate_limited);
    REQUIRE(error.provider_request_id() == "limited");
  }

  transport->response = {
      .status = 200, .body = R"({"text":""})", .request_id = "empty"};
  REQUIRE_THROWS_AS(client.transcribe(request, std::stop_token{}),
                    sanguinius::TranscriptionError);

  transport->response = {.status = 200,
                         .body =
                             R"({"text":")" + std::string(1'801, 'a') + R"("})",
                         .request_id = "oversized"};
  REQUIRE_THROWS_AS(client.transcribe(request, std::stop_token{}),
                    sanguinius::TranscriptionError);
  transport->response = {.status = 200,
                         .body =
                             std::string{"{\"text\":\""} + "\xC0\xAF" + "\"}",
                         .request_id = "invalid-utf8"};
  REQUIRE_THROWS_AS(client.transcribe(request, std::stop_token{}),
                    sanguinius::TranscriptionError);
  transport->response = {
      .status = 200, .body = "not-json", .request_id = "invalid"};
  REQUIRE_THROWS_AS(client.transcribe(request, std::stop_token{}),
                    sanguinius::TranscriptionError);

  transport->response = {.status = 200,
                         .body = R"({"text":"Sanitized metadata only."})",
                         .request_id = "hostile\nrequest"};
  REQUIRE(client.transcribe(request, std::stop_token{})
              .provider_request_id.empty());

  const auto calls_before_timeout = transport->calls;
  transport->mark_transmitted = false;
  transport->failure = sanguinius::TranscriptionFailureCategory::timeout;
  REQUIRE_THROWS_AS(client.transcribe(request, std::stop_token{}),
                    sanguinius::TranscriptionError);
  REQUIRE(transport->calls == calls_before_timeout + 1);
}

TEST_CASE("OpenAI transcription honors cancellation before transmission",
          "[voice-input][openai][cancel]") {
  auto transport = std::make_shared<FakeTransport>();
  sanguinius::OpenAiTranscriptionClient client{"secret-fixture", transport};
  std::array<std::byte, 192> pcm{};
  std::stop_source stop;
  stop.request_stop();
  REQUIRE_THROWS_AS(client.transcribe({.pcm = pcm,
                                       .sample_rate = 48'000,
                                       .channels = 2,
                                       .bits_per_sample = 16,
                                       .timeout = std::chrono::seconds{5}},
                                      stop.get_token()),
                    sanguinius::TranscriptionError);
  REQUIRE_FALSE(transport->request.has_value());
}

TEST_CASE("OpenAI transcription clamps connect timeout to total timeout",
          "[voice-input][openai][timeout][config]") {
  auto transport = std::make_shared<FakeTransport>();
  transport->response = {.status = 200,
                         .body = R"({"text":"A bounded request."})",
                         .request_id = "short-timeout"};
  sanguinius::OpenAiTranscriptionClient client{
      "secret-fixture",
      transport,
      {.model = std::string{sanguinius::transcription_model},
       .total_timeout = std::chrono::milliseconds{1}}};
  std::array<std::byte, 192> pcm{};
  REQUIRE(client
              .transcribe({.pcm = pcm,
                           .sample_rate = 48'000,
                           .channels = 2,
                           .bits_per_sample = 16,
                           .timeout = std::chrono::milliseconds{1}},
                          std::stop_token{})
              .text == "A bounded request.");
  REQUIRE(transport->request.has_value());
  REQUIRE(transport->request->connect_timeout == std::chrono::milliseconds{1});
  REQUIRE(transport->request->total_timeout == std::chrono::milliseconds{1});
}
