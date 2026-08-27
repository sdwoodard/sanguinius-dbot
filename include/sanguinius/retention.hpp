#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/persistent_id.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sanguinius {

class SpeechRepository;
class TtsCache;

inline constexpr std::string_view retention_job_type{
    "maintenance.retention.v1"};

struct RetentionCounts {
  std::size_t appearance_contexts{};
  std::size_t appearance_previews{};
  std::size_t notice_payloads{};
  std::size_t interaction_snapshots{};
  std::size_t speech_items{};
  std::size_t provider_usage{};
  std::size_t diagnostics{};
  std::size_t tts_cache_removals{};
  std::size_t tts_cache_failures{};
};

class RetentionRepository {
public:
  virtual ~RetentionRepository() = default;
  virtual void ensure_schedule(std::int64_t now_ms, std::string job_id) = 0;
  [[nodiscard]] virtual RetentionCounts run(std::int64_t now_ms,
                                            std::string run_id,
                                            RetentionCounts initial = {}) = 0;
};

class RetentionService {
public:
  RetentionService(std::unique_ptr<RetentionRepository> repository,
                   const Clock &clock, PersistentIdGenerator &ids,
                   SpeechRepository *speech = nullptr,
                   TtsCache *tts_cache = nullptr);

  [[nodiscard]] RetentionCounts run();
  [[nodiscard]] static std::int64_t next_due_utc(std::int64_t now_ms);

private:
  std::unique_ptr<RetentionRepository> repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  SpeechRepository *speech_{};
  TtsCache *tts_cache_{};
};

} // namespace sanguinius
