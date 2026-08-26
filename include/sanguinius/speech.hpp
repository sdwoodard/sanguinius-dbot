#pragma once

#include "sanguinius/tts.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view vox_tts_purge_job_type{"vox.tts_purge.v1"};
inline constexpr std::int64_t vox_tts_purge_interval_ms{60 * 60 * 1'000};

enum class SpeechPriority : std::int64_t {
  flavor = 100,
  event_narration = 200,
  interactive = 300,
  critical_control = 400,
};

enum class SpeechState {
  pending,
  synthesizing,
  ready,
  playing,
  played,
  failed,
  expired,
  cancelled,
};

struct SpeechItem {
  std::string speech_id;
  std::string voice_session_id;
  std::optional<std::string> source_event_id;
  std::string source_kind;
  std::optional<std::string> text;
  std::string text_hash;
  std::size_t scalar_count{};
  std::string provider;
  std::string model;
  std::string voice;
  SpeechPriority priority{SpeechPriority::flavor};
  std::uint8_t narration_rank{};
  SpeechState state{SpeechState::pending};
  std::size_t revision{1};
  std::int64_t earliest_at_ms{};
  std::optional<std::int64_t> expires_at_ms;
  bool interruptible{true};
  std::string deduplication_key;
  std::optional<std::string> provider_request_id;
  std::optional<std::string> cache_key;
  std::optional<std::string> cache_checksum;
  std::optional<std::string> marker;
  std::optional<std::int64_t> duration_ms;
  std::size_t attempt_count{};
  std::int64_t created_at_ms{};
  std::optional<std::int64_t> terminal_at_ms;
  std::optional<std::string> last_error_code;
};

struct SpeechEnqueueRequest {
  std::string speech_id;
  std::string voice_session_id;
  std::optional<std::string> source_event_id;
  std::string source_kind;
  NormalizedTtsText text;
  std::string text_hash;
  std::string provider{"openai"};
  std::string model{"tts-1"};
  std::string voice{"onyx"};
  SpeechPriority priority{SpeechPriority::interactive};
  std::uint8_t narration_rank{};
  std::int64_t earliest_at_ms{};
  std::optional<std::int64_t> expires_at_ms;
  bool interruptible{};
  std::string deduplication_key;
  std::int64_t created_at_ms{};
};

enum class SpeechEnqueueStatus {
  accepted,
  replay,
  queue_full,
  invalid_session,
};

struct SpeechEnqueueResult {
  SpeechEnqueueStatus status{SpeechEnqueueStatus::invalid_session};
  std::optional<SpeechItem> item;
  std::optional<std::string> evicted_speech_id;
};

struct SpeechTransitionRequest {
  std::string speech_id;
  std::size_t expected_revision{};
  SpeechState target{SpeechState::failed};
  std::string transition_id;
  std::string reason;
  std::string idempotency_key;
  std::int64_t occurred_at_ms{};
  std::optional<std::string> provider_request_id;
  std::optional<std::string> cache_key;
  std::optional<std::string> cache_checksum;
  std::optional<std::string> marker;
  std::optional<std::int64_t> duration_ms;
  std::optional<std::string> error_code;
};

enum class SpeechMutationStatus {
  applied,
  unchanged,
  not_found,
  stale,
  invalid_state,
};

struct TtsUsagePolicy {
  std::int64_t rolling_day_micro_usd{100'000};
  std::int64_t calendar_month_micro_usd{2'000'000};
  std::size_t rolling_day_attempts{20};
};

struct TtsUsageSummary {
  std::int64_t rolling_day_micro_usd{};
  std::int64_t calendar_month_micro_usd{};
  std::size_t rolling_day_attempts{};
  std::size_t rolling_day_succeeded{};
  std::size_t rolling_day_failed{};
  std::size_t rolling_day_unknown{};
};

struct TtsUsageReservationRequest {
  std::string attempt_id;
  std::string speech_id;
  std::size_t attempt_number{};
  std::string provider;
  std::string model;
  std::string voice;
  std::size_t scalar_count{};
  std::int64_t estimated_micro_usd{};
  std::int64_t now_ms{};
  std::int64_t calendar_month_start_ms{};
  TtsUsagePolicy policy;
};

struct TtsUsageReservationResult {
  bool accepted{};
  bool replay{};
  TtsUsageSummary usage;
};

struct TtsUsageCompletion {
  std::string attempt_id;
  std::string state;
  std::optional<std::string> provider_request_id;
  std::optional<std::int64_t> latency_ms;
  std::optional<std::int64_t> duration_ms;
  std::optional<std::string> error_code;
  std::int64_t completed_at_ms{};
};

struct TtsCacheMetadata {
  std::string cache_key;
  std::string checksum;
  std::uintmax_t byte_count{};
  std::uintmax_t frame_count{};
  std::string provider;
  std::string model;
  std::string voice;
  std::int64_t created_at_ms{};
  std::int64_t last_access_at_ms{};
};

struct SpeechRepositoryHealth {
  std::size_t queued{};
  std::size_t synthesizing{};
  std::size_t ready{};
  std::size_t playing{};
  TtsUsageSummary usage;
  std::size_t cache_entries{};
  std::uintmax_t cache_bytes{};
};

class SpeechRepository {
public:
  virtual ~SpeechRepository() = default;
  [[nodiscard]] virtual SpeechEnqueueResult
  enqueue(const SpeechEnqueueRequest &request) = 0;
  [[nodiscard]] virtual std::optional<SpeechItem>
  claim_next(std::string_view voice_session_id, std::int64_t now_ms,
             std::string transition_id, std::string idempotency_key) = 0;
  [[nodiscard]] virtual SpeechMutationStatus
  transition(const SpeechTransitionRequest &request) = 0;
  [[nodiscard]] virtual std::optional<SpeechItem>
  find(std::string_view speech_id) = 0;
  [[nodiscard]] virtual std::size_t
  cancel_session(std::string_view voice_session_id, std::int64_t now_ms,
                 std::string_view reason, bool include_interactive,
                 bool preserve_event_narration = false) = 0;
  [[nodiscard]] virtual std::size_t
  recover(std::int64_t now_ms, std::string_view reason) = 0;
  virtual void ensure_purge_schedule(std::int64_t now_ms,
                                     std::string job_id) = 0;
  [[nodiscard]] virtual std::size_t purge_retained(std::int64_t now_ms) = 0;
  [[nodiscard]] virtual TtsUsageReservationResult
  reserve_usage(const TtsUsageReservationRequest &request) = 0;
  [[nodiscard]] virtual SpeechMutationStatus
  complete_usage(const TtsUsageCompletion &completion) = 0;
  [[nodiscard]] virtual std::optional<TtsCacheMetadata>
  cache_metadata(std::string_view cache_key, std::int64_t accessed_at_ms) = 0;
  virtual void put_cache_metadata(const TtsCacheMetadata &metadata) = 0;
  virtual void remove_cache_metadata(std::string_view cache_key) = 0;
  [[nodiscard]] virtual std::vector<std::string> cache_keys() = 0;
  [[nodiscard]] virtual std::string
  selected_voice(std::string_view guild_id) = 0;
  [[nodiscard]] virtual SpeechMutationStatus
  select_voice(std::string_view guild_id, std::string_view voice,
               std::string_view actor_user_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual SpeechRepositoryHealth
  health(std::int64_t now_ms, std::int64_t calendar_month_start_ms) = 0;
};

[[nodiscard]] const char *speech_priority_name(SpeechPriority priority) noexcept;
[[nodiscard]] const char *speech_state_name(SpeechState state) noexcept;
[[nodiscard]] bool speech_transition_allowed(SpeechState from,
                                             SpeechState to) noexcept;

} // namespace sanguinius
