#pragma once

#include "sanguinius/speech_service.hpp"

#include <filesystem>

namespace sanguinius {

[[nodiscard]] StaticSpeechAssets
load_static_speech_assets(const std::filesystem::path &directory);

} // namespace sanguinius
