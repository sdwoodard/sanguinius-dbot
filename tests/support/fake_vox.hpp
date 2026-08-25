#pragma once

#include "sanguinius/vox.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeVoxRepository final : public VoxRepository {
public:
  void seed_active(const DiscordSnowflake guild_id = 10,
                   const DiscordSnowflake text_channel_id = 20,
                   const DiscordSnowflake voice_channel_id = 40,
                   const DiscordSnowflake summoner_user_id = 31) {
    const std::scoped_lock lock{mutex_};
    session_ = VoxSession{
        .session_id = "00000000-0000-4000-8000-000000000990",
        .guild_id = guild_id,
        .text_channel_id = text_channel_id,
        .voice_channel_id = voice_channel_id,
        .summoner_user_id = summoner_user_id,
        .deployment_instance_id = "00000000-0000-4000-8000-000000000989",
        .state = VoxState::ready,
        .revision = 2,
        .connection_generation = 1,
        .reconnect_count = 0,
        .fixture_state = VoxFixtureState::played,
        .fixture_marker = "vox-proof:seeded",
        .empty_since_ms = std::nullopt,
        .timeout_job_id = std::nullopt,
        .started_at_ms = 0,
        .last_active_at_ms = 0,
        .ended_at_ms = std::nullopt,
        .end_reason = std::nullopt,
        .last_failure_category = std::nullopt,
    };
    changed_.notify_all();
  }

  VoxCommandResult preflight_summon(const VoxCommandContext &) override {
    const std::scoped_lock lock{mutex_};
    return result(session_ ? VoxResultCode::active_session
                           : VoxResultCode::accepted,
                  session_, session_ ? "active" : "");
  }

  VoxCommandResult record_summon_rejection(const VoxCommandContext &,
                                           VoxResultCode code,
                                           std::string message) override {
    return result(code, std::nullopt, std::move(message));
  }

  VoxCommandResult start(const VoxStartRequest &request) override {
    const std::scoped_lock lock{mutex_};
    if (session_)
      return result(VoxResultCode::active_session, session_, "active");
    session_ = VoxSession{
        .session_id = request.session_id,
        .guild_id = request.context.guild_id,
        .text_channel_id = request.context.text_channel_id,
        .voice_channel_id = request.voice_channel_id,
        .summoner_user_id = request.context.actor_user_id,
        .deployment_instance_id = request.deployment_instance_id,
        .state = VoxState::connecting,
        .revision = 1,
        .connection_generation = 1,
        .reconnect_count = 0,
        .fixture_state = VoxFixtureState::pending,
        .fixture_marker = std::nullopt,
        .empty_since_ms = std::nullopt,
        .timeout_job_id = request.timeout_job_id,
        .started_at_ms = request.context.now_ms,
        .last_active_at_ms = request.context.now_ms,
        .ended_at_ms = std::nullopt,
        .end_reason = std::nullopt,
        .last_failure_category = std::nullopt,
    };
    changed_.notify_all();
    auto accepted = result(VoxResultCode::accepted, session_, "connecting");
    accepted.wake_scheduler = true;
    return accepted;
  }

  VoxCommandResult finalize_summon(const VoxCommandContext &,
                                   std::string_view session_id,
                                   std::size_t expected_revision,
                                   bool gateway_accepted,
                                   std::string) override {
    const std::scoped_lock lock{mutex_};
    if (!session_ || session_->session_id != session_id ||
        session_->revision != expected_revision)
      return result(VoxResultCode::invalid_state, session_);
    if (gateway_accepted)
      return result(VoxResultCode::accepted, session_, "connecting");
    auto rejected =
        transition_unlocked(VoxState::failed, session_->last_active_at_ms,
                            "gateway_rejected", false, std::nullopt);
    rejected.code = VoxResultCode::unavailable;
    rejected.message = "Vox could not begin a new session.";
    rejected.session->last_failure_category = "gateway_unavailable";
    session_->last_failure_category = "gateway_unavailable";
    return rejected;
  }

  VoxCommandResult command_status(const VoxCommandContext &) override {
    const std::scoped_lock lock{mutex_};
    return result(session_ ? VoxResultCode::accepted : VoxResultCode::inactive,
                  session_);
  }

  VoxCommandResult command_leave(const VoxCommandContext &context, std::string,
                                 std::string timeout_job_id) override {
    const std::scoped_lock lock{mutex_};
    if (!active_unlocked())
      return result(VoxResultCode::inactive);
    if (context.actor_user_id != context.owner_user_id &&
        context.actor_user_id != session_->summoner_user_id)
      return result(VoxResultCode::unauthorized, session_, "unauthorized");
    return transition_unlocked(VoxState::leaving, context.now_ms,
                               "commanded_leave", false,
                               std::move(timeout_job_id));
  }

  VoxCommandResult
  command_test_disconnect(const VoxCommandContext &context, std::string,
                          std::string timeout_job_id) override {
    const std::scoped_lock lock{mutex_};
    if (!active_unlocked() || session_->state != VoxState::ready ||
        session_->reconnect_count != 0)
      return result(VoxResultCode::invalid_state, session_);
    return transition_unlocked(VoxState::reconnecting, context.now_ms,
                               "owner_test_disconnect", false,
                               std::move(timeout_job_id));
  }

  VoxCommandResult
  transition(const VoxTransitionRequest &request,
             std::optional<std::string> = std::nullopt) override {
    const std::scoped_lock lock{mutex_};
    if (!session_ || session_->revision != request.expected_revision)
      return result(VoxResultCode::invalid_state, session_);
    auto changed =
        transition_unlocked(request.target, request.now_ms, request.reason,
                            request.public_card, request.timeout_job_id);
    if (changed.session)
      changed.session->last_failure_category = request.failure_category;
    session_->last_failure_category = request.failure_category;
    return changed;
  }

  VoxCommandResult fixture(const VoxFixtureRequest &request) override {
    const std::scoped_lock lock{mutex_};
    if (!session_ || session_->revision != request.expected_revision)
      return result(VoxResultCode::invalid_state, session_);
    session_->fixture_state = request.target;
    session_->fixture_marker = request.marker;
    session_->last_failure_category = request.failure_category;
    ++session_->revision;
    changed_.notify_all();
    return result(VoxResultCode::accepted, session_);
  }

  VoxCommandResult occupancy(const VoxOccupancyRequest &) override {
    const std::scoped_lock lock{mutex_};
    return result(VoxResultCode::accepted, session_);
  }

  VoxCommandResult handle_timeout(const ClaimedScheduledJob &, std::int64_t,
                                  std::string, std::string, std::string,
                                  std::optional<std::size_t>) override {
    return result(VoxResultCode::inactive);
  }

  WorkMutationStatus fail_timeout_job(const ClaimedScheduledJob &, std::int64_t,
                                      std::int64_t, std::string,
                                      bool) override {
    return WorkMutationStatus::applied;
  }

  std::optional<VoxSession> active() override {
    const std::scoped_lock lock{mutex_};
    return active_unlocked() ? session_ : std::nullopt;
  }

  std::size_t recover(std::string_view, std::int64_t now_ms, std::string,
                      std::string) override {
    const std::scoped_lock lock{mutex_};
    ++recover_calls_;
    if (!active_unlocked())
      return 0;
    static_cast<void>(transition_unlocked(
        VoxState::inactive, now_ms, "restart_abandoned", false, std::nullopt));
    return 1;
  }

  VoxCommandResult shutdown(
      std::int64_t now_ms, std::string, std::string,
      std::optional<std::string> queued_fixture_failure_category) override {
    const std::scoped_lock lock{mutex_};
    if (!active_unlocked())
      return result(VoxResultCode::inactive, session_);
    if (session_->fixture_state == VoxFixtureState::queued) {
      session_->fixture_state = VoxFixtureState::failed;
      session_->last_failure_category =
          queued_fixture_failure_category.value_or("playback_interrupted");
      ++session_->revision;
    }
    return transition_unlocked(VoxState::inactive, now_ms, "shutdown", false,
                               std::nullopt);
  }

  [[nodiscard]] bool
  wait_for_state(const VoxState state,
                 const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, state] {
      return session_ && session_->state == state;
    });
  }

  [[nodiscard]] bool
  wait_for_fixture(const VoxFixtureState state,
                   const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, state] {
      return session_ && session_->fixture_state == state;
    });
  }

  [[nodiscard]] std::size_t recover_calls() const {
    const std::scoped_lock lock{mutex_};
    return recover_calls_;
  }

