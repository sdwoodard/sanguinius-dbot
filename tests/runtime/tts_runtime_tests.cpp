#include "sanguinius/ffmpeg_audio_normalizer.hpp"
#include "sanguinius/openai_tts_client.hpp"
#include "sanguinius/composition_root.hpp"
#include "sanguinius/static_speech_assets.hpp"
#include "sanguinius/tts_cache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

class FakeTransport final : public sanguinius::TtsHttpTransport {
public:
  sanguinius::TtsHttpResponse response;
  mutable std::optional<sanguinius::TtsHttpRequest> request;

  sanguinius::TtsHttpResponse
  post(const sanguinius::TtsHttpRequest &value,
       std::stop_token) const override {
    request = value;
    return response;
  }
};

void append_u16(std::vector<std::byte> &bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xFFU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte> &bytes, const std::uint32_t value) {
  append_u16(bytes, static_cast<std::uint16_t>(value & 0xFFFFU));
  append_u16(bytes, static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
}

void append_text(std::vector<std::byte> &bytes, const std::string_view value) {
  for (const auto character : value)
    bytes.push_back(static_cast<std::byte>(character));
}

std::vector<std::byte> mono_wav() {
  constexpr std::uint32_t sample_rate = 24'000;
  constexpr std::uint32_t frames = 2'400;
  constexpr std::uint32_t data_bytes = frames * 2;
  std::vector<std::byte> bytes;
  append_text(bytes, "RIFF");
  append_u32(bytes, 36 + data_bytes);
  append_text(bytes, "WAVEfmt ");
  append_u32(bytes, 16);
  append_u16(bytes, 1);
  append_u16(bytes, 1);
  append_u32(bytes, sample_rate);
  append_u32(bytes, sample_rate * 2);
  append_u16(bytes, 2);
  append_u16(bytes, 16);
  append_text(bytes, "data");
  append_u32(bytes, data_bytes);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto phase = static_cast<double>(frame) / sample_rate;
    const auto sample = static_cast<std::int16_t>(
        std::sin(phase * 440.0 * 6.283185307179586) * 2'000.0);
    append_u16(bytes, static_cast<std::uint16_t>(sample));
  }
  return bytes;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array pattern{'/', 't', 'm', 'p', '/', 's', 'a', 'n', 'g', 'u', 'i',
                       'n', 'i', 'u', 's', '-', 't', 't', 's', '-', 'X', 'X',
                       'X', 'X', 'X', 'X', '\0'};
    const auto *created = ::mkdtemp(pattern.data());
    if (created == nullptr)
      throw std::runtime_error{"mkdtemp failed"};
    path_ = created;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void write_executable(const std::filesystem::path &path,
                      const std::string_view contents) {
  {
    std::ofstream output{path};
    output << contents;
  }
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0)
    throw std::runtime_error{"chmod failed"};
}

} // namespace

TEST_CASE("OpenAI TTS request is exact and does not send instructions",
          "[tts][openai][contract]") {
  auto transport = std::make_shared<FakeTransport>();
  transport->response = {.status = 200,
                         .content_type = "audio/wav",
                         .request_id = "request-fixture",
                         .retry_after = std::nullopt,
                         .body = mono_wav()};
  sanguinius::OpenAiTtsClient client{"secret-fixture", transport};
  const auto result = client.synthesize(
      {.text = "  The vox\n is open.  "}, std::stop_token{});
  REQUIRE(result.provider_request_id == "request-fixture");
  REQUIRE(transport->request.has_value());
  const auto json = nlohmann::json::parse(transport->request->json_body);
  REQUIRE(json.at("model") == "tts-1");
  REQUIRE(json.at("voice") == "onyx");
  REQUIRE(json.at("input") == "The vox is open.");
  REQUIRE(json.at("response_format") == "wav");
  REQUIRE_FALSE(json.contains("instructions"));
  REQUIRE(transport->request->authorization ==
          "Authorization: Bearer secret-fixture");
}

