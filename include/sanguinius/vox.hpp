#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/speech_service.hpp"
#include "sanguinius/voice_gateway.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sanguinius {

inline constexpr std::string_view vox_connect_timeout_job_type{
    "vox.connect_timeout.v1"};
inline constexpr std::string_view vox_reconnect_timeout_job_type{
    "vox.reconnect_timeout.v1"};
inline constexpr std::string_view vox_leave_timeout_job_type{
    "vox.leave_timeout.v1"};
inline constexpr std::string_view vox_empty_timeout_job_type{
    "vox.empty_timeout.v1"};
inline constexpr std::string_view vox_mute_expiry_job_type{
    "vox.mute_expiry.v1"};
inline constexpr std::int64_t vox_connect_timeout_ms = 20'000;
inline constexpr std::int64_t vox_empty_timeout_ms = 60'000;
inline constexpr std::size_t vox_worker_capacity = 64;

enum class VoxState {
  connecting,
  ready,
  muted,
  reconnecting,
  leaving,
  inactive,
  failed,
};

enum class VoxFixtureState {
  pending,
  queued,
  played,
  failed,
};

struct VoxSession {
  std::string session_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake text_channel_id;
  DiscordSnowflake voice_channel_id;
  DiscordSnowflake summoner_user_id;
  std::string deployment_instance_id;
  VoxState state{VoxState::connecting};
  std::size_t revision{1};
  std::uint64_t connection_generation{1};
  std::size_t reconnect_count{};
  VoxFixtureState fixture_state{VoxFixtureState::pending};
  std::optional<std::string> fixture_marker;
  std::optional<std::int64_t> empty_since_ms;
  std::optional<std::string> timeout_job_id;
  std::optional<std::int64_t> muted_at_ms;
  std::optional<std::int64_t> mute_until_ms;
  std::optional<std::string> mute_job_id;
  std::int64_t started_at_ms{};
  std::int64_t last_active_at_ms{};
  std::optional<std::int64_t> ended_at_ms;
  std::optional<std::string> end_reason;
  std::optional<std::string> last_failure_category;
};

enum class VoxResultCode {
  accepted,
  replay,
  inactive,
  active_session,
  unauthorized,
  no_voice,
  unsupported_channel,
  permission_denied,
  channel_full,
  unavailable,
  invalid_state,
  feature_disabled,
  test_mode_disabled,
};

struct VoxCommandResult {
  VoxResultCode code{VoxResultCode::unavailable};
  std::optional<VoxSession> session;
  std::string message;
  bool wake_scheduler{};
  bool wake_outbox{};
};

struct VoxCommandContext {
  DiscordSnowflake guild_id;
  DiscordSnowflake text_channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string interaction_idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
};

struct VoxStartRequest {
  VoxCommandContext context;
  DiscordSnowflake voice_channel_id;
  std::string session_id;
  std::string event_id;
  std::string timeout_job_id;
  std::string deployment_instance_id;
};

struct VoxTransitionRequest {
  std::string session_id;
  std::size_t expected_revision{};
  VoxState target{VoxState::failed};
  std::string reason;
  std::optional<DiscordSnowflake> actor_user_id;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
  std::optional<std::string> timeout_job_id;
  std::optional<std::int64_t> timeout_due_at_ms;
  std::optional<std::string> failure_category;
  bool public_card{};
};

struct VoxFixtureRequest {
  std::string session_id;
  std::size_t expected_revision{};
  VoxFixtureState target{VoxFixtureState::failed};
  std::string marker;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
  std::int64_t now_ms{};
  std::optional<std::string> failure_category;
};

struct VoxOccupancyRequest {
  std::string session_id;
  std::size_t expected_revision{};
  std::size_t human_count{};
  std::int64_t now_ms{};
  std::optional<std::string> empty_job_id;
  std::string event_id;
  std::string idempotency_key;
  std::string correlation_id;
};