private:
  static VoxCommandResult
  result(const VoxResultCode code,
         std::optional<VoxSession> session = std::nullopt,
         std::string message = {}) {
    return {.code = code,
            .session = std::move(session),
            .message = std::move(message),
            .wake_scheduler = false,
            .wake_outbox = false};
  }

  [[nodiscard]] bool active_unlocked() const {
    return session_ && session_->state != VoxState::inactive &&
           session_->state != VoxState::failed;
  }

  VoxCommandResult transition_unlocked(VoxState target, std::int64_t now_ms,
                                       std::string reason, bool public_card,
                                       std::optional<std::string> timeout_job) {
    session_->state = target;
    ++session_->revision;
    if (target == VoxState::reconnecting) {
      ++session_->reconnect_count;
      ++session_->connection_generation;
    }
    session_->timeout_job_id = std::move(timeout_job);
    session_->last_active_at_ms = now_ms;
    if (target == VoxState::inactive || target == VoxState::failed) {
      session_->ended_at_ms = now_ms;
      session_->end_reason = std::move(reason);
    }
    changed_.notify_all();
    auto changed = result(VoxResultCode::accepted, session_);
    changed.wake_scheduler = session_->timeout_job_id.has_value();
    changed.wake_outbox = public_card;
    return changed;
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::optional<VoxSession> session_;
  std::size_t recover_calls_{};
};

