#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/transcription.hpp"
#include "sanguinius/vox.hpp"
#include "sanguinius/work_queue.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::string_view voice_transcript_component_prefix{"sgvt:1:"};
inline constexpr std::string_view voice_transcript_modal_prefix{"sgvtm:1:"};
inline constexpr std::string_view voice_listening_stop_prefix{"sgvs:1:"};
inline constexpr std::size_t maximum_voice_window_seconds = 15;
inline constexpr std::size_t maximum_voice_pcm_bytes =
    48'000U * 2U * 2U * maximum_voice_window_seconds;
inline constexpr std::size_t maximum_voice_callback_pcm_bytes =
    48'000U * 2U * 2U;
static_assert(maximum_voice_pcm_bytes == maximum_transcription_pcm_bytes);

enum class VoiceInputCapability {
  disabled,
  unsupported_build,
  unavailable_runtime,
  ready,
};

enum class VoiceListeningState {
  proposed,
  arming_transport,
  arming_indicator,
  active,
  transcribing,
  completed,
  stopped,
  failed,
  abandoned,
};

enum class VoiceInputEventKind {
  membership_changed,
  requester_left,
  empty,
  other_bot_joined,
  connection_changed,
  unavailable,
};

struct VoiceInputEvent {
  VoiceInputEventKind kind{VoiceInputEventKind::unavailable};
  std::string session_id;
  std::uint64_t generation{};
  std::size_t human_count{};
};

struct VoiceInputPresence {
  bool available{};
  bool requester_present{};
  bool other_bot_present{};
  std::size_t human_count{};
};

struct VoiceInputArmRequest {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake voice_channel_id;
  DiscordSnowflake requester_user_id;
  std::uint64_t generation{};
};

class VoiceInputAdapter {
public:
  using AudioCallback = std::function<void(std::string_view, std::uint64_t,
                                           std::span<const std::byte>)>;
  using EventCallback = std::function<void(VoiceInputEvent)>;

  virtual ~VoiceInputAdapter() = default;
  [[nodiscard]] virtual VoiceInputCapability capability() const noexcept = 0;
  virtual void start(AudioCallback audio_callback,
                     EventCallback event_callback) = 0;
  [[nodiscard]] virtual VoiceInputPresence
  preflight(const VoiceInputArmRequest &request) const = 0;
  [[nodiscard]] virtual bool
  enable_transport(const VoiceInputArmRequest &request,
                   std::stop_token stop_token) = 0;
  [[nodiscard]] virtual bool arm(const VoiceInputArmRequest &request) = 0;
  virtual void disarm() noexcept = 0;
  [[nodiscard]] virtual bool disable_transport() noexcept = 0;
  virtual void shutdown() noexcept = 0;
};

class SecureAudioBuffer final {
public:
  explicit SecureAudioBuffer(std::size_t capacity,
                             bool require_memory_lock = true);
  ~SecureAudioBuffer();

  SecureAudioBuffer(const SecureAudioBuffer &) = delete;
  SecureAudioBuffer &operator=(const SecureAudioBuffer &) = delete;
  SecureAudioBuffer(SecureAudioBuffer &&other) noexcept;
  SecureAudioBuffer &operator=(SecureAudioBuffer &&other) noexcept;

  [[nodiscard]] bool append(std::span<const std::byte> audio) noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  void scrub() noexcept;
  [[nodiscard]] bool all_zero_for_test() const noexcept;

private:
  void release() noexcept;
  void *mapping_{};
  std::size_t capacity_{};
  std::size_t size_{};
  bool locked_{};
};

[[nodiscard]] bool disable_process_core_dumps() noexcept;

