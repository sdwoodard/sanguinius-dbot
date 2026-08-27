#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/speech.hpp"
#include "sanguinius/tts_cache.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sanguinius {

struct StaticSpeechAssets {
  PcmAudio entrance;
  PcmAudio error;
  PcmAudio farewell;
};

struct SpeechServiceConfiguration {
  bool provider_enabled{};
  TtsUsagePolicy usage_policy;
  AudioNormalizationLimits normalization_limits;
  std::chrono::milliseconds request_deadline{30'000};
  std::size_t maximum_attempts{2};
  std::size_t maximum_text_scalars{maximum_tts_scalar_count};
  std::size_t queue_capacity{64};
};

struct SpeechAdmission {
  SpeechEnqueueStatus status{SpeechEnqueueStatus::invalid_session};
  std::string message;
};

using SpeechTextFallback =
    std::function<void(std::string provider_nonce, std::string message)>;

struct SpeechServiceHealth {
  bool provider_enabled{};
  std::string voice{"onyx"};
  QueueSnapshot synthesis_worker;
  QueueSnapshot playback_worker;
  SpeechRepositoryHealth repository;
  TtsCacheHealth cache;
  TtsUsagePolicy usage_policy;
  std::size_t synthesis_worker_rejections{};
  std::size_t playback_worker_rejections{};
  std::optional<std::int64_t> last_normalization_latency_ms;
  std::optional<std::string> last_failure_category;
};

class SpeechService {
public:
  SpeechService(SpeechRepository &repository, TextToSpeechClient *client,
                AudioNormalizer &normalizer, TtsCache &cache,
                VoiceGateway &gateway, const Clock &clock,
                PersistentIdGenerator &ids, Diagnostics &diagnostics,
                StaticSpeechAssets assets,
                SpeechServiceConfiguration configuration = {},
                SpeechTextFallback text_fallback = {},
                std::function<bool()> automatic_narration_suppressed = {});
  ~SpeechService();

  SpeechService(const SpeechService &) = delete;
  SpeechService &operator=(const SpeechService &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SpeechAdmission say(std::string_view session_id,
                                    std::string_view guild_id, std::string text,
                                    std::string deduplication_key,
                                    std::int64_t now_ms);
  void session_ready(std::string session_id, std::string guild_id, bool muted,
                     bool enqueue_entrance = true);
  void session_reconnecting(std::string_view session_id);
  [[nodiscard]] bool session_leaving(std::string_view session_id,
                                     std::string_view guild_id,
                                     bool allow_contextual = true) noexcept;
  void session_closed(std::string_view session_id) noexcept;
  void set_muted(std::string_view session_id, bool muted);
  void set_voice_input_listening(bool listening) noexcept;
  void wake() noexcept;
  void begin_session_flavor(std::string session_id);
  void prepare_session_flavor(std::string session_id, std::string guild_id,
                              std::string entrance_line,
                              std::string farewell_line);
  void discard_session_flavor(std::string_view session_id) noexcept;
  [[nodiscard]] std::optional<PcmAudio>
  take_prepared_entrance(std::string_view session_id) noexcept;
  [[nodiscard]] bool track_marker(std::string_view session_id,
                                  std::string marker);
  [[nodiscard]] std::string selected_voice(std::string_view guild_id);
  [[nodiscard]] SpeechMutationStatus
  select_voice(std::string_view guild_id, std::string_view voice,
               std::string_view actor_user_id, std::int64_t now_ms);
  [[nodiscard]] SpeechServiceHealth health() const;
  [[nodiscard]] std::size_t purge();
  [[nodiscard]] const PcmAudio &entrance_clip() const noexcept;
  [[nodiscard]] bool run_test_scenario(std::string session_id,
                                       std::string guild_id,
                                       std::string scenario,
                                       std::string source_identity);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::int64_t calendar_month_start_utc_ms(std::int64_t now_ms);

} // namespace sanguinius
