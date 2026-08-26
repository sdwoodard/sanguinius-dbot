#pragma once

#include "sanguinius/tts.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

struct TtsCachePolicy {
  std::uintmax_t maximum_bytes{128U * 1024U * 1024U};
  std::chrono::hours maximum_age{24 * 30};
};

struct TtsCacheHealth {
  std::size_t entries{};
  std::uintmax_t bytes{};
  std::size_t hits{};
  std::size_t misses{};
  std::size_t corruptions{};
};

struct TtsCacheMutationResult {
  std::vector<std::string> removed_keys;
};

class TtsCache {
public:
  virtual ~TtsCache() = default;
  [[nodiscard]] virtual std::optional<PcmAudio>
  read(std::string_view key, std::string_view expected_checksum) = 0;
  [[nodiscard]] virtual TtsCacheMutationResult
  write(std::string_view key, const PcmAudio &audio) = 0;
  virtual void erase(std::string_view key) noexcept = 0;
  virtual void record_miss() noexcept = 0;
  [[nodiscard]] virtual TtsCacheMutationResult purge() = 0;
  [[nodiscard]] virtual std::vector<std::string> keys() const = 0;
  [[nodiscard]] virtual TtsCacheHealth health() const = 0;
};

class FilesystemTtsCache final : public TtsCache {
public:
  FilesystemTtsCache(std::filesystem::path root,
                     TtsCachePolicy policy = {});

  [[nodiscard]] std::optional<PcmAudio>
  read(std::string_view key, std::string_view expected_checksum) override;
  [[nodiscard]] TtsCacheMutationResult
  write(std::string_view key, const PcmAudio &audio) override;
  void erase(std::string_view key) noexcept override;
  void record_miss() noexcept override;
  [[nodiscard]] TtsCacheMutationResult purge() override;
  [[nodiscard]] std::vector<std::string> keys() const override;
  [[nodiscard]] TtsCacheHealth health() const override;

private:
  std::filesystem::path root_;
  TtsCachePolicy policy_;
  mutable std::mutex mutex_;
  TtsCacheHealth health_;

  [[nodiscard]] TtsCacheMutationResult
  purge_locked(std::string_view protected_key = {});
};

} // namespace sanguinius