TEST_CASE("OpenAI TTS classifies provider and media failures",
          "[tts][openai][failure]") {
  auto transport = std::make_shared<FakeTransport>();
  sanguinius::OpenAiTtsClient client{"secret-fixture", transport};
  transport->response = {.status = 429,
                         .content_type = "application/json",
                         .request_id = "limited",
                         .retry_after = std::chrono::seconds{2},
                         .body = {}};
  try {
    static_cast<void>(client.synthesize({.text = "Line"}, std::stop_token{}));
    FAIL("expected provider failure");
  } catch (const sanguinius::TtsError &error) {
    REQUIRE(error.category() == sanguinius::TtsFailureCategory::rate_limited);
    REQUIRE(error.retryable());
    REQUIRE(error.provider_request_id() == "limited");
  }

  const std::string invalid{"{\"error\":true}"};
  transport->response = {
      .status = 200,
      .content_type = "application/json",
      .request_id = {},
      .retry_after = std::nullopt,
      .body = {reinterpret_cast<const std::byte *>(invalid.data()),
               reinterpret_cast<const std::byte *>(invalid.data() +
                                                    invalid.size())}};
  REQUIRE_THROWS_AS(
      client.synthesize({.text = "Line"}, std::stop_token{}),
      sanguinius::TtsError);
}

TEST_CASE("OpenAI Retry-After seconds are bounded instead of discarded",
          "[tts][openai][retry]") {
  REQUIRE(sanguinius::bounded_retry_after("0") == std::chrono::seconds{0});
  REQUIRE(sanguinius::bounded_retry_after("3") == std::chrono::seconds{3});
  REQUIRE(sanguinius::bounded_retry_after("99") == std::chrono::seconds{5});
  REQUIRE_FALSE(sanguinius::bounded_retry_after("-1").has_value());
  REQUIRE_FALSE(sanguinius::bounded_retry_after("tomorrow").has_value());
}

