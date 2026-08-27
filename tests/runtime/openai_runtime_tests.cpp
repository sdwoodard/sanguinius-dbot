#include "sanguinius/openai_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("OpenAI response accumulation is bounded before JSON parsing",
          "[runtime][openai][response-limit]") {
  std::string response(
      sanguinius::openai_client_detail::maximum_response_bytes - 4, 'a');
  REQUIRE(sanguinius::openai_client_detail::append_bounded_response(
      response, "1234",
      sanguinius::openai_client_detail::maximum_response_bytes));
  REQUIRE(response.size() ==
          sanguinius::openai_client_detail::maximum_response_bytes);
  REQUIRE_FALSE(sanguinius::openai_client_detail::append_bounded_response(
      response, "5", sanguinius::openai_client_detail::maximum_response_bytes));
  REQUIRE(response.size() ==
          sanguinius::openai_client_detail::maximum_response_bytes);
}

TEST_CASE("OpenAI request identifiers are captured from safe response headers",
          "[runtime][openai][request-id]") {
  REQUIRE(sanguinius::openai_client_detail::provider_request_id_from_header(
              "X-Request-ID: req_123-safe.4\r\n") == "req_123-safe.4");
  REQUIRE(sanguinius::openai_client_detail::provider_request_id_from_header(
              "x-request-id:\trequest-5\n") == "request-5");
  REQUIRE(sanguinius::openai_client_detail::provider_request_id_from_header(
              "authorization: secret\r\n")
              .empty());
  REQUIRE(sanguinius::openai_client_detail::provider_request_id_from_header(
              "x-request-id: unsafe value\r\n")
              .empty());
}