class VoxRepository {
public:
  virtual ~VoxRepository() = default;
  [[nodiscard]] virtual VoxCommandResult
  preflight_summon(const VoxCommandContext &context) = 0;
  [[nodiscard]] virtual VoxCommandResult
  record_summon_rejection(const VoxCommandContext &context, VoxResultCode code,
                          std::string message) = 0;
  [[nodiscard]] virtual VoxCommandResult
  start(const VoxStartRequest &request) = 0;
  [[nodiscard]] virtual VoxCommandResult
  finalize_summon(const VoxCommandContext &context, std::string_view session_id,
                  std::size_t expected_revision, bool gateway_accepted,
                  std::string event_id) = 0;
  [[nodiscard]] virtual VoxCommandResult
  command_status(const VoxCommandContext &context) = 0;
  [[nodiscard]] virtual VoxCommandResult
  command_leave(const VoxCommandContext &context, std::string event_id,
                std::string timeout_job_id) = 0;
  [[nodiscard]] virtual VoxCommandResult
  command_mute(const VoxCommandContext &context, bool unmute,
               std::optional<std::int64_t> mute_until_ms, std::string event_id,
               std::optional<std::string> mute_job_id) = 0;
  [[nodiscard]] virtual std::optional<VoxCommandResult>
  command_receipt(const VoxCommandContext &context, std::string_view operation,
                  std::string_view request_fingerprint) = 0;
  [[nodiscard]] virtual VoxCommandResult record_command_receipt(
      const VoxCommandContext &context, std::string_view operation,
      std::string_view request_fingerprint, VoxCommandResult result) = 0;
  [[nodiscard]] virtual VoxCommandResult
  command_test_disconnect(const VoxCommandContext &context,
                          std::string event_id, std::string timeout_job_id) = 0;
  [[nodiscard]] virtual VoxCommandResult
  transition(const VoxTransitionRequest &request,
             std::optional<std::string> outbox_id = std::nullopt) = 0;
  [[nodiscard]] virtual VoxCommandResult
  fixture(const VoxFixtureRequest &request) = 0;
  [[nodiscard]] virtual VoxCommandResult
  occupancy(const VoxOccupancyRequest &request) = 0;
  [[nodiscard]] virtual VoxCommandResult
  handle_timeout(const ClaimedScheduledJob &job, std::int64_t now_ms,
                 std::string event_id, std::string outbox_id,
                 std::string fixture_event_id,
                 std::optional<std::size_t> observed_humans) = 0;
  [[nodiscard]] virtual WorkMutationStatus
  fail_timeout_job(const ClaimedScheduledJob &job, std::int64_t now_ms,
                   std::int64_t retry_at_ms, std::string error_code,
                   bool retryable) = 0;
  [[nodiscard]] virtual std::optional<VoxSession> active() = 0;
  [[nodiscard]] virtual std::size_t recover(std::string_view instance_id,
                                            std::int64_t now_ms,
                                            std::string event_id,
                                            std::string fixture_event_id) = 0;
  [[nodiscard]] virtual VoxCommandResult
  shutdown(std::int64_t now_ms, std::string event_id,
           std::string fixture_event_id,
           std::optional<std::string> queued_fixture_failure_category =
               std::nullopt) = 0;
};

struct VoxHealth {
  bool enabled{};
  std::optional<VoxState> state;
  std::optional<VoxFixtureState> fixture_state;
  bool dave_active{};
  std::size_t reconnect_count{};
  std::size_t callback_drops{};
  std::size_t reconciliations{};
  QueueSnapshot queue;
  std::optional<std::string> last_failure_category;
  std::optional<SpeechServiceHealth> speech;
};

class VoxService {
public:
  using Completion = std::function<void(VoxCommandResult)>;
  using Wake = std::function<void()>;
  using PrepareSessionFlavor =
      std::function<void(std::string, std::string, std::string)>;

  VoxService(VoxRepository &repository, VoiceGateway &gateway,
             const Clock &clock, PersistentIdGenerator &ids,
             Diagnostics &diagnostics, ServerScopeConfiguration scope,
             ControlConfiguration controls, std::string instance_id,
             Wake wake_scheduler, Wake wake_outbox,
             std::size_t queue_capacity = vox_worker_capacity,
             SpeechService *speech = nullptr,
             bool contextual_narration_enabled = false,
             std::function<bool()> automatic_quiet = {},
             PrepareSessionFlavor prepare_session_flavor = {});
  ~VoxService();

  VoxService(const VoxService &) = delete;
  VoxService &operator=(const VoxService &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult summon(VoxCommandContext context,
                                    Completion completion);
  [[nodiscard]] SubmitResult status(VoxCommandContext context,
                                    Completion completion);
  [[nodiscard]] SubmitResult leave(VoxCommandContext context,
                                   Completion completion);
  [[nodiscard]] SubmitResult say(VoxCommandContext context, std::string text,
                                 Completion completion);
  [[nodiscard]] SubmitResult mute(VoxCommandContext context,
                                  std::string duration, Completion completion);
  [[nodiscard]] SubmitResult voice(VoxCommandContext context,
                                   std::optional<std::string> voice,
                                   Completion completion);
  [[nodiscard]] SubmitResult test_disconnect(VoxCommandContext context,
                                             Completion completion);
  [[nodiscard]] SubmitResult speech_test(VoxCommandContext context,
                                         std::string scenario,
                                         Completion completion);
  [[nodiscard]] SubmitResult handle_timeout(const ClaimedScheduledJob &job);
  [[nodiscard]] VoxHealth health() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] PcmAudio make_vox_proof_chime();
[[nodiscard]] const char *vox_state_name(VoxState state) noexcept;
[[nodiscard]] const char *
vox_fixture_state_name(VoxFixtureState state) noexcept;
[[nodiscard]] bool vox_transition_allowed(VoxState from, VoxState to,
                                          std::string_view reason,
                                          std::size_t reconnect_count) noexcept;
[[nodiscard]] std::string render_vox_status(const VoxSession *session,
                                            std::int64_t now_ms = -1);

} // namespace sanguinius