TEST_CASE("FFmpeg normalizes bounded WAV to exact D++ PCM",
          "[tts][ffmpeg]") {
  sanguinius::FfmpegAudioNormalizer normalizer{"/usr/bin/ffprobe",
                                                "/usr/bin/ffmpeg"};
  const auto normalized = normalizer.normalize(
      {.bytes = mono_wav(),
       .format = sanguinius::AudioFormat::wav,
       .content_type = "audio/wav",
       .provider_request_id = {}},
      {}, std::stop_token{});
  REQUIRE(normalized.pcm.sample_rate == 48'000);
  REQUIRE(normalized.pcm.channels == 2);
  REQUIRE(normalized.pcm.bits_per_sample == 16);
  REQUIRE(normalized.pcm.samples.size() % 2 == 0);
  REQUIRE(normalized.pcm.samples.size() >= 9'500);
  REQUIRE(normalized.pcm.samples.size() <= 9'700);
  REQUIRE(normalized.duration_ms >= 99);
  REQUIRE(normalized.duration_ms <= 101);
}

TEST_CASE("FFmpeg runtime validation pins the tested major version",
          "[tts][ffmpeg][configuration]") {
  REQUIRE_NOTHROW(sanguinius::validate_ffmpeg_runtime(
      "/usr/bin/ffprobe", "/usr/bin/ffmpeg", 9));
  REQUIRE_THROWS(sanguinius::validate_ffmpeg_runtime(
      "/usr/bin/ffprobe", "/usr/bin/ffmpeg", 8));
}

TEST_CASE("FFmpeg wrapper closes inherited descriptors and contains early stdin closure",
          "[tts][ffmpeg][security]") {
  TemporaryDirectory temporary;
  const int base_descriptor = ::open("/dev/null", O_RDONLY);
  REQUIRE(base_descriptor >= 0);
  const int inherited_descriptor = ::fcntl(base_descriptor, F_DUPFD, 200);
  static_cast<void>(::close(base_descriptor));
  REQUIRE(inherited_descriptor >= 200);

  const auto ffprobe = temporary.path() / "ffprobe";
  const auto ffmpeg = temporary.path() / "ffmpeg";
  write_executable(
      ffprobe,
      "#!/bin/sh\nif [ -e /proc/$$/fd/" +
          std::to_string(inherited_descriptor) +
          " ]; then printf '%s' '{\"streams\":[{\"codec_type\":\"audio\"}],"
          "\"format\":{\"duration\":\"0.001\"}}'; else printf '%s' "
          "'{\"streams\":[{\"codec_type\":\"audio\"}],\"format\":{}}'; fi\n");
  write_executable(ffmpeg,
                   "#!/bin/sh\ndd if=/dev/zero bs=192 count=1 2>/dev/null\n");

  std::vector<std::byte> media(sanguinius::maximum_tts_encoded_bytes,
                               std::byte{0});
  const std::string signature{"RIFF0000WAVE"};
  std::copy(signature.begin(), signature.end(),
            reinterpret_cast<char *>(media.data()));
  sanguinius::FfmpegAudioNormalizer normalizer{ffprobe, ffmpeg};
  REQUIRE_THROWS_AS(normalizer.normalize(
                        {.bytes = std::move(media),
                         .format = sanguinius::AudioFormat::wav,
                         .content_type = "audio/wav",
                         .provider_request_id = {}},
                        {}, std::stop_token{}),
                    sanguinius::TtsError);
  static_cast<void>(::close(inherited_descriptor));
}

TEST_CASE("FFmpeg timeout kills and reaps the isolated process group",
          "[tts][ffmpeg][security][timeout]") {
  TemporaryDirectory temporary;
  const auto ffprobe = temporary.path() / "ffprobe";
  const auto ffmpeg = temporary.path() / "ffmpeg";
  const auto descendant_pid_file = temporary.path() / "descendant.pid";
  write_executable(
      ffprobe,
      "#!/bin/sh\n"
      "trap 'exit 0' TERM\n"
      "(trap '' TERM; while :; do sleep 1; done) &\n"
      "printf '%s\\n' \"$!\" > '" + descendant_pid_file.string() +
          "'\n"
          "wait\n");
  write_executable(ffmpeg, "#!/bin/sh\nexit 1\n");
  sanguinius::FfmpegAudioNormalizer normalizer{ffprobe, ffmpeg};
  auto limits = sanguinius::AudioNormalizationLimits{};
  limits.probe_timeout = std::chrono::milliseconds{100};
  REQUIRE_THROWS_AS(
      normalizer.normalize(
          {.bytes = mono_wav(),
           .format = sanguinius::AudioFormat::wav,
           .content_type = "audio/wav",
           .provider_request_id = {}},
          limits, std::stop_token{}),
      sanguinius::TtsError);

  std::ifstream pid_input{descendant_pid_file};
  long descendant_pid{};
  pid_input >> descendant_pid;
  REQUIRE(descendant_pid > 0);
  const auto process_path =
      std::filesystem::path{"/proc"} / std::to_string(descendant_pid);
  for (std::size_t attempt = 0;
       attempt < 200 && std::filesystem::exists(process_path); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  REQUIRE_FALSE(std::filesystem::exists(process_path));
}

TEST_CASE("Filesystem TTS cache verifies checksum and bounds corruption",
          "[tts][cache]") {
  TemporaryDirectory temporary;
  sanguinius::FilesystemTtsCache cache{temporary.path()};
  sanguinius::PcmAudio audio;
  audio.samples.assign(960, 42);
  const auto text = sanguinius::normalize_tts_text("Cache fixture");
  const auto key = sanguinius::tts_cache_key(text, {.text = text.text});
  std::vector<std::byte> bytes;
  bytes.reserve(audio.samples.size() * 2);
  for (const auto sample : audio.samples) {
    const auto bits = static_cast<std::uint16_t>(sample);
    bytes.push_back(static_cast<std::byte>(bits & 0xFFU));
    bytes.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
  }
  const auto checksum = sanguinius::sha256_hex(bytes);
  REQUIRE(cache.write(key, audio).removed_keys.empty());
  REQUIRE(cache.health().entries == 1);
  REQUIRE(cache.health().bytes == bytes.size());
  const auto loaded = cache.read(key, checksum);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->samples == audio.samples);
  REQUIRE(cache.health().hits == 1);
  REQUIRE_FALSE(cache.read(key, std::string(64, '0')).has_value());
  REQUIRE(cache.health().corruptions == 1);

  const auto victim = temporary.path() / "victim";
  {
    std::ofstream output{victim};
    output << "must remain";
  }
  const auto entry = temporary.path() / (key + ".pcm");
  std::filesystem::create_symlink(victim, entry);
  REQUIRE_FALSE(cache.read(key, checksum).has_value());
  REQUIRE_FALSE(std::filesystem::exists(entry));
  REQUIRE(std::filesystem::exists(victim));
}

TEST_CASE("Filesystem TTS cache purges orphans expiry and oldest entries",
          "[tts][cache][purge]") {
  TemporaryDirectory temporary;
  sanguinius::FilesystemTtsCache cache{
      temporary.path(),
      {.maximum_bytes = 4, .maximum_age = std::chrono::hours{24}}};
  sanguinius::PcmAudio audio;
  audio.samples = {1, 1};
  const auto checksum = sanguinius::sha256_hex(
      std::array{std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0}});
  const auto first = sanguinius::tts_cache_key(
      sanguinius::normalize_tts_text("First cache entry"),
      {.text = "First cache entry"});
  const auto second = sanguinius::tts_cache_key(
      sanguinius::normalize_tts_text("Second cache entry"),
      {.text = "Second cache entry"});
  REQUIRE(cache.write(first, audio).removed_keys.empty());
  const auto hard_bound = cache.write(second, audio);
  REQUIRE(hard_bound.removed_keys == std::vector<std::string>{first});
  REQUIRE(cache.health().bytes == 4);
  {
    std::ofstream orphan{temporary.path() /
                         ".sanguinius-tts-tmp-123-456"};
    orphan << "orphan";
  }
  REQUIRE(cache.purge().removed_keys.empty());
  REQUIRE_FALSE(cache.read(first, checksum).has_value());
  REQUIRE(cache.read(second, checksum).has_value());
  REQUIRE_FALSE(
      std::filesystem::exists(temporary.path() /
                              ".sanguinius-tts-tmp-123-456"));

  std::filesystem::last_write_time(
      temporary.path() / (second + ".pcm"),
      std::filesystem::file_time_type::clock::now() - std::chrono::hours{25});
  const auto expired = cache.purge();
  REQUIRE(expired.removed_keys == std::vector<std::string>{second});
  REQUIRE_FALSE(cache.read(second, checksum).has_value());
  REQUIRE(cache.health().entries == 0);
  REQUIRE(cache.health().bytes == 0);
}

TEST_CASE("Filesystem TTS cache refuses unowned directory contents",
          "[tts][cache][security]") {
  TemporaryDirectory temporary;
  const auto unrelated = temporary.path() / "unrelated.db";
  {
    std::ofstream output{unrelated};
    output << "must remain";
  }
  REQUIRE_THROWS_AS(sanguinius::FilesystemTtsCache{temporary.path()},
                    sanguinius::TtsError);
  REQUIRE(std::filesystem::exists(unrelated));
}

TEST_CASE("Filesystem TTS cache never purges unknown owned-directory entries",
          "[tts][cache][security]") {
  TemporaryDirectory temporary;
  sanguinius::FilesystemTtsCache cache{temporary.path()};
  const auto unrelated = temporary.path() / "operator-note";
  {
    std::ofstream output{unrelated};
    output << "must remain";
  }
  REQUIRE_THROWS_AS(cache.purge(), sanguinius::TtsError);
  REQUIRE(std::filesystem::exists(unrelated));
}

TEST_CASE("Static Vox assets require an approved exact provenance manifest",
          "[tts][fallback][security]") {
  TemporaryDirectory temporary;
  const std::vector<std::byte> pcm{std::byte{0}, std::byte{0}, std::byte{0},
                                   std::byte{0}};
  const auto checksum = sanguinius::sha256_hex(pcm);
  const auto write_pcm = [&temporary, &pcm](const std::string_view name) {
    std::ofstream output{temporary.path() / name, std::ios::binary};
    output.write(reinterpret_cast<const char *>(pcm.data()),
                 static_cast<std::streamsize>(pcm.size()));
  };
  write_pcm("entrance.pcm");
  write_pcm("error.pcm");
  write_pcm("farewell.pcm");
  nlohmann::json manifest{
      {"manifest_version", 1},
      {"approved", true},
      {"provider", "openai"},
      {"model", "tts-1"},
      {"voice", "onyx"},
      {"response_format", "wav"},
      {"speed", 1.0},
      {"target_format", "s16le/48000/stereo"},
      {"generation_date", "2026-08-25"},
      {"clips",
       {{"entrance",
         {{"text", "The vox is open. Sanguinius attends."},
          {"file", "entrance.pcm"},
          {"sha256", checksum}}},
        {"error",
         {{"text", "The vox falters. Read the channel for details."},
          {"file", "error.pcm"},
          {"sha256", checksum}}},
        {"farewell",
         {{"text", "The channel closes. Until we speak again."},
          {"file", "farewell.pcm"},
          {"sha256", checksum}}}}}};
  {
    std::ofstream output{temporary.path() / "fallbacks-v1.json"};
    output << manifest.dump();
  }
  const auto assets =
      sanguinius::load_static_speech_assets(temporary.path());
  REQUIRE(assets.entrance.samples.size() == 2);
  REQUIRE(assets.error.samples.size() == 2);
  REQUIRE(assets.farewell.samples.size() == 2);

  manifest["approved"] = false;
  {
    std::ofstream output{temporary.path() / "fallbacks-v1.json"};
    output << manifest.dump();
  }
  REQUIRE_THROWS(sanguinius::load_static_speech_assets(temporary.path()));
}

TEST_CASE("Checked-in Vox fallback assets match their approved manifest",
          "[tts][fallback][assets]") {
  const auto assets = sanguinius::load_static_speech_assets(
      SANGUINIUS_TEST_VOX_ASSET_DIRECTORY);
  REQUIRE(assets.entrance.samples.size() == 208'800);
  REQUIRE(assets.error.samples.size() == 270'000);
  REQUIRE(assets.farewell.samples.size() == 220'800);
}

TEST_CASE("Vox runtime validation requires an owner-only cache directory",
          "[tts][cache][config][security]") {
  TemporaryDirectory temporary;
  sanguinius::Config config;
  config.features.vox_enabled = true;
  config.tts.cache_directory = temporary.path();
  config.tts.fallback_directory = SANGUINIUS_TEST_VOX_ASSET_DIRECTORY;
  config.paths.database_file = temporary.path().parent_path() /
                               "sanguinius-runtime-config-state.sqlite3";
  config.paths.message_log = temporary.path().parent_path() /
                             "sanguinius-runtime-config-messages.log";

  std::filesystem::permissions(
      temporary.path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  REQUIRE_NOTHROW(sanguinius::validate_runtime_configuration(config));

  std::filesystem::permissions(
      temporary.path(), std::filesystem::perms::group_read |
                            std::filesystem::perms::group_exec,
      std::filesystem::perm_options::add);
  REQUIRE_THROWS(sanguinius::validate_runtime_configuration(config));

  std::filesystem::permissions(
      temporary.path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
}
