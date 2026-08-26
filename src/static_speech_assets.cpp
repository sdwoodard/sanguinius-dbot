#include "sanguinius/static_speech_assets.hpp"

#include "sanguinius/tts.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace sanguinius {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_manifest_bytes = 64U * 1024U;

[[nodiscard]] std::vector<std::byte>
read_secure_file(const std::filesystem::path &path, const std::size_t limit) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    throw std::runtime_error{"A required Vox fallback file is unavailable."};
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || static_cast<std::uintmax_t>(status.st_size) > limit) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error{"A Vox fallback file has an invalid shape."};
  }
  std::vector<std::byte> result(static_cast<std::size_t>(status.st_size));
  std::size_t offset{};
  while (offset < result.size()) {
    const auto received = ::read(
        descriptor, reinterpret_cast<char *>(result.data()) + offset,
        result.size() - offset);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR)
      continue;
    static_cast<void>(::close(descriptor));
    throw std::runtime_error{"A Vox fallback file could not be read fully."};
  }
  static_cast<void>(::close(descriptor));
  return result;
}

[[nodiscard]] bool iso_date(const std::string_view value) noexcept {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-')
    return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 4 || index == 7)
      continue;
    if (value[index] < '0' || value[index] > '9')
      return false;
  }
  return true;
}

[[nodiscard]] PcmAudio load_clip(const std::filesystem::path &directory,
                                 const Json &clips,
                                 const std::string_view kind,
                                 const std::string_view expected_text) {
  if (!clips.contains(kind) || !clips.at(kind).is_object())
    throw std::runtime_error{"The Vox fallback manifest is incomplete."};
  const auto &clip = clips.at(kind);
  const auto expected_file = std::string{kind} + ".pcm";
  if (clip.value("text", "") != expected_text ||
      clip.value("file", "") != expected_file)
    throw std::runtime_error{"The Vox fallback manifest text is not approved."};
  const auto expected_hash = clip.value("sha256", "");
  const auto bytes = read_secure_file(directory / expected_file,
                                      maximum_tts_pcm_bytes);
  if (bytes.empty() || bytes.size() % 4 != 0 ||
      sha256_hex(bytes) != expected_hash)
    throw std::runtime_error{"A Vox fallback PCM checksum is invalid."};
  PcmAudio audio;
  audio.samples.reserve(bytes.size() / 2);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    audio.samples.push_back(static_cast<std::int16_t>(low | (high << 8U)));
  }
  static_cast<void>(validated_pcm_bytes(audio));
  return audio;
}

} // namespace

StaticSpeechAssets
load_static_speech_assets(const std::filesystem::path &directory) {
  if (!directory.is_absolute())
    throw std::invalid_argument{"Vox fallback directory must be absolute."};
  const auto bytes =
      read_secure_file(directory / "fallbacks-v1.json", maximum_manifest_bytes);
  Json manifest;
  try {
    manifest = Json::parse(
        std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
  } catch (const Json::exception &) {
    throw std::runtime_error{"The Vox fallback manifest is invalid."};
  }
  if (!manifest.is_object() || manifest.value("manifest_version", 0) != 1 ||
      !manifest.value("approved", false) ||
      manifest.value("provider", "") != "openai" ||
      manifest.value("model", "") != "tts-1" ||
      manifest.value("voice", "") != "onyx" ||
      manifest.value("response_format", "") != "wav" ||
      manifest.value("speed", 0.0) != 1.0 ||
      manifest.value("target_format", "") != "s16le/48000/stereo" ||
      !iso_date(manifest.value("generation_date", "")) ||
      !manifest.contains("clips") || !manifest.at("clips").is_object())
    throw std::runtime_error{
        "The Vox fallback manifest is not an approved tts-1/onyx asset set."};
  const auto &clips = manifest.at("clips");
  return {.entrance = load_clip(directory, clips, "entrance",
                                "The vox is open. Sanguinius attends."),
          .error = load_clip(
              directory, clips, "error",
              "The vox falters. Read the channel for details."),
          .farewell = load_clip(
              directory, clips, "farewell",
              "The channel closes. Until we speak again.")};
}

} // namespace sanguinius