struct TranscriptionUsagePolicy {
  std::size_t rolling_day_windows{10};
  std::int64_t rolling_day_micro_usd{50'000};
  std::int64_t calendar_month_micro_usd{1'000'000};
};

struct VoiceListeningConfiguration {
  bool enabled{};
  bool guild_consent_attested{};
  bool provider_enabled{};
  std::string model{transcription_model};
  std::chrono::milliseconds request_timeout{30'000};
  TranscriptionUsagePolicy usage_policy;
  std::size_t queue_capacity{32};
  bool require_memory_lock{true};
  std::chrono::milliseconds transcript_draft_lifetime{std::chrono::minutes{5}};
  std::chrono::milliseconds public_status_timeout{std::chrono::seconds{10}};
};

struct VoiceListeningWindow {
  std::string window_id;
  std::string vox_session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake text_channel_id;
  DiscordSnowflake voice_channel_id;
  DiscordSnowflake requester_user_id;
  VoiceListeningState state{VoiceListeningState::proposed};
  std::size_t revision{1};
  std::uint64_t connection_generation{};
  std::size_t requested_seconds{};
  std::size_t initial_human_count{};
  std::int64_t reserved_micro_usd{};
  bool provider_attempt_started{};
  std::optional<std::int64_t> provider_attempt_started_at_ms;
  std::int64_t created_at_ms{};
  std::optional<std::int64_t> active_at_ms;
  std::optional<std::int64_t> ended_at_ms;
  std::optional<DiscordSnowflake> public_message_id;
  std::optional<std::string> terminal_reason;
};

enum class VoiceWindowBeginCode {
  created,
  replay,
  active_window,
  window_limit,
  daily_budget,
  monthly_budget,
  kill_switch,
  consent_missing,
};

struct VoiceWindowBeginRequest {
  VoiceListeningWindow window;
  std::string interaction_idempotency_key;
  std::string request_fingerprint;
  std::string transition_id;
};

struct VoiceWindowBeginResult {
  VoiceWindowBeginCode code{VoiceWindowBeginCode::active_window};
  std::optional<VoiceListeningWindow> window;
};

struct VoiceWindowTransitionRequest {
  std::string window_id;
  std::size_t expected_revision{};
  VoiceListeningState target{VoiceListeningState::failed};
  std::string reason;
  std::optional<DiscordSnowflake> actor_user_id;
  std::string transition_id;
  std::string idempotency_key;
  std::int64_t now_ms{};
};

struct VoiceTranscriptionUsage {
  std::string window_id;
  std::string provider{"openai"};
  std::string model{transcription_model};
  std::optional<std::string> provider_request_id;
  std::size_t captured_bytes{};
  std::int64_t captured_duration_ms{};
  std::int64_t estimated_micro_usd{};
  std::int64_t latency_ms{};
  std::string result_code;
  bool provider_sent{};
  std::int64_t recorded_at_ms{};
};

struct VoiceListeningRepositoryHealth {
  std::size_t active_windows{};
  std::size_t day_windows{};
  std::int64_t day_micro_usd{};
  std::int64_t month_micro_usd{};
  bool kill_switch{};
  std::optional<std::string> last_result_code;
};

class VoiceListeningRepository {
public:
  virtual ~VoiceListeningRepository() = default;
  virtual void record_consent_attestation(bool attested,
                                          DiscordSnowflake owner_user_id,
                                          std::string attestation_id,
                                          std::int64_t now_ms) = 0;
  [[nodiscard]] virtual VoiceWindowBeginResult
  begin(const VoiceWindowBeginRequest &request,
        const TranscriptionUsagePolicy &policy) = 0;
  [[nodiscard]] virtual std::optional<VoiceListeningWindow> active() = 0;
  [[nodiscard]] virtual std::optional<VoiceListeningWindow>
  transition(const VoiceWindowTransitionRequest &request) = 0;
  // Persist a successful provider usage record and the completed state
  // transition in one transaction. Implementations must commit both or
  // neither, including when either write fails.
  [[nodiscard]] virtual std::optional<VoiceListeningWindow>
  complete_transcription(const VoiceWindowTransitionRequest &request,
                         const VoiceTranscriptionUsage &usage) = 0;
  virtual void record_public_message(std::string_view window_id,
                                     DiscordSnowflake message_id,
                                     std::int64_t now_ms) = 0;
  // Recording known-unsent usage must atomically release its monetary
  // reservation. Implementations must leave both records unchanged on error.
  virtual void record_usage(const VoiceTranscriptionUsage &usage) = 0;
  virtual void record_provider_attempt(std::string_view window_id,
                                       std::int64_t now_ms) = 0;
  virtual void release_reservation(std::string_view window_id,
                                   std::int64_t now_ms) = 0;
  [[nodiscard]] virtual bool kill_switch_enabled() = 0;
  virtual void set_kill_switch(bool enabled, DiscordSnowflake actor_user_id,
                               std::string change_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t
  abandon_nonterminal(std::int64_t now_ms, std::string_view reason,
                      std::string_view transition_prefix) = 0;
  [[nodiscard]] virtual VoiceListeningRepositoryHealth
  health(std::int64_t now_ms) = 0;
};

struct ActiveVoxListeningContext {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake text_channel_id;
  DiscordSnowflake voice_channel_id;
  std::uint64_t connection_generation{};
  bool ready{};
  bool speech_idle{};
};

struct VoiceListeningHealth {
  VoiceInputCapability capability{VoiceInputCapability::disabled};
  bool configured_enabled{};
  bool consent_attested{};
  bool provider_enabled{};
  std::optional<VoiceListeningState> state;
  VoiceListeningRepositoryHealth repository;
  QueueSnapshot control_queue;
  QueueSnapshot privacy_queue;
  QueueSnapshot transcription_queue;
  std::size_t callback_drops{};
  std::size_t volatile_transcript_drafts{};
  std::optional<std::string> last_failure_category;
};

class ChronicleService;

class VoiceListeningService final {
public:
  using Completion = std::function<void(InteractionMessage)>;
  using ConfirmedCompletion =
      std::function<void(InteractionMessage, DeliveryCallback)>;
  using VoxContextProvider =
      std::function<std::optional<ActiveVoxListeningContext>()>;
  using SpeechExclusion = std::function<void(bool)>;