class FakeVoiceGateway final : public VoiceGateway {
public:
  void start(EventCallback callback) override {
    const std::scoped_lock lock{mutex_};
    callback_ = std::move(callback);
    lifecycle_.push_back("voice.start");
  }

  void resolve_member_channel(DiscordSnowflake, DiscordSnowflake,
                              ResolveCallback callback) override {
    VoiceResolvedChannel resolution;
    {
      const std::scoped_lock lock{mutex_};
      resolution = resolution_;
    }
    callback(std::move(resolution));
  }

  VoiceGatewaySubmit connect(const VoiceConnectRequest &request) override {
    const std::scoped_lock lock{mutex_};
    request_ = request;
    ++connect_count_;
    snapshot_ = {.bound = true,
                 .connected = true,
                 .ready = false,
                 .dave_active = false,
                 .marker_completed = false,
                 .bot_moved = false,
                 .session_id = request.session_id,
                 .guild_id = request.guild_id,
                 .channel_id = request.channel_id,
                 .observed_channel_id = request.channel_id,
                 .generation = request.generation,
                 .human_count = resolution_.human_count};
    lifecycle_.push_back("voice.connect");
    changed_.notify_all();
    return connect_result_;
  }

  VoiceGatewaySubmit disconnect(std::string_view) override {
    const std::scoped_lock lock{mutex_};
    ++disconnect_count_;
    lifecycle_.push_back("voice.disconnect");
    changed_.notify_all();
    return VoiceGatewaySubmit::accepted;
  }

  void release_binding(std::string_view session_id) noexcept override {
    const std::scoped_lock lock{mutex_};
    if (snapshot_.session_id == session_id)
      snapshot_ = {};
    lifecycle_.push_back("voice.release_binding");
  }

