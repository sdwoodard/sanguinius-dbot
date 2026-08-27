#include "sanguinius/retention.hpp"

#include "sanguinius/speech_service.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

} // namespace

RetentionService::RetentionService(
    std::unique_ptr<RetentionRepository> repository, const Clock &clock,
    PersistentIdGenerator &ids, SpeechRepository *speech, TtsCache *tts_cache)
    : repository_{std::move(repository)}, clock_{clock}, ids_{ids},
      speech_{speech}, tts_cache_{tts_cache} {
  if (!repository_)
    throw std::invalid_argument{"Retention repository is required."};
  if ((speech_ == nullptr) != (tts_cache_ == nullptr))
    throw std::invalid_argument{
        "Retention speech and TTS cache dependencies must be paired."};
  repository_->ensure_schedule(now_ms(clock_), ids_.next_id());
}

RetentionCounts RetentionService::run() {
  RetentionCounts initial;
  if (speech_) {
    try {
      initial.tts_cache_removals = reconcile_tts_cache(*speech_, *tts_cache_);
    } catch (...) {
      // Cache corruption or filesystem failures must not prevent the durable
      // database policies from running. The daily audit records the category
      // failure without retaining a path or exception detail.
      initial.tts_cache_failures = 1;
    }
  }
  return repository_->run(now_ms(clock_), ids_.next_id(), initial);
}

std::int64_t RetentionService::next_due_utc(const std::int64_t now_ms_value) {
  if (now_ms_value < 0)
    throw std::invalid_argument{"Retention time is invalid."};
  using namespace std::chrono;
  const auto now = sys_time<milliseconds>{milliseconds{now_ms_value}};
  auto due = floor<days>(now) + hours{4};
  if (due <= now)
    due += days{1};
  return duration_cast<milliseconds>(due.time_since_epoch()).count();
}

} // namespace sanguinius
