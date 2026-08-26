#include "sanguinius/tts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("TTS text normalization is strict and Unicode scalar bounded",
          "[tts][text]") {
  const auto normalized =
      sanguinius::normalize_tts_text("  The\tvox\r\n is  open. \xE2\x98\x85  ");
  REQUIRE(normalized.text == "The vox is open. \xE2\x98\x85");
  REQUIRE(normalized.scalar_count == 18);

  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text(" \t\r\n "),
                    sanguinius::TtsError);
  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text(std::string{"a\0b", 3}),
                    sanguinius::TtsError);
  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text("\xC0\xAF"),
                    sanguinius::TtsError);
  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text("\xED\xA0\x80"),
                    sanguinius::TtsError);
  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text("a\xC2\x85z"),
                    sanguinius::TtsError);
  REQUIRE_THROWS_AS(sanguinius::normalize_tts_text(std::string(351, 'a')),
                    sanguinius::TtsError);
}

TEST_CASE("TTS budget estimation and cache identity are deterministic",
          "[tts][budget][cache]") {
  const auto text = sanguinius::normalize_tts_text("The vox is open.");
  REQUIRE(sanguinius::estimated_tts_cost_micro_usd(text.scalar_count) == 240);
  const sanguinius::TtsRequest request{.text = text.text};
  const auto first = sanguinius::tts_cache_key(text, request);
  const auto second = sanguinius::tts_cache_key(text, request);
  REQUIRE(first == second);
  REQUIRE(first.size() == 64);

  auto changed = request;
  changed.voice = "another";
  REQUIRE(sanguinius::tts_cache_key(text, changed) != first);
  changed = request;
  changed.provider = "another-provider";
  REQUIRE(sanguinius::tts_cache_key(text, changed) != first);
}

TEST_CASE("TTS media signature rejects superficial and truncated bodies",
          "[tts][media]") {
  const std::string wav{"RIFF1234WAVE"};
  REQUIRE(sanguinius::wav_media_signature(
      std::as_bytes(std::span{wav.data(), wav.size()})));
  const std::string not_wav{"RIFF1234JSON"};
  REQUIRE_FALSE(sanguinius::wav_media_signature(
      std::as_bytes(std::span{not_wav.data(), not_wav.size()})));
}
