#include "sanguinius/speech_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t wall_now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] SpeechAdmission admission(const SpeechEnqueueStatus status) {
  switch (status) {
  case SpeechEnqueueStatus::accepted:
    return {status, "The line is queued for Vox Sanguinius."};
  case SpeechEnqueueStatus::replay:
    return {status, "That Vox line was already received."};
  case SpeechEnqueueStatus::queue_full:
    return {status, "The Vox speech queue is full. Try again shortly."};
  case SpeechEnqueueStatus::invalid_session:
    return {status, "Vox must be ready before speech can be queued."};
  }
  return {SpeechEnqueueStatus::invalid_session, "Vox speech is unavailable."};
}

void stop_aware_delay(const std::stop_token stop_token,
                      const std::chrono::milliseconds delay) {
  std::mutex mutex;
  std::condition_variable_any condition;
  std::unique_lock lock{mutex};
  condition.wait_for(lock, stop_token, delay, [] { return false; });
}

[[nodiscard]] std::int64_t duration_for(const PcmAudio &audio) {
  const auto frames = audio.samples.size() / 2;
  return static_cast<std::int64_t>(
      (frames * std::size_t{1'000} + std::size_t{47'999}) /
      std::size_t{48'000});
}

[[nodiscard]] bool terminal_speech_state(const SpeechState state) noexcept {
  return state == SpeechState::played || state == SpeechState::failed ||
         state == SpeechState::expired || state == SpeechState::cancelled;
}

} // namespace

std::int64_t calendar_month_start_utc_ms(const std::int64_t now_ms) {
  if (now_ms < 0)
    throw std::invalid_argument{"TTS usage time must be nonnegative."};
  using namespace std::chrono;
  const auto now = sys_time<milliseconds>{milliseconds{now_ms}};
  const year_month_day date{floor<days>(now)};
  return duration_cast<milliseconds>(
             sys_days{date.year() / date.month() / day{1}}.time_since_epoch())
      .count();
}

class SpeechService::Impl {
public:
  struct PreparedSessionFlavor {
    std::optional<PcmAudio> entrance;
    std::optional<PcmAudio> farewell;
    std::string farewell_text;
    std::int64_t entrance_expires_at_ms{};
    bool entrance_consumed{};
  };

  Impl(SpeechRepository &repository, TextToSpeechClient *client,
       AudioNormalizer &normalizer, TtsCache &cache, VoiceGateway &gateway,
       const Clock &clock, PersistentIdGenerator &ids, Diagnostics &diagnostics,
       StaticSpeechAssets assets, SpeechServiceConfiguration configuration,
       SpeechTextFallback text_fallback,
       std::function<bool()> automatic_narration_suppressed)
      : repository_{repository}, client_{client}, normalizer_{normalizer},
        cache_{cache}, gateway_{gateway}, clock_{clock}, ids_{ids},
        diagnostics_{diagnostics}, assets_{std::move(assets)},
        configuration_{configuration}, text_fallback_{std::move(text_fallback)},
        automatic_narration_suppressed_{
            std::move(automatic_narration_suppressed)},
        synthesis_worker_{configuration.queue_capacity, 1},
        flavor_worker_{configuration.queue_capacity, 1},
        playback_worker_{configuration.queue_capacity, 1} {
    if ((configuration_.provider_enabled && client_ == nullptr) ||
        configuration_.maximum_attempts == 0 ||
        configuration_.maximum_attempts > 2 ||
        configuration_.request_deadline <= std::chrono::milliseconds::zero() ||
        configuration_.request_deadline > std::chrono::milliseconds{30'000} ||
        configuration_.maximum_text_scalars == 0 ||
        configuration_.maximum_text_scalars > maximum_tts_scalar_count ||
        configuration_.queue_capacity == 0)
      throw std::invalid_argument{"Speech service configuration is invalid."};
    static_cast<void>(validated_pcm_bytes(assets_.entrance));
    static_cast<void>(validated_pcm_bytes(assets_.error));
    static_cast<void>(validated_pcm_bytes(assets_.farewell));
  }

  void start() {
    if (started_.exchange(true))
      throw std::logic_error{"Speech service may only be started once."};
    static_cast<void>(
        repository_.recover(wall_now_ms(clock_), "restart_abandoned"));
    repository_.ensure_purge_schedule(wall_now_ms(clock_), ids_.next_id());
    reconcile_cache();
    playback_worker_.start();
    try {
      synthesis_worker_.start();
    } catch (...) {
      playback_worker_.stop();
      throw;
    }
    try {
      flavor_worker_.start();
    } catch (...) {
      synthesis_worker_.stop();
      playback_worker_.stop();
      throw;
    }
  }

  void stop() noexcept {
    if (stopped_.exchange(true))
      return;
    flavor_worker_.stop();
    synthesis_worker_.stop();
    playback_worker_.stop();
    std::optional<std::string> session;
    {
      const std::scoped_lock lock{state_mutex_};
      session = active_session_id_;
    }
    if (session) {
      try {
        static_cast<void>(gateway_.stop_audio(*session));
        static_cast<void>(repository_.cancel_session(
            *session, wall_now_ms(clock_), "service_stopped", true));
      } catch (...) {
      }
    }
  }

  SpeechAdmission say(const std::string_view session_id,
                      const std::string_view guild_id, std::string text,
                      std::string deduplication_key,
                      const std::int64_t current_time_ms) {
    if (stopped_.load())
      return admission(SpeechEnqueueStatus::invalid_session);
    const auto normalized = normalize_tts_text(text);
    if (normalized.scalar_count > configuration_.maximum_text_scalars)
      return {SpeechEnqueueStatus::invalid_session,
              "That Vox line exceeds the configured text limit."};
    const auto voice = repository_.selected_voice(guild_id);
    const auto text_hash = sha256_hex(std::as_bytes(
        std::span{normalized.text.data(), normalized.text.size()}));
    const auto source_identity = deduplication_key;
    const auto result =
        repository_.enqueue({.speech_id = ids_.next_id(),
                             .voice_session_id = std::string{session_id},
                             .source_event_id = source_identity,
                             .source_kind = "direct_say",
                             .text = normalized,
                             .text_hash = text_hash,
                             .provider = "openai",
                             .model = "tts-1",
                             .voice = voice,
                             .priority = SpeechPriority::interactive,
                             .earliest_at_ms = current_time_ms,
                             .expires_at_ms = current_time_ms + 120'000,
                             .interruptible = false,
                             .deduplication_key = std::move(deduplication_key),
                             .created_at_ms = current_time_ms});
    if (result.status == SpeechEnqueueStatus::accepted)
      request_pump();
    return admission(result.status);
  }

  void session_ready(std::string session_id, std::string guild_id,
                     const bool muted, const bool enqueue_entrance) {
    submit([this, session_id = std::move(session_id),
            guild_id = std::move(guild_id), muted,
            enqueue_entrance](std::stop_token stop_token) mutable {
      {
        const std::scoped_lock lock{state_mutex_};
        active_session_id_ = session_id;
        active_guild_id_ = guild_id;
        muted_ = muted;
        transport_ready_ = true;
      }
      if (!muted && enqueue_entrance)
        enqueue_static(session_id, guild_id, "entrance", assets_.entrance,
                       SpeechPriority::flavor, 15'000, {}, {}, stop_token);
      pump(stop_token);
    });
  }

  void session_reconnecting(const std::string_view session_id) {
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_session_id_ == session_id)
        transport_ready_ = false;
    }
    submit([this, session_id = std::string{session_id}](
               const std::stop_token stop_token) {
      static_cast<void>(gateway_.stop_audio(session_id));
      if (!fail_playing(session_id, "playback_interrupted", stop_token))
        return;
      static_cast<void>(cancel_session_until_persisted(
          session_id, "reconnect_stale", false, true, stop_token));
    });
  }

  bool session_leaving(const std::string_view session_id,
                       const std::string_view guild_id,
                       const bool allow_contextual) noexcept {
    try {
      bool muted{};
      {
        const std::scoped_lock lock{state_mutex_};
        if (active_session_id_ != session_id)
          return false;
        muted = muted_;
      }
      static_cast<void>(gateway_.stop_audio(session_id));
      static_cast<void>(repository_.cancel_session(
          session_id, wall_now_ms(clock_), "commanded_leave", true));
      clear_playing_for_session(session_id);
      if (muted)
        return false;
      PcmAudio farewell = assets_.farewell;
      std::string kind{"farewell"};
      std::string farewell_text;
      {
        const std::scoped_lock lock{state_mutex_};
        const auto found = prepared_flavor_.find(std::string{session_id});
        if (allow_contextual && found != prepared_flavor_.end() &&
            found->second.farewell) {
          farewell = *found->second.farewell;
          farewell_text = found->second.farewell_text;
          kind = "contextual_farewell";
        }
        prepared_flavor_.erase(std::string{session_id});
      }
      const auto queued =
          enqueue_static(std::string{session_id}, std::string{guild_id}, kind,
                         farewell, SpeechPriority::flavor, 15'000, {}, {},
                         std::stop_token{}, farewell_text);
      if (queued == SpeechEnqueueStatus::accepted)
        request_pump();
      return queued == SpeechEnqueueStatus::accepted ||
             queued == SpeechEnqueueStatus::replay;
    } catch (...) {
      return false;
    }
  }

  void session_closed(const std::string_view session_id) noexcept {
    {
      const std::scoped_lock lock{state_mutex_};
      prepared_flavor_.erase(std::string{session_id});
      pending_contextual_farewell_.erase(std::string{session_id});
      if (active_session_id_ == session_id)
        transport_ready_ = false;
    }
    submit([this, session_id = std::string{session_id}](
               const std::stop_token stop_token) {
      static_cast<void>(gateway_.stop_audio(session_id));
      if (!fail_playing(session_id, "session_closed", stop_token))
        return;
      if (!cancel_session_until_persisted(session_id, "session_closed", true,
                                          false, stop_token))
        return;
      const std::scoped_lock lock{state_mutex_};
      if (active_session_id_ == session_id) {
        active_session_id_.reset();
        active_guild_id_.reset();
        muted_ = false;
      }
    });
  }

  void set_muted(const std::string_view session_id, const bool muted) {
    std::optional<SpeechItem> automatic_playback;
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_session_id_ != session_id)
        return;
      muted_ = muted;
      if (muted)
        prepared_flavor_.erase(std::string{session_id});
      if (muted && playing_ &&
          playing_->priority != SpeechPriority::interactive)
        automatic_playback = playing_;
    }
    if (automatic_playback)
      static_cast<void>(gateway_.stop_audio(session_id));
    if (muted) {
      static_cast<void>(repository_.cancel_session(
          session_id, wall_now_ms(clock_), "muted", false));
      if (automatic_playback)
        clear_playing(automatic_playback->speech_id);
    } else {
      request_pump();
    }
  }

  void wake() noexcept { request_pump(); }

  void begin_session_flavor(std::string session_id) {
    const auto entrance_expires_at_ms = wall_now_ms(clock_) + 15'000;
    const std::scoped_lock lock{state_mutex_};
    prepared_flavor_.clear();
    prepared_flavor_.emplace(std::move(session_id),
                             PreparedSessionFlavor{.entrance = std::nullopt,
                                                   .farewell = std::nullopt,
                                                   .farewell_text = {},
                                                   .entrance_expires_at_ms =
                                                       entrance_expires_at_ms});
  }

  void prepare_session_flavor(std::string session_id, std::string guild_id,
                              std::string entrance_line,
                              std::string farewell_line) {
    const auto entrance = normalize_tts_text(entrance_line);
    const auto farewell = normalize_tts_text(farewell_line);
    if (entrance.scalar_count > 160 || farewell.scalar_count > 160)
      throw std::invalid_argument{"Contextual Vox flavor is too long."};
    std::int64_t entrance_expires_at_ms{};
    {
      const std::scoped_lock lock{state_mutex_};
      auto found = prepared_flavor_.find(session_id);
      // Missing preparation state means mute, quiet, leave, or another session
      // boundary cancelled this generation. A late AI callback must not
      // recreate it after the suppressing condition is cleared.
      if (found == prepared_flavor_.end())
        return;
      found->second.farewell_text = farewell.text;
      entrance_expires_at_ms = found->second.entrance_expires_at_ms;
    }
    const auto flavor_session_id = session_id;
    const auto submitted = flavor_worker_.try_submit(
        [this, session_id = std::move(session_id),
         guild_id = std::move(guild_id), entrance, farewell,
         entrance_expires_at_ms](const std::stop_token stop_token) {
          const auto prepare_line =
              [this, &session_id, &guild_id, &stop_token](
                  const NormalizedTtsText &line, const std::string_view kind,
                  const std::optional<std::int64_t> expires_at_ms) {
                const auto bytes = std::as_bytes(
                    std::span{line.text.data(), line.text.size()});
                const auto current = wall_now_ms(clock_);
                SpeechItem item{.speech_id = ids_.next_id(),
                                .voice_session_id = session_id,
                                .source_event_id = std::nullopt,
                                .source_kind = std::string{kind},
                                .text = line.text,
                                .text_hash = sha256_hex(bytes),
                                .scalar_count = line.scalar_count,
                                .provider = "openai",
                                .model = "tts-1",
                                .voice = repository_.selected_voice(guild_id),
                                .priority = SpeechPriority::flavor,
                                .narration_rank = 0,
                                .state = SpeechState::pending,
                                .revision = 1,
                                .earliest_at_ms = current,
                                .expires_at_ms = expires_at_ms,
                                .interruptible = true,
                                .deduplication_key = {},
                                .provider_request_id = std::nullopt,
                                .cache_key = std::nullopt,
                                .cache_checksum = std::nullopt,
                                .marker = std::nullopt,
                                .duration_ms = std::nullopt,
                                .attempt_count = 0,
                                .created_at_ms = current,
                                .terminal_at_ms = std::nullopt,
                                .last_error_code = std::nullopt};
                return prepare(item, stop_token).audio;
              };
          try {
            if (entrance_flavor_allowed(session_id, entrance_expires_at_ms)) {
              auto entrance_audio = prepare_line(
                  entrance, "contextual_entrance", entrance_expires_at_ms);
              if (entrance_flavor_allowed(session_id, entrance_expires_at_ms)) {
                const std::scoped_lock lock{state_mutex_};
                const auto found = prepared_flavor_.find(session_id);
                if (found != prepared_flavor_.end() &&
                    !found->second.entrance_consumed)
                  found->second.entrance = std::move(entrance_audio);
              }
            }
          } catch (const TtsError &error) {
            record_failure(error.category());
          } catch (...) {
            record_failure(TtsFailureCategory::unavailable);
          }
          try {
            if (context_flavor_allowed(session_id)) {
              auto farewell_audio =
                  prepare_line(farewell, "contextual_farewell", std::nullopt);
              if (context_flavor_allowed(session_id)) {
                const std::scoped_lock lock{state_mutex_};
                const auto found = prepared_flavor_.find(session_id);
                if (found != prepared_flavor_.end())
                  found->second.farewell = std::move(farewell_audio);
              }
            }
          } catch (const TtsError &error) {
            record_failure(error.category());
          } catch (...) {
            record_failure(TtsFailureCategory::unavailable);
          }
        });
    if (submitted != SubmitResult::accepted) {
      const std::scoped_lock lock{state_mutex_};
      prepared_flavor_.erase(flavor_session_id);
    }
  }

  void discard_session_flavor(const std::string_view session_id) noexcept {
    const std::scoped_lock lock{state_mutex_};
    prepared_flavor_.erase(std::string{session_id});
    pending_contextual_farewell_.erase(std::string{session_id});
  }

  std::optional<PcmAudio>
  take_prepared_entrance(const std::string_view session_id) noexcept {
    try {
      const std::scoped_lock lock{state_mutex_};
      const auto found = prepared_flavor_.find(std::string{session_id});
      if (found == prepared_flavor_.end())
        return std::nullopt;
      found->second.entrance_consumed = true;
      if (found->second.entrance_expires_at_ms <= wall_now_ms(clock_)) {
        found->second.entrance.reset();
        return std::nullopt;
      }
      auto result = std::move(found->second.entrance);
      found->second.entrance.reset();
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }

  bool track_marker(const std::string_view session_id, std::string marker) {
    return submit([this, session_id = std::string{session_id},
                   marker = std::move(marker)](std::stop_token stop_token) {
      std::optional<SpeechItem> playing;
      {
        const std::scoped_lock lock{state_mutex_};
        if (!playing_ || playing_->voice_session_id != session_id ||
            playing_->marker != marker)
          return;
        playing = playing_;
      }
      if (persist_terminal(*playing, SpeechState::played, "marker_completed",
                           stop_token))
        pump(stop_token);
    });
  }

  std::string selected_voice(const std::string_view guild_id) {
    return repository_.selected_voice(guild_id);
  }

  SpeechMutationStatus select_voice(const std::string_view guild_id,
                                    const std::string_view voice,
                                    const std::string_view actor_user_id,
                                    const std::int64_t now_ms) {
    return repository_.select_voice(guild_id, voice, actor_user_id, now_ms);
  }

  SpeechServiceHealth health() const {
    const auto current = wall_now_ms(clock_);
    std::string voice{"onyx"};
    std::optional<std::string> guild;
    std::optional<std::string> failure;
    std::optional<std::int64_t> normalization_latency;
    {
      const std::scoped_lock lock{state_mutex_};
      guild = active_guild_id_;
      failure = last_failure_category_;
      normalization_latency = last_normalization_latency_ms_;
    }
    if (guild) {
      try {
        voice = repository_.selected_voice(*guild);
      } catch (...) {
      }
    }
    return {.provider_enabled = configuration_.provider_enabled,
            .voice = std::move(voice),
            .synthesis_worker = synthesis_worker_.snapshot(),
            .playback_worker = playback_worker_.snapshot(),
            .repository = repository_.health(
                current, calendar_month_start_utc_ms(current)),
            .cache = cache_.health(),
            .usage_policy = configuration_.usage_policy,
            .synthesis_worker_rejections = synthesis_worker_rejections_.load(),
            .playback_worker_rejections = playback_worker_rejections_.load(),
            .last_normalization_latency_ms = normalization_latency,
            .last_failure_category = std::move(failure)};
  }

  std::size_t purge() {
    reconcile_cache();
    return repository_.purge_retained(wall_now_ms(clock_));
  }

  [[nodiscard]] const PcmAudio &entrance_clip() const noexcept {
    return assets_.entrance;
  }

  bool run_test_scenario(std::string session_id, std::string guild_id,
                         std::string scenario, std::string source_identity) {
    if (source_identity.empty() || source_identity.size() > 80)
      return false;
    return submit([this, session_id = std::move(session_id),
                   guild_id = std::move(guild_id),
                   scenario = std::move(scenario),
                   source_identity =
                       std::move(source_identity)](std::stop_token stop_token) {
      if (scenario == "queue") {
        static_cast<void>(enqueue_static(
            session_id, guild_id, "test_flavor", assets_.error,
            SpeechPriority::flavor, 15'000, {}, source_identity, stop_token));
        static_cast<void>(
            enqueue_static(session_id, guild_id, "test_critical", assets_.error,
                           SpeechPriority::critical_control, 30'000, {},
                           source_identity, stop_token));
      } else if (scenario == "narration-stale") {
        const auto current = wall_now_ms(clock_);
        const auto normalized =
            normalize_tts_text("Audited stale narration simulation.");
        const auto text_hash = sha256_hex(std::as_bytes(
            std::span{normalized.text.data(), normalized.text.size()}));
        static_cast<void>(repository_.enqueue(
            {.speech_id = ids_.next_id(),
             .voice_session_id = session_id,
             .source_event_id = source_identity,
             .source_kind = "test_narration_stale",
             .text = normalized,
             .text_hash = text_hash,
             .provider = "openai",
             .model = "tts-1",
             .voice = repository_.selected_voice(guild_id),
             .priority = SpeechPriority::event_narration,
             .narration_rank = 1,
             .earliest_at_ms = current + 5'000,
             .expires_at_ms = current + 6'000,
             .interruptible = true,
             .deduplication_key = "speech:test:" + session_id +
                                  ":narration-stale:" + source_identity,
             .created_at_ms = current}));
      } else {
        const auto current = wall_now_ms(clock_);
        const auto kind = scenario == "provider-failure"
                              ? std::string{"test_provider_failure"}
                              : std::string{"test_budget_limit"};
        const auto normalized =
            normalize_tts_text(scenario == "provider-failure"
                                   ? "Audited provider failure simulation."
                                   : "Audited budget limit simulation.");
        const auto text_hash = sha256_hex(std::as_bytes(
            std::span{normalized.text.data(), normalized.text.size()}));
        static_cast<void>(repository_.enqueue(
            {.speech_id = ids_.next_id(),
             .voice_session_id = session_id,
             .source_event_id = source_identity,
             .source_kind = kind,
             .text = normalized,
             .text_hash = text_hash,
             .provider = "openai",
             .model = "tts-1",
             .voice = repository_.selected_voice(guild_id),
             .priority = SpeechPriority::interactive,
             .narration_rank = 0,
             .earliest_at_ms = current,
             .expires_at_ms = current + 30'000,
             .interruptible = false,
             .deduplication_key = "speech:test:" + session_id + ":" + kind +
                                  ":" + source_identity,
             .created_at_ms = current}));
      }
      pump(stop_token);
    });
  }

private:
  bool submit(BoundedExecutor::Task task) noexcept {
    if (stopped_.load()) {
      ++synthesis_worker_rejections_;
      return false;
    }
    if (synthesis_worker_.try_submit(std::move(task)) !=
        SubmitResult::accepted) {
      ++synthesis_worker_rejections_;
      return false;
    }
    return true;
  }

  void request_pump() {
    submit([this](const std::stop_token stop_token) { pump(stop_token); });
  }

  void record_failure(const TtsFailureCategory category) noexcept {
    const std::scoped_lock lock{state_mutex_};
    last_failure_category_ = tts_failure_category_name(category);
  }

  SpeechEnqueueStatus enqueue_static(
      const std::string &session_id, const std::string &guild_id,
      const std::string_view kind, const PcmAudio &audio,
      const SpeechPriority priority, const std::int64_t expiry_delay_ms,
      const std::string_view error_class,
      const std::string_view source_identity, const std::stop_token stop_token,
      const std::string_view text_override = {}) {
    const auto current = wall_now_ms(clock_);
    const auto default_text =
        kind.ends_with("entrance") ? "The vox is open. Sanguinius attends."
        : kind.ends_with("farewell")
            ? "The channel closes. Until we speak again."
            : "The vox falters. Read the channel for details.";
    const auto text = normalize_tts_text(text_override.empty() ? default_text
                                                               : text_override);
    const auto text_hash = sha256_hex(
        std::as_bytes(std::span{text.text.data(), text.text.size()}));
    const auto result = repository_.enqueue(
        {.speech_id = ids_.next_id(),
         .voice_session_id = session_id,
         .source_event_id = source_identity.empty()
                                ? std::nullopt
                                : std::optional<std::string>{source_identity},
         .source_kind = "static_" + std::string{kind},
         .text = text,
         .text_hash = text_hash,
         .provider = "static",
         .model = "static-v1",
         .voice = repository_.selected_voice(guild_id),
         .priority = priority,
         .earliest_at_ms = current,
         .expires_at_ms = current + expiry_delay_ms,
         .interruptible = true,
         .deduplication_key =
             "speech:static:" + session_id + ":" + std::string{kind} +
             (!source_identity.empty() ? ":" + std::string{source_identity}
              : kind == "error"        ? ":" + std::string{error_class} + ":" +
                                             std::to_string(current / 60'000)
                                       : std::string{}),
         .created_at_ms = current});
    std::optional<SpeechItem> playing;
    {
      const std::scoped_lock lock{state_mutex_};
      if (kind == "contextual_farewell" && result.item &&
          (result.status == SpeechEnqueueStatus::accepted ||
           result.status == SpeechEnqueueStatus::replay))
        pending_contextual_farewell_[session_id] = audio;
      if (result.status == SpeechEnqueueStatus::accepted && playing_ &&
          priority == SpeechPriority::critical_control &&
          playing_->interruptible && playing_->priority < priority)
        playing = playing_;
    }
    if (playing) {
      static_cast<void>(gateway_.stop_audio(session_id));
      static_cast<void>(persist_terminal(*playing, SpeechState::failed,
                                         "priority_preempted", stop_token));
    }
    return result.status;
  }

  [[nodiscard]] PcmAudio asset_for(const SpeechItem &item) {
    const auto source_kind = std::string_view{item.source_kind};
    if (source_kind == "static_entrance")
      return assets_.entrance;
    if (source_kind == "static_farewell")
      return assets_.farewell;
    if (source_kind == "static_contextual_farewell") {
      const std::scoped_lock lock{state_mutex_};
      const auto found =
          pending_contextual_farewell_.find(item.voice_session_id);
      if (found != pending_contextual_farewell_.end()) {
        auto audio = std::move(found->second);
        pending_contextual_farewell_.erase(found);
        return audio;
      }
      return assets_.farewell;
    }
    return assets_.error;
  }

  [[nodiscard]] static bool
  automatic_policy_applies(const SpeechItem &item) noexcept {
    return item.priority == SpeechPriority::event_narration ||
           item.source_kind == "static_contextual_farewell";
  }

  [[nodiscard]] bool
  automatic_item_is_suppressed(const SpeechItem &item) noexcept {
    return automatic_policy_applies(item) &&
           automatic_narration_is_suppressed();
  }

  [[nodiscard]] bool
  narration_transport_deferred(const SpeechItem &item) noexcept {
    if (item.priority != SpeechPriority::event_narration)
      return false;
    const std::scoped_lock lock{state_mutex_};
    return active_session_id_ == item.voice_session_id && !transport_ready_;
  }

  bool suppress_automatic_item(SpeechItem item,
                               const std::stop_token stop_token) noexcept {
    if (!persist_terminal(item, SpeechState::cancelled, "automatic_suppressed",
                          stop_token))
      return false;
    if (item.source_kind != "static_contextual_farewell")
      return true;
    std::optional<std::string> guild_id;
    {
      const std::scoped_lock lock{state_mutex_};
      pending_contextual_farewell_.erase(item.voice_session_id);
      if (active_session_id_ == item.voice_session_id)
        guild_id = active_guild_id_;
    }
    if (!guild_id)
      return true;
    const auto fallback = enqueue_static(
        item.voice_session_id, *guild_id, "farewell", assets_.farewell,
        SpeechPriority::flavor, 15'000, {}, {}, stop_token);
    return fallback == SpeechEnqueueStatus::accepted ||
           fallback == SpeechEnqueueStatus::replay;
  }

  void pump(const std::stop_token stop_token) {
    {
      const std::scoped_lock lock{state_mutex_};
      if (stopped_.load() || playing_ || playback_pending_)
        return;
    }
    std::optional<std::string> session;
    bool muted{};
    {
      const std::scoped_lock lock{state_mutex_};
      session = active_session_id_;
      muted = muted_;
    }
    if (!session)
      return;
    const auto snapshot = gateway_.snapshot(*session);
    if (!snapshot.bound || !snapshot.ready || !snapshot.dave_active)
      return;
    while (true) {
      auto item =
          repository_.claim_next(*session, wall_now_ms(clock_), ids_.next_id(),
                                 "speech:claim:" + ids_.next_id());
      if (!item)
        return;
      if (muted && item->priority != SpeechPriority::interactive) {
        if (!persist_terminal(*item, SpeechState::cancelled, "muted",
                              stop_token))
          return;
        continue;
      }
      if (automatic_item_is_suppressed(*item)) {
        if (!suppress_automatic_item(*item, stop_token))
          return;
        continue;
      }
      try {
        auto audio = prepare(*item, stop_token);
        const auto after_synthesis = wall_now_ms(clock_);
        if (item->expires_at_ms && *item->expires_at_ms <= after_synthesis) {
          if (!persist_terminal(*item, SpeechState::expired,
                                "expired_after_synthesis", stop_token))
            return;
          continue;
        }
        bool cancelled_by_mute{};
        {
          const std::scoped_lock lock{state_mutex_};
          cancelled_by_mute =
              muted_ && item->priority != SpeechPriority::interactive;
        }
        if (cancelled_by_mute) {
          if (!persist_terminal(*item, SpeechState::cancelled, "muted",
                                stop_token))
            return;
          continue;
        }
        if (automatic_item_is_suppressed(*item)) {
          if (!suppress_automatic_item(*item, stop_token))
            return;
          continue;
        }
        item->state = SpeechState::ready;
        ++item->revision;
        item->cache_key = audio.cache_key;
        item->cache_checksum = audio.checksum;
        item->duration_ms = audio.duration_ms;
        item->provider_request_id = audio.provider_request_id;
        const auto marker = item->source_kind.ends_with("farewell")
                                ? "speech:farewell:" + item->speech_id
                                : "speech:" + item->speech_id;
        const auto ready_status = repository_.transition(
            {.speech_id = item->speech_id,
             .expected_revision = item->revision - 1,
             .target = SpeechState::ready,
             .transition_id = ids_.next_id(),
             .reason = "synthesis_ready",
             .idempotency_key = "speech:ready:" + item->speech_id,
             .occurred_at_ms = wall_now_ms(clock_),
             .provider_request_id = item->provider_request_id,
             .cache_key = item->cache_key,
             .cache_checksum = item->cache_checksum,
             .marker = std::nullopt,
             .duration_ms = item->duration_ms,
             .error_code = std::nullopt});
        if (ready_status != SpeechMutationStatus::applied &&
            ready_status != SpeechMutationStatus::unchanged) {
          const auto stored = repository_.find(item->speech_id);
          if (stored && (stored->state == SpeechState::cancelled ||
                         stored->state == SpeechState::expired ||
                         stored->state == SpeechState::played))
            continue;
          if (stored && item->priority == SpeechPriority::event_narration &&
              stored->state == SpeechState::pending) {
            if (narration_transport_deferred(*stored))
              return;
            continue;
          }
          throw TtsError{TtsFailureCategory::unavailable,
                         "Speech readiness lost its persistence fence."};
        }
        if (narration_transport_deferred(*item))
          return;
        if (!submit_playback(*session, snapshot.guild_id.str(), *item,
                             std::move(audio.audio), marker)) {
          record_failure(TtsFailureCategory::unavailable);
          bool failure_persisted{};
          if (!persist_terminal(*item, SpeechState::failed,
                                "playback_queue_rejected", stop_token,
                                &failure_persisted))
            return;
          if (failure_persisted && item->provider == "openai")
            enqueue_error_fallback(*session, snapshot.guild_id.str(),
                                   "playback_queue_rejected", stop_token);
          continue;
        }
        return;
      } catch (const TtsError &error) {
        record_failure(error.category());
        diagnostics_.emit({DiagnosticSeverity::warning, "vox.tts",
                           std::string{"Speech failed with category "} +
                               tts_failure_category_name(error.category()) +
                               ".",
                           item->speech_id});
        bool failure_persisted{};
        if (!persist_terminal(*item, SpeechState::failed,
                              tts_failure_category_name(error.category()),
                              stop_token, &failure_persisted))
          return;
        if (failure_persisted && item->provider == "openai")
          enqueue_error_fallback(*session, snapshot.guild_id.str(),
                                 tts_failure_category_name(error.category()),
                                 stop_token);
      } catch (const std::exception &error) {
        record_failure(TtsFailureCategory::unavailable);
        diagnostics_.emit({DiagnosticSeverity::error, "vox.tts.internal",
                           error.what(), item->speech_id});
        bool failure_persisted{};
        if (!persist_terminal(*item, SpeechState::failed, "internal_error",
                              stop_token, &failure_persisted))
          return;
        if (failure_persisted && item->provider == "openai")
          enqueue_error_fallback(*session, snapshot.guild_id.str(),
                                 "internal_error", stop_token);
      } catch (...) {
        record_failure(TtsFailureCategory::unavailable);
        diagnostics_.emit({DiagnosticSeverity::error, "vox.tts.internal",
                           "Unknown speech-service failure.", item->speech_id});
        bool failure_persisted{};
        if (!persist_terminal(*item, SpeechState::failed, "internal_error",
                              stop_token, &failure_persisted))
          return;
        if (failure_persisted && item->provider == "openai")
          enqueue_error_fallback(*session, snapshot.guild_id.str(),
                                 "internal_error", stop_token);
      }
    }
  }

  struct PreparedAudio {
    PcmAudio audio;
    std::string cache_key;
    std::string checksum;
    std::optional<std::string> provider_request_id;
    std::int64_t duration_ms{};
  };

  bool submit_playback(std::string session_id, std::string guild_id,
                       SpeechItem item, PcmAudio audio,
                       std::string marker) noexcept {
    const auto speech_id = item.speech_id;
    {
      const std::scoped_lock lock{state_mutex_};
      if (stopped_.load() || playing_ || playback_pending_)
        return false;
      playback_pending_ = speech_id;
    }
    auto cancellation = [this, item] {
      clear_playback_pending(item.speech_id);
    };
    const auto status = playback_worker_.try_submit(
        [this, session_id = std::move(session_id),
         guild_id = std::move(guild_id), item = std::move(item),
         audio = std::move(audio),
         marker = std::move(marker)](const std::stop_token stop_token) mutable {
          run_playback_submission(session_id, guild_id, std::move(item),
                                  std::move(audio), std::move(marker),
                                  stop_token);
        },
        std::move(cancellation));
    if (status == SubmitResult::accepted)
      return true;
    ++playback_worker_rejections_;
    clear_playback_pending(speech_id);
    return false;
  }

  void run_playback_submission(const std::string &session_id,
                               const std::string &guild_id, SpeechItem item,
                               PcmAudio audio, std::string marker,
                               const std::stop_token stop_token) noexcept {
    bool accepted{};
    bool cancelled_by_mute{};
    bool cancelled_by_transport{};
    if (item.expires_at_ms && *item.expires_at_ms <= wall_now_ms(clock_)) {
      static_cast<void>(persist_terminal(
          item, SpeechState::expired, "expired_before_playback", stop_token));
      clear_playback_pending(item.speech_id);
      request_pump();
      return;
    }
    if (narration_transport_deferred(item)) {
      clear_playback_pending(item.speech_id);
      return;
    }
    if (automatic_item_is_suppressed(item)) {
      static_cast<void>(suppress_automatic_item(item, stop_token));
      clear_playback_pending(item.speech_id);
      request_pump();
      return;
    }
    try {
      if (stopped_.load() || stop_token.stop_requested())
        throw TtsError{TtsFailureCategory::cancelled,
                       "Speech playback submission was cancelled."};
      const auto status = repository_.transition(
          {.speech_id = item.speech_id,
           .expected_revision = item.revision,
           .target = SpeechState::playing,
           .transition_id = ids_.next_id(),
           .reason = "playback_submitted",
           .idempotency_key = "speech:playing:" + item.speech_id,
           .occurred_at_ms = wall_now_ms(clock_),
           .provider_request_id = std::nullopt,
           .cache_key = std::nullopt,
           .cache_checksum = std::nullopt,
           .marker = marker,
           .duration_ms = std::nullopt,
           .error_code = std::nullopt});
      if (status != SpeechMutationStatus::applied) {
        bool muted_cancellation{};
        {
          const std::scoped_lock lock{state_mutex_};
          muted_cancellation =
              muted_ && item.priority != SpeechPriority::interactive;
        }
        if (muted_cancellation) {
          clear_playback_pending(item.speech_id);
          request_pump();
          return;
        }
        if (narration_transport_deferred(item)) {
          clear_playback_pending(item.speech_id);
          return;
        }
        if (automatic_item_is_suppressed(item)) {
          static_cast<void>(suppress_automatic_item(item, stop_token));
          clear_playback_pending(item.speech_id);
          request_pump();
          return;
        }
        throw TtsError{TtsFailureCategory::unavailable,
                       "Speech playback lost its persistence fence."};
      }
      item.state = SpeechState::playing;
      ++item.revision;
      item.marker = marker;
      const auto cancelled_by_policy = automatic_item_is_suppressed(item);
      if (cancelled_by_policy) {
        static_cast<void>(suppress_automatic_item(item, stop_token));
        clear_playback_pending(item.speech_id);
        request_pump();
        return;
      }
      {
        const std::scoped_lock lock{state_mutex_};
        if (stopped_.load() || active_session_id_ != session_id)
          throw TtsError{TtsFailureCategory::cancelled,
                         "The Vox session closed before playback."};
        if (muted_ && item.priority != SpeechPriority::interactive) {
          cancelled_by_mute = true;
        } else if (item.priority == SpeechPriority::event_narration &&
                   !transport_ready_) {
          cancelled_by_transport = true;
        } else {
          playing_ = item;
          accepted = gateway_.send_pcm(session_id, audio, marker) ==
                     VoiceGatewaySubmit::accepted;
        }
      }
      if (cancelled_by_mute) {
        static_cast<void>(persist_terminal(item, SpeechState::cancelled,
                                           "muted", stop_token));
      } else if (cancelled_by_transport) {
        static_cast<void>(persist_terminal(
            item, SpeechState::cancelled, "transport_unavailable", stop_token));
      } else if (!accepted) {
        record_failure(TtsFailureCategory::unavailable);
        bool failure_persisted{};
        static_cast<void>(persist_terminal(item, SpeechState::failed,
                                           "gateway_rejected", stop_token,
                                           &failure_persisted));
        if (failure_persisted && item.provider == "openai")
          enqueue_error_fallback(session_id, guild_id, "gateway_rejected",
                                 stop_token);
      }
    } catch (const TtsError &error) {
      record_failure(error.category());
      bool failure_persisted{};
      static_cast<void>(
          persist_terminal(item,
                           error.category() == TtsFailureCategory::cancelled
                               ? SpeechState::cancelled
                               : SpeechState::failed,
                           tts_failure_category_name(error.category()),
                           stop_token, &failure_persisted));
      if (error.category() != TtsFailureCategory::cancelled &&
          failure_persisted && item.provider == "openai")
        enqueue_error_fallback(session_id, guild_id,
                               tts_failure_category_name(error.category()),
                               stop_token);
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "vox.tts.playback.internal",
                         error.what(), item.speech_id});
      record_failure(TtsFailureCategory::unavailable);
      bool failure_persisted{};
      static_cast<void>(persist_terminal(item, SpeechState::failed,
                                         "internal_error", stop_token,
                                         &failure_persisted));
      if (failure_persisted && item.provider == "openai")
        enqueue_error_fallback(session_id, guild_id, "internal_error",
                               stop_token);
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "vox.tts.playback.internal",
                         "Unknown playback-submission failure.",
                         item.speech_id});
      record_failure(TtsFailureCategory::unavailable);
      bool failure_persisted{};
      static_cast<void>(persist_terminal(item, SpeechState::failed,
                                         "internal_error", stop_token,
                                         &failure_persisted));
      if (failure_persisted && item.provider == "openai")
        enqueue_error_fallback(session_id, guild_id, "internal_error",
                               stop_token);
    }
    clear_playback_pending(item.speech_id);
    if (!accepted)
      request_pump();
  }

  void clear_playback_pending(const std::string_view speech_id) noexcept {
    const std::scoped_lock lock{state_mutex_};
    if (playback_pending_ == speech_id)
      playback_pending_.reset();
  }

  void clear_playing(const std::string_view speech_id) noexcept {
    const std::scoped_lock lock{state_mutex_};
    if (playing_ && playing_->speech_id == speech_id)
      playing_.reset();
  }

  void clear_playing_for_session(const std::string_view session_id) noexcept {
    const std::scoped_lock lock{state_mutex_};
    if (playing_ && playing_->voice_session_id == session_id)
      playing_.reset();
  }

  PreparedAudio prepare(const SpeechItem &item,
                        const std::stop_token stop_token) {
    if (item.expires_at_ms && *item.expires_at_ms <= wall_now_ms(clock_))
      throw TtsError{TtsFailureCategory::timeout,
                     "The speech item expired before synthesis."};
    if (!item.text)
      throw TtsError{TtsFailureCategory::invalid_request,
                     "Pending speech text is unavailable."};
    if (item.source_kind == "test_provider_failure")
      throw TtsError{TtsFailureCategory::provider_unavailable,
                     "An audited provider failure was simulated."};
    if (item.source_kind == "test_budget_limit")
      throw TtsError{TtsFailureCategory::budget_exhausted,
                     "An audited budget limit was simulated."};
    const auto normalized = normalize_tts_text(*item.text);
    TtsRequest request{.text = normalized.text,
                       .provider = item.provider,
                       .model = item.model,
                       .voice = item.voice};
    const auto key = tts_cache_key(normalized, request);
    if (const auto metadata =
            repository_.cache_metadata(key, wall_now_ms(clock_))) {
      if (auto cached = cache_.read(key, metadata->checksum)) {
        const auto cached_duration = duration_for(*cached);
        return {.audio = std::move(*cached),
                .cache_key = key,
                .checksum = metadata->checksum,
                .provider_request_id = std::nullopt,
                .duration_ms = cached_duration};
      }
      repository_.remove_cache_metadata(key);
    } else {
      cache_.record_miss();
    }

    PcmAudio pcm;
    std::optional<std::string> provider_request_id;
    if (item.provider == "static") {
      pcm = asset_for(item);
    } else {
      if (!configuration_.provider_enabled || client_ == nullptr)
        throw TtsError{TtsFailureCategory::unavailable,
                       "The TTS provider is disabled."};
      pcm = synthesize(item, request, provider_request_id, stop_token);
    }
    if (item.expires_at_ms && *item.expires_at_ms <= wall_now_ms(clock_))
      throw TtsError{TtsFailureCategory::timeout,
                     "The speech item expired during synthesis."};
    const auto bytes = validated_pcm_bytes(pcm);
    const auto checksum = sha256_hex(bytes);
    const auto cache_mutation = cache_.write(key, pcm);
    remove_cache_metadata(cache_mutation);
    const auto current = wall_now_ms(clock_);
    try {
      repository_.put_cache_metadata({.cache_key = key,
                                      .checksum = checksum,
                                      .byte_count = bytes.size(),
                                      .frame_count = bytes.size() / 4,
                                      .provider = item.provider,
                                      .model = item.model,
                                      .voice = item.voice,
                                      .created_at_ms = current,
                                      .last_access_at_ms = current});
    } catch (...) {
      cache_.erase(key);
      throw;
    }
    const auto pcm_duration = duration_for(pcm);
    return {.audio = std::move(pcm),
            .cache_key = key,
            .checksum = checksum,
            .provider_request_id = std::move(provider_request_id),
            .duration_ms = pcm_duration};
  }

  PcmAudio synthesize(const SpeechItem &item, const TtsRequest &request,
                      std::optional<std::string> &provider_request_id,
                      const std::stop_token stop_token) {
    const auto steady_now = std::chrono::steady_clock::now();
    auto deadline = steady_now + configuration_.request_deadline;
    if (item.expires_at_ms) {
      const auto remaining_ms = *item.expires_at_ms - wall_now_ms(clock_);
      if (remaining_ms <= 0)
        throw TtsError{TtsFailureCategory::timeout,
                       "The speech item expired before synthesis."};
      deadline = std::min(deadline,
                          steady_now + std::chrono::milliseconds{remaining_ms});
    }
    for (std::size_t attempt = 1; attempt <= configuration_.maximum_attempts;
         ++attempt) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero())
        throw TtsError{TtsFailureCategory::timeout,
                       "The total TTS request deadline elapsed."};
      const auto attempt_id = ids_.next_id();
      const auto started_at = wall_now_ms(clock_);
      const auto reservation = repository_.reserve_usage(
          {.attempt_id = attempt_id,
           .speech_id = item.speech_id,
           .attempt_number = attempt,
           .provider = item.provider,
           .model = item.model,
           .voice = item.voice,
           .scalar_count = item.scalar_count,
           .estimated_micro_usd =
               estimated_tts_cost_micro_usd(item.scalar_count),
           .now_ms = started_at,
           .calendar_month_start_ms = calendar_month_start_utc_ms(started_at),
           .policy = configuration_.usage_policy});
      if (!reservation.accepted)
        throw TtsError{TtsFailureCategory::budget_exhausted,
                       "The TTS usage ceiling has been reached."};
      try {
        auto attempt_request = request;
        attempt_request.timeout = remaining;
        const auto media = client_->synthesize(attempt_request, stop_token);
        provider_request_id = media.provider_request_id.empty()
                                  ? std::nullopt
                                  : std::optional{media.provider_request_id};
        const auto normalization_started = std::chrono::steady_clock::now();
        const auto normalized = normalizer_.normalize(
            media, configuration_.normalization_limits, stop_token);
        {
          const std::scoped_lock lock{state_mutex_};
          last_normalization_latency_ms_ =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - normalization_started)
                  .count();
        }
        const auto completed_at = wall_now_ms(clock_);
        static_cast<void>(repository_.complete_usage(
            {.attempt_id = attempt_id,
             .state = "succeeded",
             .provider_request_id = provider_request_id,
             .latency_ms = std::max<std::int64_t>(0, completed_at - started_at),
             .duration_ms = normalized.duration_ms,
             .error_code = std::nullopt,
             .completed_at_ms = completed_at}));
        return normalized.pcm;
      } catch (const TtsError &error) {
        const auto ambiguous =
            error.category() == TtsFailureCategory::transport ||
            error.category() == TtsFailureCategory::timeout;
        static_cast<void>(repository_.complete_usage(
            {.attempt_id = attempt_id,
             .state = ambiguous ? "unknown" : "failed",
             .provider_request_id =
                 error.provider_request_id().empty()
                     ? std::nullopt
                     : std::optional{error.provider_request_id()},
             .latency_ms =
                 std::max<std::int64_t>(0, wall_now_ms(clock_) - started_at),
             .duration_ms = std::nullopt,
             .error_code = tts_failure_category_name(error.category()),
             .completed_at_ms = wall_now_ms(clock_)}));
        if (!error.retryable() || attempt == configuration_.maximum_attempts)
          throw;
        const auto delay =
            std::min(error.retry_after().value_or(
                         std::chrono::milliseconds{250 * attempt}),
                     std::chrono::milliseconds{5'000});
        if (std::chrono::steady_clock::now() + delay >= deadline)
          throw TtsError{TtsFailureCategory::timeout,
                         "The total TTS request deadline elapsed."};
        stop_aware_delay(stop_token, delay);
      }
    }
    throw TtsError{TtsFailureCategory::unavailable,
                   "TTS attempts were exhausted."};
  }

  bool
  persist_terminal(SpeechItem item, const SpeechState target,
                   const std::string_view reason,
                   const std::stop_token stop_token,
                   bool *const target_transition_applied = nullptr) noexcept {
    if (target_transition_applied)
      *target_transition_applied = false;
    bool reported{};
    auto delay = std::chrono::milliseconds{25};
    const auto idempotency_key =
        "speech:terminal:" + std::string{speech_state_name(target)} + ":" +
        item.speech_id;
    while (!stop_token.stop_requested() && !stopped_.load()) {
      try {
        const auto status = repository_.transition(
            {.speech_id = item.speech_id,
             .expected_revision = item.revision,
             .target = target,
             .transition_id = ids_.next_id(),
             .reason = std::string{reason},
             .idempotency_key = idempotency_key,
             .occurred_at_ms = wall_now_ms(clock_),
             .provider_request_id = std::nullopt,
             .cache_key = std::nullopt,
             .cache_checksum = std::nullopt,
             .marker = item.marker,
             .duration_ms = std::nullopt,
             .error_code = target == SpeechState::played
                               ? std::nullopt
                               : std::optional<std::string>{reason}});
        if (status == SpeechMutationStatus::applied) {
          if (target_transition_applied)
            *target_transition_applied = true;
          clear_playing(item.speech_id);
          return true;
        }
        if (status == SpeechMutationStatus::unchanged ||
            status == SpeechMutationStatus::not_found) {
          clear_playing(item.speech_id);
          return true;
        }
        const auto stored = repository_.find(item.speech_id);
        if (!stored || terminal_speech_state(stored->state)) {
          clear_playing(item.speech_id);
          return true;
        }
        item = *stored;
      } catch (...) {
      }
      if (!reported) {
        reported = true;
        try {
          diagnostics_.emit(
              {DiagnosticSeverity::error, "vox.tts.persistence",
               "A speech terminal transition is waiting for persistence.",
               item.speech_id});
        } catch (...) {
        }
      }
      stop_aware_delay(stop_token, delay);
      delay = std::min(delay * 2, std::chrono::milliseconds{1'000});
    }
    return false;
  }

  bool fail_playing(const std::string_view session_id,
                    const std::string_view reason,
                    const std::stop_token stop_token) noexcept {
    std::optional<SpeechItem> playing;
    {
      const std::scoped_lock lock{state_mutex_};
      if (playing_ && playing_->voice_session_id == session_id)
        playing = playing_;
    }
    return !playing ||
           persist_terminal(*playing, SpeechState::failed, reason, stop_token);
  }

  bool cancel_session_until_persisted(
      const std::string_view session_id, const std::string_view reason,
      const bool include_interactive, const bool preserve_event_narration,
      const std::stop_token stop_token) noexcept {
    bool reported{};
    auto delay = std::chrono::milliseconds{25};
    while (!stop_token.stop_requested() && !stopped_.load()) {
      try {
        static_cast<void>(repository_.cancel_session(
            session_id, wall_now_ms(clock_), reason, include_interactive,
            preserve_event_narration));
        clear_playing_for_session(session_id);
        return true;
      } catch (...) {
      }
      if (!reported) {
        reported = true;
        try {
          diagnostics_.emit(
              {DiagnosticSeverity::error, "vox.tts.persistence",
               "Speech-session cancellation is waiting for persistence.",
               std::string{session_id}});
        } catch (...) {
        }
      }
      stop_aware_delay(stop_token, delay);
      delay = std::min(delay * 2, std::chrono::milliseconds{1'000});
    }
    return false;
  }

  void enqueue_error_fallback(const std::string &session_id,
                              const std::string &guild_id,
                              const std::string_view error_class,
                              const std::stop_token stop_token) noexcept {
    if (text_fallback_) {
      try {
        text_fallback_(
            ids_.next_id(),
            "Vox could not play a requested line. No speech text or provider "
            "details are exposed; use `/vox status` and try again later.");
      } catch (...) {
      }
    }
    try {
      static_cast<void>(enqueue_static(session_id, guild_id, "error",
                                       assets_.error,
                                       SpeechPriority::critical_control, 30'000,
                                       error_class, {}, stop_token));
    } catch (...) {
    }
  }

  void remove_cache_metadata(const TtsCacheMutationResult &mutation) {
    for (const auto &key : mutation.removed_keys)
      repository_.remove_cache_metadata(key);
  }

  void reconcile_cache() {
    remove_cache_metadata(cache_.purge());
    auto files = cache_.keys();
    auto metadata = repository_.cache_keys();
    std::sort(files.begin(), files.end());
    std::sort(metadata.begin(), metadata.end());
    for (const auto &key : metadata) {
      if (!std::binary_search(files.begin(), files.end(), key))
        repository_.remove_cache_metadata(key);
    }
    for (const auto &key : files) {
      if (!std::binary_search(metadata.begin(), metadata.end(), key))
        cache_.erase(key);
    }
    const auto remaining = cache_.keys();
    for (const auto &key : remaining) {
      if (!std::binary_search(metadata.begin(), metadata.end(), key))
        throw TtsError{TtsFailureCategory::cache_failed,
                       "Unable to remove an orphaned TTS cache file."};
    }
  }

  [[nodiscard]] bool automatic_narration_is_suppressed() noexcept {
    if (!automatic_narration_suppressed_)
      return false;
    try {
      return automatic_narration_suppressed_();
    } catch (...) {
      return true;
    }
  }

  [[nodiscard]] bool
  context_flavor_allowed(const std::string_view session_id) noexcept {
    {
      const std::scoped_lock lock{state_mutex_};
      if (stopped_.load() ||
          !prepared_flavor_.contains(std::string{session_id}) ||
          (active_session_id_ && active_session_id_ != session_id) ||
          (active_session_id_ == session_id && muted_))
        return false;
    }
    return !automatic_narration_is_suppressed();
  }

  [[nodiscard]] bool
  entrance_flavor_allowed(const std::string_view session_id,
                          const std::int64_t expires_at_ms) noexcept {
    {
      const std::scoped_lock lock{state_mutex_};
      const auto found = prepared_flavor_.find(std::string{session_id});
      if (stopped_.load() || found == prepared_flavor_.end() ||
          found->second.entrance_consumed ||
          wall_now_ms(clock_) >= expires_at_ms ||
          (active_session_id_ && active_session_id_ != session_id) ||
          (active_session_id_ == session_id && muted_))
        return false;
    }
    return !automatic_narration_is_suppressed();
  }

  SpeechRepository &repository_;
  TextToSpeechClient *client_;
  AudioNormalizer &normalizer_;
  TtsCache &cache_;
  VoiceGateway &gateway_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  StaticSpeechAssets assets_;
  SpeechServiceConfiguration configuration_;
  SpeechTextFallback text_fallback_;
  std::function<bool()> automatic_narration_suppressed_;
  BoundedExecutor synthesis_worker_;
  BoundedExecutor flavor_worker_;
  BoundedExecutor playback_worker_;
  mutable std::mutex state_mutex_;
  std::optional<std::string> active_session_id_;
  std::optional<std::string> active_guild_id_;
  std::unordered_map<std::string, PreparedSessionFlavor> prepared_flavor_;
  std::unordered_map<std::string, PcmAudio> pending_contextual_farewell_;
  bool muted_{};
  bool transport_ready_{};
  std::optional<SpeechItem> playing_;
  std::optional<std::string> playback_pending_;
  std::optional<std::string> last_failure_category_;
  std::optional<std::int64_t> last_normalization_latency_ms_;
  std::atomic<std::size_t> synthesis_worker_rejections_{0};
  std::atomic<std::size_t> playback_worker_rejections_{0};
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
};

SpeechService::SpeechService(
    SpeechRepository &repository, TextToSpeechClient *client,
    AudioNormalizer &normalizer, TtsCache &cache, VoiceGateway &gateway,
    const Clock &clock, PersistentIdGenerator &ids, Diagnostics &diagnostics,
    StaticSpeechAssets assets, SpeechServiceConfiguration configuration,
    SpeechTextFallback text_fallback,
    std::function<bool()> automatic_narration_suppressed)
    : impl_{
          std::make_unique<Impl>(repository, client, normalizer, cache, gateway,
                                 clock, ids, diagnostics, std::move(assets),
                                 configuration, std::move(text_fallback),
                                 std::move(automatic_narration_suppressed))} {}

SpeechService::~SpeechService() { stop(); }
void SpeechService::start() { impl_->start(); }
void SpeechService::stop() noexcept { impl_->stop(); }
SpeechAdmission SpeechService::say(const std::string_view session_id,
                                   const std::string_view guild_id,
                                   std::string text,
                                   std::string deduplication_key,
                                   const std::int64_t now_ms) {
  return impl_->say(session_id, guild_id, std::move(text),
                    std::move(deduplication_key), now_ms);
}
void SpeechService::session_ready(std::string session_id, std::string guild_id,
                                  const bool muted,
                                  const bool enqueue_entrance) {
  impl_->session_ready(std::move(session_id), std::move(guild_id), muted,
                       enqueue_entrance);
}
void SpeechService::session_reconnecting(const std::string_view session_id) {
  impl_->session_reconnecting(session_id);
}
bool SpeechService::session_leaving(const std::string_view session_id,
                                    const std::string_view guild_id,
                                    const bool allow_contextual) noexcept {
  return impl_->session_leaving(session_id, guild_id, allow_contextual);
}
void SpeechService::session_closed(const std::string_view session_id) noexcept {
  impl_->session_closed(session_id);
}
void SpeechService::set_muted(const std::string_view session_id,
                              const bool muted) {
  impl_->set_muted(session_id, muted);
}

void SpeechService::wake() noexcept { impl_->wake(); }
void SpeechService::begin_session_flavor(std::string session_id) {
  impl_->begin_session_flavor(std::move(session_id));
}
void SpeechService::prepare_session_flavor(std::string session_id,
                                           std::string guild_id,
                                           std::string entrance_line,
                                           std::string farewell_line) {
  impl_->prepare_session_flavor(std::move(session_id), std::move(guild_id),
                                std::move(entrance_line),
                                std::move(farewell_line));
}
void SpeechService::discard_session_flavor(
    const std::string_view session_id) noexcept {
  impl_->discard_session_flavor(session_id);
}
std::optional<PcmAudio> SpeechService::take_prepared_entrance(
    const std::string_view session_id) noexcept {
  return impl_->take_prepared_entrance(session_id);
}
bool SpeechService::track_marker(const std::string_view session_id,
                                 std::string marker) {
  return impl_->track_marker(session_id, std::move(marker));
}
std::string SpeechService::selected_voice(const std::string_view guild_id) {
  return impl_->selected_voice(guild_id);
}
SpeechMutationStatus SpeechService::select_voice(
    const std::string_view guild_id, const std::string_view voice,
    const std::string_view actor_user_id, const std::int64_t now_ms) {
  return impl_->select_voice(guild_id, voice, actor_user_id, now_ms);
}
SpeechServiceHealth SpeechService::health() const { return impl_->health(); }
std::size_t SpeechService::purge() { return impl_->purge(); }
const PcmAudio &SpeechService::entrance_clip() const noexcept {
  return impl_->entrance_clip();
}
bool SpeechService::run_test_scenario(std::string session_id,
                                      std::string guild_id,
                                      std::string scenario,
                                      std::string source_identity) {
  return impl_->run_test_scenario(std::move(session_id), std::move(guild_id),
                                  std::move(scenario),
                                  std::move(source_identity));
}

} // namespace sanguinius
