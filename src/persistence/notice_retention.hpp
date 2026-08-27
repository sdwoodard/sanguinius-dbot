#pragma once

#include "sanguinius/tts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace sanguinius::persistence::detail {

inline constexpr std::string_view retained_notice_title{
    "Expired sealed notice"};
inline constexpr std::string_view retained_notice_body{
    "Content removed by retention."};
inline constexpr std::string_view retained_notice_version_field{
    "retention_tombstone_version"};
inline constexpr std::string_view retained_notice_fingerprint_field{
    "retained_content_sha256"};

[[nodiscard]] inline bool valid_sha256(const std::string_view value) noexcept {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] inline nlohmann::json
notice_content_projection(const nlohmann::json &payload) {
  return nlohmann::json{{"title", payload.at("title")},
                        {"body", payload.at("body")},
                        {"actions", payload.at("actions")}};
}

[[nodiscard]] inline std::string
notice_content_fingerprint(const nlohmann::json &payload) {
  const auto serialized = notice_content_projection(payload).dump();
  return sha256_hex(
      std::as_bytes(std::span{serialized.data(), serialized.size()}));
}

[[nodiscard]] inline bool
is_retained_notice_tombstone(const nlohmann::json &payload) noexcept {
  try {
    return payload.is_object() &&
           payload.value(std::string{retained_notice_version_field}, 0) == 1 &&
           payload.contains(retained_notice_fingerprint_field) &&
           payload.at(retained_notice_fingerprint_field).is_string() &&
           valid_sha256(payload.at(retained_notice_fingerprint_field)
                            .get_ref<const std::string &>());
  } catch (...) {
    return false;
  }
}

[[nodiscard]] inline nlohmann::json
retained_notice_tombstone(const nlohmann::json &payload) {
  if (is_retained_notice_tombstone(payload))
    return payload;
  auto result = payload;
  result[retained_notice_version_field] = 1;
  result[retained_notice_fingerprint_field] =
      notice_content_fingerprint(payload);
  result["title"] = retained_notice_title;
  result["body"] = retained_notice_body;
  result["actions"] = nlohmann::json::array();
  return result;
}

[[nodiscard]] inline bool
retained_notice_matches(const nlohmann::json &stored,
                        const nlohmann::json &requested) {
  if (!is_retained_notice_tombstone(stored))
    return notice_content_projection(stored) ==
           notice_content_projection(requested);
  return stored.at(retained_notice_fingerprint_field).get<std::string>() ==
         notice_content_fingerprint(requested);
}

inline void normalize_retained_notice_for_replay(nlohmann::json &payload) {
  const auto fingerprint =
      is_retained_notice_tombstone(payload)
          ? payload.at(retained_notice_fingerprint_field).get<std::string>()
          : notice_content_fingerprint(payload);
  payload.erase("title");
  payload.erase("body");
  payload.erase("actions");
  payload[retained_notice_version_field] = 1;
  payload[retained_notice_fingerprint_field] = fingerprint;
}

} // namespace sanguinius::persistence::detail