  VoiceListeningService(
      VoiceListeningRepository &repository, VoiceInputAdapter &adapter,
      TranscriptionClient *transcription,
      DiscordPublicDelivery &public_delivery, const Clock &clock,
      PersistentIdGenerator &ids, Diagnostics &diagnostics,
      ServerScopeConfiguration scope, VoiceListeningConfiguration configuration,
      VoxContextProvider vox_context, ChronicleService *chronicle = nullptr,
      SpeechExclusion speech_exclusion = {});
  ~VoiceListeningService();

  VoiceListeningService(const VoiceListeningService &) = delete;
  VoiceListeningService &operator=(const VoiceListeningService &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult listen_start(IncomingInteraction interaction,
                                          std::size_t duration_seconds,
                                          Completion completion);
  [[nodiscard]] SubmitResult listen_start(IncomingInteraction interaction,
                                          std::size_t duration_seconds,
                                          ConfirmedCompletion completion);
  [[nodiscard]] SubmitResult
  listen_stop(IncomingInteraction interaction, Completion completion,
              std::optional<std::string> window_id = std::nullopt);
  [[nodiscard]] SubmitResult set_kill_switch(
      IncomingInteraction interaction, bool disabled, Completion completion,
      std::optional<std::uint64_t> disable_generation = std::nullopt);
  [[nodiscard]] std::uint64_t preempt_privacy_abort(
      DiscordSnowflake actor_user_id, bool latch_kill_switch,
      std::optional<std::string> window_id = std::nullopt) noexcept;
  [[nodiscard]] std::uint64_t disable_generation() const noexcept;
  [[nodiscard]] std::optional<ModalPayload>
  transcript_modal(const IncomingInteraction &interaction);
  void transcript_modal_delivery(const IncomingInteraction &interaction,
                                 DeliveryResult result) noexcept;
  [[nodiscard]] InteractionMessage
  propose_transcript(const IncomingInteraction &interaction);
  [[nodiscard]] VoiceListeningHealth health() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool
voice_listening_transition_allowed(VoiceListeningState from,
                                   VoiceListeningState to) noexcept;
[[nodiscard]] const char *
voice_listening_state_name(VoiceListeningState state) noexcept;
[[nodiscard]] const char *
voice_input_capability_name(VoiceInputCapability capability) noexcept;
[[nodiscard]] std::optional<std::string>
parse_voice_control(std::string_view custom_id, std::string_view prefix);

} // namespace sanguinius
