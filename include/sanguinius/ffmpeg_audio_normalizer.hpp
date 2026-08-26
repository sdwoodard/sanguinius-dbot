#pragma once

#include "sanguinius/tts.hpp"

#include <filesystem>

namespace sanguinius {

void validate_ffmpeg_runtime(const std::filesystem::path &ffprobe_path,
                             const std::filesystem::path &ffmpeg_path,
                             unsigned expected_major_version = 9);

class FfmpegAudioNormalizer final : public AudioNormalizer {
public:
  FfmpegAudioNormalizer(std::filesystem::path ffprobe_path,
                        std::filesystem::path ffmpeg_path);

  [[nodiscard]] NormalizedAudio
  normalize(const SynthesizedAudio &audio,
            const AudioNormalizationLimits &limits,
            std::stop_token stop_token) const override;

private:
  std::filesystem::path ffprobe_path_;
  std::filesystem::path ffmpeg_path_;
};

} // namespace sanguinius