  VoiceGatewaySubmit stop_audio(std::string_view) override {
    const std::scoped_lock lock{mutex_};
    ++stop_audio_count_;
    lifecycle_.push_back("voice.stop_audio");
    return VoiceGatewaySubmit::accepted;
  }

  VoiceGatewaySubmit send_pcm(std::string_view, const PcmAudio &audio,
                              std::string_view marker) override {
    const std::scoped_lock lock{mutex_};
    ++send_count_;
    audio_ = audio;
    marker_ = marker;
    lifecycle_.push_back("voice.send_pcm");
    changed_.notify_all();
    return send_result_;
  }

  VoiceGatewaySnapshot
  snapshot(std::string_view session_id) const noexcept override {
    const std::scoped_lock lock{mutex_};
    return snapshot_.session_id == session_id ? snapshot_
                                              : VoiceGatewaySnapshot{};
  }

  void shutdown() noexcept override {
    const std::scoped_lock lock{mutex_};
    callback_ = {};
    lifecycle_.push_back("voice.shutdown");
  }

  void emit(VoiceEventKind kind, bool dave_active = true,
            std::size_t human_count = 1) {
    EventCallback callback;
    VoiceEvent event;
    {
      const std::scoped_lock lock{mutex_};
      callback = callback_;
      event = {.kind = kind,
               .session_id = request_.session_id,
               .guild_id = request_.guild_id,
               .channel_id = request_.channel_id,
               .generation = request_.generation,
               .human_count = human_count,
               .dave_active = dave_active,
               .marker = marker_,
               .failure_category = {}};
      snapshot_.human_count = human_count;
      if (kind == VoiceEventKind::ready) {
        snapshot_.connected = true;
        snapshot_.ready = true;
        snapshot_.dave_active = dave_active;
      } else if (kind == VoiceEventKind::disconnected) {
        snapshot_.connected = false;
        snapshot_.ready = false;
        snapshot_.dave_active = false;
        if (!snapshot_.bot_moved)
          snapshot_.observed_channel_id = {};
      } else if (kind == VoiceEventKind::bot_moved) {
        snapshot_.connected = true;
        snapshot_.ready = false;
        snapshot_.dave_active = false;
        snapshot_.observed_channel_id = request_.channel_id;
        snapshot_.bot_moved = true;
      } else if (kind == VoiceEventKind::track_marker) {
        snapshot_.marker_completed = true;
      }
    }
    if (callback)
      callback(std::move(event));
  }

  [[nodiscard]] bool
  wait_for_connects(std::size_t count,
                    std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return connect_count_ >= count; });
  }

  [[nodiscard]] bool wait_for_sends(std::size_t count,
                                    std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return send_count_ >= count; });
  }

  [[nodiscard]] bool
  wait_for_disconnects(std::size_t count,
                       std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return disconnect_count_ >= count; });
  }

  [[nodiscard]] std::size_t send_count() const {
    const std::scoped_lock lock{mutex_};
    return send_count_;
  }

  [[nodiscard]] std::size_t disconnect_count() const {
    const std::scoped_lock lock{mutex_};
    return disconnect_count_;
  }

  [[nodiscard]] std::vector<std::string> lifecycle() const {
    const std::scoped_lock lock{mutex_};
    return lifecycle_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  EventCallback callback_;
  VoiceResolvedChannel resolution_{.status = VoiceResolveStatus::ready,
                                   .channel_id = 40,
                                   .human_count = 1,
                                   .failure_category = {}};
  VoiceGatewaySubmit connect_result_{VoiceGatewaySubmit::accepted};
  VoiceGatewaySubmit send_result_{VoiceGatewaySubmit::accepted};
  VoiceConnectRequest request_;
  VoiceGatewaySnapshot snapshot_;
  PcmAudio audio_;
  std::string marker_;
  std::vector<std::string> lifecycle_;
  std::size_t connect_count_{};
  std::size_t disconnect_count_{};
  std::size_t stop_audio_count_{};
  std::size_t send_count_{};
};

} // namespace sanguinius::test
