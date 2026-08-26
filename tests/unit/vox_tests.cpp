#include "sanguinius/vox.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] sanguinius::VoxCommandResult
fake_result(sanguinius::VoxResultCode code,
            std::optional<sanguinius::VoxSession> session = std::nullopt,
            std::string message = {}, bool wake_scheduler = false,
            bool wake_outbox = false) {
  return {.code = code,
          .session = std::move(session),
          .message = std::move(message),
          .wake_scheduler = wake_scheduler,
          .wake_outbox = wake_outbox};
}

[[nodiscard]] sanguinius::ClaimedScheduledJob connect_timeout_job() {
  return {.job_id = "00000000-0000-4000-8000-000000000700",
          .job_type = std::string{sanguinius::vox_connect_timeout_job_type},
          .lease_owner = "test-worker",
          .lease_token = "00000000-0000-4000-8000-000000000701",
          .attempt_count = 1,
          .max_attempts = 5,
          .due_at_ms = 20'000,
          .payload =
              sanguinius::VoxTimeoutJobPayload{
                  .session_id = "00000000-0000-4000-8000-000000000001",
                  .expected_revision = 1},
          .correlation_id = "vox-timeout-test",
          .causation_event_id = std::nullopt};
}

class BlockingDiagnostics final : public sanguinius::Diagnostics {
public:
  void emit(const sanguinius::DiagnosticEvent &) noexcept override {
    std::unique_lock lock{mutex_};
    entered_ = true;
    changed_.notify_all();
    static_cast<void>(
        changed_.wait_for(lock, 2s, [this] { return released_; }));
  }

  [[nodiscard]] bool
  wait_for_entry(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() {
    const std::scoped_lock lock{mutex_};
    released_ = true;
    changed_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool entered_{};
  bool released_{};
};

class WaitableCounter final {
public:
  void increment() noexcept {
    try {
      const std::scoped_lock lock{mutex_};
      ++value_;
      changed_.notify_all();
    } catch (...) {
    }
  }

  [[nodiscard]] bool
  wait_for_at_least(const std::size_t expected,
                    const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, expected] { return value_ >= expected; });
  }

  [[nodiscard]] std::size_t load() const {
    const std::scoped_lock lock{mutex_};
    return value_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::size_t value_{};
};

class FakeVoxRepository final : public sanguinius::VoxRepository {
public:
  sanguinius::VoxCommandResult
  preflight_summon(const sanguinius::VoxCommandContext &) override {
    const std::scoped_lock lock{mutex_};
    return fake_result(session_ ? sanguinius::VoxResultCode::active_session
                                : sanguinius::VoxResultCode::accepted,
                       session_, session_ ? "active" : "");
  }

  sanguinius::VoxCommandResult
  record_summon_rejection(const sanguinius::VoxCommandContext &,
                          sanguinius::VoxResultCode code,
                          std::string message) override {
    return fake_result(code, std::nullopt, std::move(message));
  }

  sanguinius::VoxCommandResult
  start(const sanguinius::VoxStartRequest &request) override {
    const std::scoped_lock lock{mutex_};
    last_start_now_ms_ = request.context.now_ms;
    if (start_override_)
      return *start_override_;
    if (session_)
      return fake_result(sanguinius::VoxResultCode::active_session, session_,
                         "active");
    session_ = sanguinius::VoxSession{
        .session_id = request.session_id,
        .guild_id = request.context.guild_id,
        .text_channel_id = request.context.text_channel_id,
        .voice_channel_id = request.voice_channel_id,
        .summoner_user_id = request.context.actor_user_id,
        .deployment_instance_id = request.deployment_instance_id,
        .state = sanguinius::VoxState::connecting,
        .revision = 1,
        .connection_generation = 1,
        .reconnect_count = 0,
        .fixture_state = sanguinius::VoxFixtureState::pending,
        .fixture_marker = std::nullopt,
        .empty_since_ms = std::nullopt,
        .timeout_job_id = request.timeout_job_id,
        .muted_at_ms = std::nullopt,
        .mute_until_ms = std::nullopt,
        .mute_job_id = std::nullopt,
        .started_at_ms = request.context.now_ms,
        .last_active_at_ms = request.context.now_ms,
        .ended_at_ms = std::nullopt,
        .end_reason = std::nullopt,
        .last_failure_category = std::nullopt};
    return fake_result(sanguinius::VoxResultCode::accepted, session_,
                       "connecting", true);
  }

  sanguinius::VoxCommandResult
  finalize_summon(const sanguinius::VoxCommandContext &,
                  std::string_view session_id, std::size_t expected_revision,
                  bool gateway_accepted, std::string) override {
    const std::scoped_lock lock{mutex_};
    if (!session_ || session_->session_id != session_id ||
        session_->revision != expected_revision)
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    if (gateway_accepted)
      return fake_result(sanguinius::VoxResultCode::accepted, session_,
                         "connecting");
    session_->state = sanguinius::VoxState::failed;
    ++session_->revision;
    session_->ended_at_ms = session_->last_active_at_ms;
    session_->end_reason = "gateway_rejected";
    session_->timeout_job_id = std::nullopt;
    session_->last_failure_category = "gateway_unavailable";
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::unavailable, session_,
                       "Vox could not begin a new session.");
  }

  sanguinius::VoxCommandResult
  command_status(const sanguinius::VoxCommandContext &) override {
    std::unique_lock lock{mutex_};
    if (throw_status_)
      throw std::runtime_error{"synthetic repository failure"};
    ++status_calls_;
    changed_.notify_all();
    if (block_status_) {
      status_entered_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return status_released_; });
    }
    return fake_result(session_ ? sanguinius::VoxResultCode::accepted
                                : sanguinius::VoxResultCode::inactive,
                       session_);
  }

  sanguinius::VoxCommandResult
  command_leave(const sanguinius::VoxCommandContext &context, std::string,
                std::string timeout_job_id) override {
    const std::scoped_lock lock{mutex_};
    if (!session_)
      return fake_result(sanguinius::VoxResultCode::inactive);
    if (context.actor_user_id != context.owner_user_id &&
        context.actor_user_id != session_->summoner_user_id)
      return fake_result(sanguinius::VoxResultCode::unauthorized, session_);
    session_->state = sanguinius::VoxState::leaving;
    ++session_->revision;
    session_->timeout_job_id = std::move(timeout_job_id);
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_,
                       "Vox Sanguinius is leaving the voice channel.", true);
  }

  sanguinius::VoxCommandResult
  command_mute(const sanguinius::VoxCommandContext &context, const bool unmute,
               const std::optional<std::int64_t> mute_until_ms, std::string,
               std::optional<std::string> mute_job_id) override {
    const std::scoped_lock lock{mutex_};
    if (!session_)
      return fake_result(sanguinius::VoxResultCode::inactive);
    if (context.actor_user_id != context.owner_user_id &&
        context.actor_user_id != session_->summoner_user_id)
      return fake_result(sanguinius::VoxResultCode::unauthorized, session_);
    if (unmute && session_->state == sanguinius::VoxState::muted) {
      session_->state = sanguinius::VoxState::ready;
      session_->muted_at_ms.reset();
      session_->mute_until_ms.reset();
      session_->mute_job_id.reset();
      session_->timeout_job_id.reset();
    } else if (!unmute && session_->state == sanguinius::VoxState::ready) {
      session_->state = sanguinius::VoxState::muted;
      session_->muted_at_ms = context.now_ms;
      session_->mute_until_ms = mute_until_ms;
      session_->mute_job_id = mute_job_id;
      session_->timeout_job_id = std::move(mute_job_id);
    } else {
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    }
    ++session_->revision;
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_, {},
                       mute_until_ms.has_value());
  }

  std::optional<sanguinius::VoxCommandResult>
  command_receipt(const sanguinius::VoxCommandContext &context,
                  const std::string_view operation,
                  const std::string_view request_fingerprint) override {
    const std::scoped_lock lock{mutex_};
    const auto found =
        command_receipts_.find(context.interaction_idempotency_key);
    if (found == command_receipts_.end())
      return std::nullopt;
    if (found->second.operation != operation ||
        found->second.request_fingerprint != request_fingerprint)
      throw std::invalid_argument{
          "Vox interaction idempotency key was reused."};
    auto replay = found->second.result;
    replay.code = sanguinius::VoxResultCode::replay;
    return replay;
  }

  sanguinius::VoxCommandResult
  record_command_receipt(const sanguinius::VoxCommandContext &context,
                         const std::string_view operation,
                         const std::string_view request_fingerprint,
                         sanguinius::VoxCommandResult result) override {
    const std::scoped_lock lock{mutex_};
    const auto [found, inserted] = command_receipts_.try_emplace(
        context.interaction_idempotency_key,
        CommandReceipt{std::string{operation}, std::string{request_fingerprint},
                       result});
    if (!inserted) {
      if (found->second.operation != operation ||
          found->second.request_fingerprint != request_fingerprint)
        throw std::invalid_argument{
            "Vox interaction idempotency key was reused."};
      auto replay = found->second.result;
      replay.code = sanguinius::VoxResultCode::replay;
      return replay;
    }
    return result;
  }

  sanguinius::VoxCommandResult
  command_test_disconnect(const sanguinius::VoxCommandContext &, std::string,
                          std::string timeout_job_id) override {
    const std::scoped_lock lock{mutex_};
    if (!session_ || session_->state != sanguinius::VoxState::ready ||
        session_->reconnect_count != 0)
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    session_->state = sanguinius::VoxState::reconnecting;
    ++session_->revision;
    ++session_->connection_generation;
    ++session_->reconnect_count;
    session_->timeout_job_id = std::move(timeout_job_id);
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_,
                       "A test disconnect was requested; Vox will rejoin once.",
                       true);
  }

  sanguinius::VoxCommandResult
  transition(const sanguinius::VoxTransitionRequest &request,
             std::optional<std::string> = std::nullopt) override {
    const std::scoped_lock lock{mutex_};
    if (throw_transition_)
      throw std::runtime_error{"synthetic transition failure"};
    if (!session_ || session_->revision != request.expected_revision)
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    last_transition_ = request;
    session_->state = request.target;
    ++session_->revision;
    session_->last_active_at_ms = request.now_ms;
    session_->timeout_job_id = request.timeout_job_id;
    if (request.target == sanguinius::VoxState::reconnecting)
      ++session_->connection_generation;
    if (request.target == sanguinius::VoxState::reconnecting)
      ++session_->reconnect_count;
    if (request.target == sanguinius::VoxState::inactive ||
        request.target == sanguinius::VoxState::failed) {
      session_->ended_at_ms = request.now_ms;
      session_->end_reason = request.reason;
    }
    session_->last_failure_category = request.failure_category;
    if (request.target == sanguinius::VoxState::ready)
      operation_order_.push_back("ready");
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_, {},
                       request.timeout_job_id.has_value(), request.public_card);
  }

  sanguinius::VoxCommandResult
  fixture(const sanguinius::VoxFixtureRequest &request) override {
    const std::scoped_lock lock{mutex_};
    ++fixture_calls_;
    changed_.notify_all();
    const bool throw_targeted_fixture =
        throw_fixture_target_ == request.target &&
        throw_fixture_failures_remaining_ > 0;
    if (throw_targeted_fixture)
      --throw_fixture_failures_remaining_;
    if (throw_fixture_ || throw_targeted_fixture)
      throw std::runtime_error{"synthetic fixture failure"};
    if (!session_ || session_->revision != request.expected_revision)
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    session_->fixture_state = request.target;
    session_->fixture_marker = request.marker;
    session_->last_failure_category = request.failure_category;
    ++session_->revision;
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_);
  }

  sanguinius::VoxCommandResult
  occupancy(const sanguinius::VoxOccupancyRequest &request) override {
    const std::scoped_lock lock{mutex_};
    ++occupancy_calls_;
    changed_.notify_all();
    if (throw_occupancy_)
      throw std::runtime_error{"synthetic occupancy failure"};
    if (!session_ || session_->revision != request.expected_revision)
      return fake_result(sanguinius::VoxResultCode::invalid_state, session_);
    session_->empty_since_ms =
        request.human_count == 0 ? std::optional{request.now_ms} : std::nullopt;
    session_->timeout_job_id = request.empty_job_id;
    ++session_->revision;
    changed_.notify_all();
    return fake_result(sanguinius::VoxResultCode::accepted, session_, {},
                       request.human_count == 0);
  }

  sanguinius::VoxCommandResult
  handle_timeout(const sanguinius::ClaimedScheduledJob &, std::int64_t,
                 std::string, std::string, std::string,
                 std::optional<std::size_t>) override {
    const std::scoped_lock lock{mutex_};
    ++timeout_calls_;
    operation_order_.push_back("timeout");
    changed_.notify_all();
    if (throw_timeout_)
      throw std::runtime_error{"synthetic timeout failure"};
    if (close_on_timeout_ && session_) {
      auto closed = *session_;
      if (closed.fixture_state == sanguinius::VoxFixtureState::queued) {
        closed.fixture_state = sanguinius::VoxFixtureState::failed;
        closed.last_failure_category = "playback_interrupted";
        ++closed.revision;
      }
      closed.state = sanguinius::VoxState::inactive;
      ++closed.revision;
      closed.timeout_job_id = std::nullopt;
      closed.ended_at_ms = closed.last_active_at_ms;
      closed.end_reason = "connect_timeout";
      session_.reset();
      return fake_result(sanguinius::VoxResultCode::accepted,
                         std::move(closed));
    }
    return fake_result(sanguinius::VoxResultCode::inactive);
  }

  sanguinius::WorkMutationStatus
  fail_timeout_job(const sanguinius::ClaimedScheduledJob &,
                   const std::int64_t now_ms, const std::int64_t retry_at_ms,
                   std::string error_code, const bool retryable) override {
    const std::scoped_lock lock{mutex_};
    ++timeout_settlements_;
    timeout_settlement_now_ms_ = now_ms;
    timeout_retry_at_ms_ = retry_at_ms;
    timeout_error_code_ = std::move(error_code);
    timeout_retryable_ = retryable;
    changed_.notify_all();
    return sanguinius::WorkMutationStatus::applied;
  }

  std::optional<sanguinius::VoxSession> active() override {
    const std::scoped_lock lock{mutex_};
    if (session_ && session_->state != sanguinius::VoxState::inactive &&
        session_->state != sanguinius::VoxState::failed)
      return session_;
    return std::nullopt;
  }

  std::size_t recover(std::string_view, std::int64_t, std::string,
                      std::string) override {
    return 0;
  }

  sanguinius::VoxCommandResult shutdown(
      std::int64_t now_ms, std::string, std::string,
      std::optional<std::string> queued_fixture_failure_category) override {
    const std::scoped_lock lock{mutex_};
    if (!session_)
      return fake_result(sanguinius::VoxResultCode::inactive);
    if (session_->fixture_state == sanguinius::VoxFixtureState::queued) {
      session_->fixture_state = sanguinius::VoxFixtureState::failed;
      session_->last_failure_category =
          queued_fixture_failure_category.value_or("playback_interrupted");
      ++session_->revision;
    }
    session_->state = sanguinius::VoxState::inactive;
    session_->ended_at_ms = now_ms;
    session_->end_reason = "shutdown";
    ++session_->revision;
    return fake_result(sanguinius::VoxResultCode::accepted, session_);
  }

  [[nodiscard]] bool
  wait_for_fixture(const sanguinius::VoxFixtureState state,
                   const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, state] {
      return session_ && session_->fixture_state == state;
    });
  }

  [[nodiscard]] bool
  wait_for_state(const sanguinius::VoxState state,
                 const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, state] {
      return session_ && session_->state == state;
    });
  }

  void block_status() {
    const std::scoped_lock lock{mutex_};
    block_status_ = true;
  }

  [[nodiscard]] bool
  wait_for_status_entry(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return status_entered_; });
  }

  [[nodiscard]] bool
  wait_for_status_calls(const std::size_t count,
                        const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return status_calls_ >= count; });
  }

  void release_status() {
    const std::scoped_lock lock{mutex_};
    status_released_ = true;
    changed_.notify_all();
  }

  void set_start_override(sanguinius::VoxCommandResult result) {
    const std::scoped_lock lock{mutex_};
    start_override_ = std::move(result);
  }

  void set_throw_status(const bool value) {
    const std::scoped_lock lock{mutex_};
    throw_status_ = value;
  }

  void set_throw_timeout(const bool value) {
    const std::scoped_lock lock{mutex_};
    throw_timeout_ = value;
  }

  void set_throw_transition(const bool value) {
    const std::scoped_lock lock{mutex_};
    throw_transition_ = value;
  }

  void set_throw_fixture(const bool value) {
    const std::scoped_lock lock{mutex_};
    throw_fixture_ = value;
  }

  void set_fixture_failures(const sanguinius::VoxFixtureState target,
                            const std::size_t count) {
    const std::scoped_lock lock{mutex_};
    throw_fixture_target_ = target;
    throw_fixture_failures_remaining_ = count;
  }

  [[nodiscard]] bool
  wait_for_fixture_calls(const std::size_t count,
                         const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return fixture_calls_ >= count; });
  }

  void set_throw_occupancy(const bool value) {
    const std::scoped_lock lock{mutex_};
    throw_occupancy_ = value;
  }

  [[nodiscard]] bool
  wait_for_occupancy_calls(const std::size_t count,
                           const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return occupancy_calls_ >= count; });
  }

  void set_close_on_timeout(const bool value) {
    const std::scoped_lock lock{mutex_};
    close_on_timeout_ = value;
  }

  [[nodiscard]] std::optional<std::int64_t> last_start_now_ms() const {
    const std::scoped_lock lock{mutex_};
    return last_start_now_ms_;
  }

  [[nodiscard]] bool
  wait_for_timeout_calls(const std::size_t count,
                         const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return timeout_calls_ >= count; });
  }

  [[nodiscard]] std::size_t timeout_calls() const {
    const std::scoped_lock lock{mutex_};
    return timeout_calls_;
  }

  [[nodiscard]] bool
  wait_for_timeout_settlements(const std::size_t count,
                               const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return timeout_settlements_ >= count; });
  }

  [[nodiscard]] std::int64_t timeout_retry_at_ms() const {
    const std::scoped_lock lock{mutex_};
    return timeout_retry_at_ms_;
  }

  [[nodiscard]] std::string timeout_error_code() const {
    const std::scoped_lock lock{mutex_};
    return timeout_error_code_;
  }

  [[nodiscard]] bool timeout_retryable() const {
    const std::scoped_lock lock{mutex_};
    return timeout_retryable_;
  }

  [[nodiscard]] std::vector<std::string> operation_order() const {
    const std::scoped_lock lock{mutex_};
    return operation_order_;
  }

  [[nodiscard]] std::optional<sanguinius::VoxTransitionRequest>
  last_transition() const {
    const std::scoped_lock lock{mutex_};
    return last_transition_;
  }

  [[nodiscard]] bool
  wait_for_empty(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] {
      return session_ && session_->empty_since_ms.has_value();
    });
  }

private:
  struct CommandReceipt {
    std::string operation;
    std::string request_fingerprint;
    sanguinius::VoxCommandResult result;
  };

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::optional<sanguinius::VoxSession> session_;
  std::map<std::string, CommandReceipt> command_receipts_;
  bool block_status_{};
  bool status_entered_{};
  bool status_released_{};
  std::size_t status_calls_{};
  bool throw_status_{};
  bool throw_timeout_{};
  bool throw_transition_{};
  bool throw_fixture_{};
  std::optional<sanguinius::VoxFixtureState> throw_fixture_target_;
  std::size_t throw_fixture_failures_remaining_{};
  std::size_t fixture_calls_{};
  bool throw_occupancy_{};
  std::size_t occupancy_calls_{};
  bool close_on_timeout_{};
  std::optional<std::int64_t> last_start_now_ms_;
  std::size_t timeout_calls_{};
  std::size_t timeout_settlements_{};
  std::int64_t timeout_settlement_now_ms_{};
  std::int64_t timeout_retry_at_ms_{};
  std::string timeout_error_code_;
  bool timeout_retryable_{};
  std::vector<std::string> operation_order_;
  std::optional<sanguinius::VoxTransitionRequest> last_transition_;
  std::optional<sanguinius::VoxCommandResult> start_override_;
};

class FakeVoiceGateway final : public sanguinius::VoiceGateway {
public:
  void start(EventCallback callback) override {
    const std::scoped_lock lock{mutex_};
    callback_ = std::move(callback);
  }

  void resolve_member_channel(sanguinius::DiscordSnowflake,
                              sanguinius::DiscordSnowflake,
                              ResolveCallback callback) override {
    sanguinius::VoiceResolvedChannel resolution;
    {
      const std::scoped_lock lock{mutex_};
      resolution = {.status = resolution_status_,
                    .channel_id = 40,
                    .human_count = resolution_human_count_,
                    .failure_category = {}};
      if (delay_resolution_) {
        pending_resolution_ = std::move(callback);
        pending_resolution_value_ = resolution;
        changed_.notify_all();
        return;
      }
    }
    callback(std::move(resolution));
  }

  sanguinius::VoiceGatewaySubmit
  connect(const sanguinius::VoiceConnectRequest &request) override {
    const std::scoped_lock lock{mutex_};
    request_ = request;
    requests_.push_back(request);
    ++connect_count_;
    if (snapshot_.bound && snapshot_.session_id != request.session_id) {
      changed_.notify_all();
      return sanguinius::VoiceGatewaySubmit::unavailable;
    }
    if (connect_result_ == sanguinius::VoiceGatewaySubmit::accepted) {
      snapshot_ = {.bound = true,
                   .connected = true,
                   .ready = false,
                   .dave_active = false,
                   .marker_completed = false,
                   .completed_marker = {},
                   .bot_moved = false,
                   .session_id = request.session_id,
                   .guild_id = request.guild_id,
                   .channel_id = request.channel_id,
                   .observed_channel_id = request.channel_id,
                   .generation = request.generation,
                   .human_count = resolution_human_count_};
    }
    changed_.notify_all();
    return connect_result_;
  }

  sanguinius::VoiceGatewaySubmit disconnect(std::string_view) override {
    const std::scoped_lock lock{mutex_};
    ++disconnect_count_;
    changed_.notify_all();
    return disconnect_result_;
  }

  void release_binding(std::string_view session_id) noexcept override {
    const std::scoped_lock lock{mutex_};
    if (snapshot_.session_id != session_id)
      return;
    ++release_count_;
    snapshot_ = {};
    changed_.notify_all();
  }

  sanguinius::VoiceGatewaySubmit stop_audio(std::string_view) override {
    const std::scoped_lock lock{mutex_};
    ++stop_count_;
    changed_.notify_all();
    return sanguinius::VoiceGatewaySubmit::accepted;
  }

  sanguinius::VoiceGatewaySubmit send_pcm(std::string_view,
                                          const sanguinius::PcmAudio &audio,
                                          std::string_view marker) override {
    const std::scoped_lock lock{mutex_};
    ++send_count_;
    audio_ = audio;
    marker_ = marker;
    changed_.notify_all();
    return send_result_;
  }

  sanguinius::VoiceGatewaySnapshot
  snapshot(std::string_view session_id) const noexcept override {
    const std::scoped_lock lock{mutex_};
    return snapshot_.session_id == session_id
               ? snapshot_
               : sanguinius::VoiceGatewaySnapshot{};
  }

  void shutdown() noexcept override {
    const std::scoped_lock lock{mutex_};
    callback_ = {};
  }

  void emit(const sanguinius::VoiceEventKind kind,
            const bool dave_active = true, const std::size_t human_count = 1,
            const sanguinius::DiscordSnowflake observed_channel_id = {}) {
    EventCallback callback;
    sanguinius::VoiceEvent event;
    {
      const std::scoped_lock lock{mutex_};
      callback = callback_;
      const auto channel_id = observed_channel_id.is_set()
                                  ? observed_channel_id
                                  : request_.channel_id;
      event = {.kind = kind,
               .session_id = request_.session_id,
               .guild_id = request_.guild_id,
               .channel_id = channel_id,
               .generation = request_.generation,
               .human_count = human_count,
               .dave_active = dave_active,
               .marker = marker_,
               .failure_category = {}};
      if (kind == sanguinius::VoiceEventKind::ready) {
        snapshot_.observed_channel_id = channel_id;
        snapshot_.connected = true;
        snapshot_.ready = true;
        snapshot_.dave_active = dave_active;
      }
      if (kind == sanguinius::VoiceEventKind::disconnected) {
        if (!snapshot_.bot_moved)
          snapshot_.observed_channel_id = {};
        snapshot_.connected = false;
        snapshot_.ready = false;
        snapshot_.dave_active = false;
      }
      if (kind == sanguinius::VoiceEventKind::bot_moved) {
        snapshot_.observed_channel_id = channel_id;
        snapshot_.connected = true;
        snapshot_.ready = false;
        snapshot_.dave_active = false;
        snapshot_.bot_moved = true;
      }
      if (kind == sanguinius::VoiceEventKind::track_marker) {
        snapshot_.marker_completed = true;
        snapshot_.completed_marker = marker_;
      }
      snapshot_.human_count = human_count;
    }
    callback(std::move(event));
  }

  [[nodiscard]] bool
  wait_for_connects(const std::size_t count,
                    const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return connect_count_ >= count; });
  }

  [[nodiscard]] bool
  wait_for_sends(const std::size_t count,
                 const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return send_count_ >= count; });
  }

  [[nodiscard]] bool
  wait_for_disconnects(const std::size_t count,
                       const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return disconnect_count_ >= count; });
  }

  [[nodiscard]] bool
  wait_for_stops(const std::size_t count,
                 const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return stop_count_ >= count; });
  }

  [[nodiscard]] std::size_t send_count() const {
    const std::scoped_lock lock{mutex_};
    return send_count_;
  }

  [[nodiscard]] std::size_t disconnect_count() const {
    const std::scoped_lock lock{mutex_};
    return disconnect_count_;
  }

  [[nodiscard]] std::size_t release_count() const {
    const std::scoped_lock lock{mutex_};
    return release_count_;
  }

  [[nodiscard]] bool
  wait_for_releases(const std::size_t count,
                    const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return release_count_ >= count; });
  }

  void set_resolution_status(const sanguinius::VoiceResolveStatus status) {
    const std::scoped_lock lock{mutex_};
    resolution_status_ = status;
  }

  void set_resolution_human_count(const std::size_t human_count) {
    const std::scoped_lock lock{mutex_};
    resolution_human_count_ = human_count;
  }

  void delay_resolution() {
    const std::scoped_lock lock{mutex_};
    delay_resolution_ = true;
  }

  [[nodiscard]] bool
  wait_for_pending_resolution(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] {
      return static_cast<bool>(pending_resolution_);
    });
  }

  void release_resolution() {
    ResolveCallback callback;
    sanguinius::VoiceResolvedChannel resolution;
    {
      const std::scoped_lock lock{mutex_};
      callback = std::move(pending_resolution_);
      resolution = pending_resolution_value_;
      delay_resolution_ = false;
    }
    if (callback)
      callback(std::move(resolution));
  }

  void discard_resolution() {
    const std::scoped_lock lock{mutex_};
    pending_resolution_ = {};
    delay_resolution_ = false;
  }

  void set_connect_result(const sanguinius::VoiceGatewaySubmit result) {
    const std::scoped_lock lock{mutex_};
    connect_result_ = result;
  }

  void set_disconnect_result(const sanguinius::VoiceGatewaySubmit result) {
    const std::scoped_lock lock{mutex_};
    disconnect_result_ = result;
  }

  void set_send_result(const sanguinius::VoiceGatewaySubmit result) {
    const std::scoped_lock lock{mutex_};
    send_result_ = result;
  }

  [[nodiscard]] std::vector<sanguinius::VoiceConnectRequest>
  connect_requests() const {
    const std::scoped_lock lock{mutex_};
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  EventCallback callback_;
  sanguinius::VoiceConnectRequest request_;
  std::vector<sanguinius::VoiceConnectRequest> requests_;
  sanguinius::VoiceGatewaySnapshot snapshot_;
  sanguinius::PcmAudio audio_;
  std::string marker_;
  std::size_t connect_count_{};
  std::size_t disconnect_count_{};
  std::size_t stop_count_{};
  std::size_t release_count_{};
  std::size_t send_count_{};
  sanguinius::VoiceResolveStatus resolution_status_{
      sanguinius::VoiceResolveStatus::ready};
  std::size_t resolution_human_count_{1};
  bool delay_resolution_{};
  ResolveCallback pending_resolution_;
  sanguinius::VoiceResolvedChannel pending_resolution_value_;
  sanguinius::VoiceGatewaySubmit connect_result_{
      sanguinius::VoiceGatewaySubmit::accepted};
  sanguinius::VoiceGatewaySubmit disconnect_result_{
      sanguinius::VoiceGatewaySubmit::accepted};
  sanguinius::VoiceGatewaySubmit send_result_{
      sanguinius::VoiceGatewaySubmit::accepted};
};

} // namespace

TEST_CASE("Vox worker failures complete commands with a safe diagnostic",
          "[vox][worker][failure]") {
  FakeVoxRepository repository;
  repository.set_throw_status(true);
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000920",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.status({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "status-throws",
                          .correlation_id = "status-throws",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(future.get().code == sanguinius::VoxResultCode::unavailable);
  REQUIRE(diagnostics.contains_category("vox.command"));
  REQUIRE(service.health().last_failure_category == "internal_error");
  service.stop();
}

TEST_CASE("Vox gateway failures retry autonomously until persistence recovers",
          "[vox][gateway][failure][persistence][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000927",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key =
                              "summon-before-transition-failure",
                          .correlation_id = "transition-failure",
                          .now_ms = 0},
                         [&summoned](sanguinius::VoxCommandResult result) {
                           summoned.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  repository.set_throw_transition(true);
  gateway.emit(sanguinius::VoiceEventKind::bot_moved, false, 1, 41);
  REQUIRE(gateway.wait_for_stops(1, 1s));
  REQUIRE(gateway.wait_for_disconnects(1, 1s));
  REQUIRE_FALSE(gateway.wait_for_releases(1, 100ms));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));

  repository.set_throw_transition(false);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  REQUIRE(gateway.wait_for_releases(1, 1s));
  REQUIRE(gateway.connect_requests().size() == 1);
  service.stop();
}

TEST_CASE("Vox occupancy persistence retries without another interaction",
          "[vox][gateway][occupancy][persistence][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  WaitableCounter scheduler_wakes;
  sanguinius::VoxService service{
      repository,
      gateway,
      clock,
      ids,
      diagnostics,
      {10, 20, 30},
      {},
      "00000000-0000-4000-8000-000000000929",
      [&scheduler_wakes] { scheduler_wakes.increment(); },
      [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  const sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-before-occupancy-failure",
      .correlation_id = "occupancy-failure",
      .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));

  repository.set_throw_occupancy(true);
  gateway.emit(sanguinius::VoiceEventKind::occupancy_changed, true, 0);
  REQUIRE(repository.wait_for_occupancy_calls(2, 1s));
  REQUIRE(diagnostics.contains_category("vox.gateway_event"));

  repository.set_throw_occupancy(false);
  REQUIRE(repository.wait_for_empty(1s));
  REQUIRE(scheduler_wakes.wait_for_at_least(2, 1s));
  service.stop();
}

TEST_CASE("Vox pending proof claims retry without another interaction",
          "[vox][gateway][fixture][persistence][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000930",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  const sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-before-fixture-claim-failure",
      .correlation_id = "fixture-claim-failure",
      .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);

  repository.set_throw_fixture(true);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(repository.wait_for_fixture_calls(2, 1s));
  REQUIRE(gateway.send_count() == 0);

  repository.set_throw_fixture(false);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));
  REQUIRE(gateway.send_count() == 1);
  service.stop();
}

TEST_CASE("Vox retains a rejected PCM failure until persistence recovers",
          "[vox][gateway][audio][persistence][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000931",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(service.summon(
              {.guild_id = 10,
               .text_channel_id = 20,
               .actor_user_id = 31,
               .owner_user_id = 30,
               .interaction_idempotency_key = "summon-before-audio-rejection",
               .correlation_id = "audio-rejection-recovery",
               .now_ms = 0},
              [&summoned](sanguinius::VoxCommandResult result) {
                summoned.set_value(std::move(result));
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);

  gateway.set_send_result(sanguinius::VoiceGatewaySubmit::unavailable);
  repository.set_fixture_failures(sanguinius::VoxFixtureState::failed, 2);
  gateway.emit(sanguinius::VoiceEventKind::ready);

  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  REQUIRE(gateway.send_count() == 1);
  const auto current = repository.active();
  REQUIRE(current);
  REQUIRE(current->last_failure_category == "audio_rejected");
  REQUIRE(service.health().reconciliations >= 2);
  service.stop();
}

TEST_CASE("Vox leave preserves a rejected PCM cause ahead of retry",
          "[vox][leave][gateway][audio][persistence][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000932",
      [] {},       [] {}};
  service.start();

  const sanguinius::VoxCommandContext summon_context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-before-rejected-audio-leave",
      .correlation_id = "rejected-audio-leave",
      .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(service.summon(summon_context,
                         [&summoned](sanguinius::VoxCommandResult result) {
                           summoned.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);

  gateway.set_send_result(sanguinius::VoiceGatewaySubmit::unavailable);
  repository.set_fixture_failures(sanguinius::VoxFixtureState::failed, 2);
  gateway.emit(sanguinius::VoiceEventKind::ready);

  auto leave_context = summon_context;
  leave_context.interaction_idempotency_key = "leave-after-rejected-audio";
  std::promise<sanguinius::VoxCommandResult> left;
  auto leave_future = left.get_future();
  REQUIRE(service.leave(leave_context,
                        [&left](sanguinius::VoxCommandResult result) {
                          left.set_value(std::move(result));
                        }) == sanguinius::SubmitResult::accepted);
  REQUIRE(leave_future.wait_for(1s) == std::future_status::ready);
  const auto leave_result = leave_future.get();
  REQUIRE(leave_result.code == sanguinius::VoxResultCode::accepted);
  REQUIRE(leave_result.session);
  REQUIRE(leave_result.session->state == sanguinius::VoxState::leaving);
  REQUIRE(leave_result.session->fixture_state ==
          sanguinius::VoxFixtureState::failed);
  REQUIRE(leave_result.session->last_failure_category == "audio_rejected");
  REQUIRE(gateway.send_count() == 1);
  service.stop();
}

TEST_CASE("Vox leave disconnects when proof persistence fails",
          "[vox][leave][gateway][persistence]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  WaitableCounter scheduler_wakes;
  sanguinius::VoxService service{
      repository,
      gateway,
      clock,
      ids,
      diagnostics,
      {10, 20, 30},
      {},
      "00000000-0000-4000-8000-000000000928",
      [&scheduler_wakes] { scheduler_wakes.increment(); },
      [] {}};
  service.start();

  const sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-before-fixture-failure",
      .correlation_id = "fixture-failure",
      .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  repository.set_throw_fixture(true);
  auto leave_context = context;
  leave_context.interaction_idempotency_key = "leave-fixture-failure";
  std::promise<sanguinius::VoxCommandResult> left;
  auto leave_future = left.get_future();
  REQUIRE(service.leave(leave_context,
                        [&left](sanguinius::VoxCommandResult result) {
                          left.set_value(std::move(result));
                        }) == sanguinius::SubmitResult::accepted);
  REQUIRE(leave_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(leave_future.get().code == sanguinius::VoxResultCode::unavailable);
  REQUIRE(gateway.wait_for_stops(1, 1s));
  REQUIRE(gateway.wait_for_disconnects(1, 1s));
  REQUIRE(scheduler_wakes.wait_for_at_least(2, 1s));
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::leaving, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  repository.set_throw_fixture(false);
  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::inactive, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  service.stop();
}

TEST_CASE("Vox readiness timeouts begin after asynchronous channel resolution",
          "[vox][clock][summon]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  gateway.delay_resolution();
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000921",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "summon-delayed",
                          .correlation_id = "summon-delayed",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(gateway.wait_for_pending_resolution(1s));
  clock.set(std::chrono::sys_seconds{10s});
  gateway.release_resolution();
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(future.get().code == sanguinius::VoxResultCode::accepted);
  REQUIRE(repository.last_start_now_ms() == 10'000);
  service.stop();
}

TEST_CASE("Vox pending resolution leases may outlive service shutdown",
          "[vox][shutdown][resolution][lifetime]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  gateway.delay_resolution();
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto service = std::make_unique<sanguinius::VoxService>(
      repository, gateway, clock, ids, diagnostics,
      sanguinius::ServerScopeConfiguration{10, 20, 30},
      sanguinius::ControlConfiguration{},
      "00000000-0000-4000-8000-000000000924", [] {}, [] {});
  service->start();

  REQUIRE(service->summon(
              {.guild_id = 10,
               .text_channel_id = 20,
               .actor_user_id = 31,
               .owner_user_id = 30,
               .interaction_idempotency_key = "summon-shutdown-resolution",
               .correlation_id = "shutdown-resolution",
               .now_ms = 0},
              [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(gateway.wait_for_pending_resolution(1s));

  service->stop();
  service.reset();
  gateway.discard_resolution();
}

TEST_CASE("Vox timeout work is serialized behind priority gateway events",
          "[vox][timeout][queue][ordering]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000922",
      [] {},       [] {}};
  service.start();

  const sanguinius::VoxCommandContext context{.guild_id = 10,
                                              .text_channel_id = 20,
                                              .actor_user_id = 31,
                                              .owner_user_id = 30,
                                              .interaction_idempotency_key =
                                                  "summon-timeout-order",
                                              .correlation_id = "timeout-order",
                                              .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);

  repository.block_status();
  auto status_context = context;
  status_context.interaction_idempotency_key = "status-timeout-order";
  REQUIRE(service.status(status_context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(service.handle_timeout(connect_timeout_job()) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.timeout_calls() == 0);
  repository.release_status();

  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(repository.wait_for_timeout_calls(1, 1s));
  const auto order = repository.operation_order();
  REQUIRE(order.size() >= 2);
  REQUIRE(order[0] == "ready");
  REQUIRE(order[1] == "timeout");
  service.stop();
}

TEST_CASE("Vox worker failures promptly settle claimed timeout jobs",
          "[vox][timeout][failure]") {
  FakeVoxRepository repository;
  repository.set_throw_timeout(true);
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock{std::chrono::sys_seconds{7s}};
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  WaitableCounter scheduler_wakes;
  sanguinius::VoxService service{
      repository,
      gateway,
      clock,
      ids,
      diagnostics,
      {10, 20, 30},
      {},
      "00000000-0000-4000-8000-000000000925",
      [&scheduler_wakes] { scheduler_wakes.increment(); },
      [] {}};
  service.start();

  REQUIRE(service.handle_timeout(connect_timeout_job()) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_timeout_settlements(1, 1s));
  REQUIRE(repository.timeout_error_code() == "handler_exception");
  REQUIRE(repository.timeout_retryable());
  REQUIRE(repository.timeout_retry_at_ms() == 12'000);
  REQUIRE(scheduler_wakes.wait_for_at_least(1, 1s));
  REQUIRE(scheduler_wakes.load() == 1);
  REQUIRE(diagnostics.contains_category("vox.timeout"));
  service.stop();
}

TEST_CASE("Vox terminal timeout releases a rejected disconnect binding",
          "[vox][timeout][gateway][binding]") {
  FakeVoxRepository repository;
  repository.set_close_on_timeout(true);
  FakeVoiceGateway gateway;
  gateway.set_disconnect_result(sanguinius::VoiceGatewaySubmit::unavailable);
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000926",
      [] {},       [] {}};
  service.start();

  const sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-before-terminal-timeout",
      .correlation_id = "terminal-timeout-binding",
      .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> first;
  auto first_future = first.get_future();
  REQUIRE(
      service.summon(context, [&first](sanguinius::VoxCommandResult result) {
        first.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(first_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(first_future.get().code == sanguinius::VoxResultCode::accepted);
  REQUIRE(gateway.wait_for_connects(1, 1s));

  REQUIRE(service.handle_timeout(connect_timeout_job()) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(gateway.wait_for_releases(1, 1s));
  REQUIRE(gateway.disconnect_count() == 1);

  auto second_context = context;
  second_context.interaction_idempotency_key = "summon-after-terminal-timeout";
  std::promise<sanguinius::VoxCommandResult> second;
  auto second_future = second.get_future();
  REQUIRE(service.summon(second_context,
                         [&second](sanguinius::VoxCommandResult result) {
                           second.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(second_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(second_future.get().code == sanguinius::VoxResultCode::accepted);
  REQUIRE(gateway.wait_for_connects(2, 1s));
  service.stop();
}

TEST_CASE("Vox saturated resolution callback uses its bounded responder",
          "[vox][queue][resolution][callback]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  gateway.delay_resolution();
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000923",
      [] {},       [] {},        1};
  service.start();

  const sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-resolution-overflow",
      .correlation_id = "resolution-overflow",
      .now_ms = 0};
  std::promise<void> completion_entered;
  auto entered = completion_entered.get_future();
  std::promise<void> release_completion;
  const auto released = release_completion.get_future().share();
  std::atomic<bool> unavailable{};
  REQUIRE(
      service.summon(context, [&completion_entered, released, &unavailable](
                                  sanguinius::VoxCommandResult result) mutable {
        unavailable.store(result.code ==
                          sanguinius::VoxResultCode::unavailable);
        completion_entered.set_value();
        released.wait();
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(gateway.wait_for_pending_resolution(1s));

  repository.block_status();
  auto status_context = context;
  status_context.interaction_idempotency_key = "status-resolution-blocked";
  REQUIRE(service.status(status_context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  status_context.interaction_idempotency_key = "status-resolution-queued";
  REQUIRE(service.status(status_context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);

  auto callback = std::async(std::launch::async,
                             [&gateway] { gateway.release_resolution(); });
  REQUIRE(entered.wait_for(1s) == std::future_status::ready);
  REQUIRE(unavailable.load());
  REQUIRE(callback.wait_for(100ms) == std::future_status::ready);
  callback.get();
  release_completion.set_value();
  repository.release_status();
  service.stop();
}

TEST_CASE("Vox state machine accepts only the governed edges",
          "[vox][state-machine]") {
  using sanguinius::VoxState;
  constexpr std::array states{VoxState::connecting, VoxState::ready,
                              VoxState::muted,      VoxState::reconnecting,
                              VoxState::leaving,    VoxState::inactive,
                              VoxState::failed};
  const auto expected_normal = [](const VoxState from, const VoxState to) {
    switch (from) {
    case VoxState::connecting:
      return to == VoxState::ready || to == VoxState::leaving ||
             to == VoxState::failed;
    case VoxState::ready:
      return to == VoxState::muted || to == VoxState::reconnecting ||
             to == VoxState::leaving || to == VoxState::failed;
    case VoxState::muted:
      return to == VoxState::ready || to == VoxState::reconnecting ||
             to == VoxState::leaving || to == VoxState::failed;
    case VoxState::reconnecting:
      return to == VoxState::ready || to == VoxState::muted ||
             to == VoxState::leaving || to == VoxState::failed;
    case VoxState::leaving:
      return to == VoxState::inactive;
    case VoxState::inactive:
    case VoxState::failed:
      return false;
    }
    return false;
  };

  for (const auto from : states) {
    for (const auto to : states) {
      REQUIRE(sanguinius::vox_transition_allowed(from, to, "voice_event", 0) ==
              expected_normal(from, to));
    }
  }
  REQUIRE(sanguinius::vox_transition_allowed(
      VoxState::ready, VoxState::inactive, "empty_timeout", 0));
  REQUIRE(sanguinius::vox_transition_allowed(
      VoxState::muted, VoxState::inactive, "empty_timeout", 0));
  REQUIRE_FALSE(sanguinius::vox_transition_allowed(
      VoxState::connecting, VoxState::inactive, "empty_timeout", 0));
  REQUIRE_FALSE(sanguinius::vox_transition_allowed(
      VoxState::ready, VoxState::reconnecting, "voice_event", 1));
  REQUIRE_FALSE(sanguinius::vox_transition_allowed(
      VoxState::muted, VoxState::reconnecting, "voice_event", 1));

  for (const auto from : states) {
    const auto active = from != VoxState::inactive && from != VoxState::failed;
    REQUIRE(sanguinius::vox_transition_allowed(
                from, VoxState::inactive, "restart_abandoned", 0) == active);
    REQUIRE(sanguinius::vox_transition_allowed(from, VoxState::inactive,
                                               "restart_cleanup", 0) == active);
    REQUIRE(sanguinius::vox_transition_allowed(from, VoxState::inactive,
                                               "shutdown", 0) == active);
  }
}

TEST_CASE("Vox summon validation and gateway rejection fail without audio",
          "[vox][summon][validation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000905",
      [] {},       [] {}};
  service.start();

  const std::array rejected{sanguinius::VoiceResolveStatus::no_voice,
                            sanguinius::VoiceResolveStatus::unsupported_channel,
                            sanguinius::VoiceResolveStatus::permission_denied,
                            sanguinius::VoiceResolveStatus::channel_full,
                            sanguinius::VoiceResolveStatus::unavailable};
  std::size_t key = 0;
  for (const auto status : rejected) {
    gateway.set_resolution_status(status);
    std::promise<sanguinius::VoxCommandResult> completed;
    auto future = completed.get_future();
    REQUIRE(service.summon({.guild_id = 10,
                            .text_channel_id = 20,
                            .actor_user_id = 31,
                            .owner_user_id = 30,
                            .interaction_idempotency_key =
                                "summon-rejected-" + std::to_string(++key),
                            .correlation_id = "test-rejected",
                            .now_ms = 0},
                           [&completed](sanguinius::VoxCommandResult result) {
                             completed.set_value(std::move(result));
                           }) == sanguinius::SubmitResult::accepted);
    REQUIRE(future.wait_for(1s) == std::future_status::ready);
    REQUIRE(future.get().code != sanguinius::VoxResultCode::accepted);
    REQUIRE_FALSE(repository.active());
  }
  REQUIRE(gateway.send_count() == 0);

  gateway.set_resolution_status(sanguinius::VoiceResolveStatus::ready);
  gateway.set_connect_result(sanguinius::VoiceGatewaySubmit::unavailable);
  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(
      service.summon({.guild_id = 10,
                      .text_channel_id = 20,
                      .actor_user_id = 31,
                      .owner_user_id = 30,
                      .interaction_idempotency_key = "summon-gateway-rejected",
                      .correlation_id = "test-gateway-rejected",
                      .now_ms = 0},
                     [&completed](sanguinius::VoxCommandResult result) {
                       completed.set_value(std::move(result));
                     }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  const auto rejected_result = future.get();
  REQUIRE(rejected_result.code == sanguinius::VoxResultCode::unavailable);
  REQUIRE(rejected_result.message == "Vox could not begin a new session.");
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  REQUIRE(gateway.send_count() == 0);
  REQUIRE(service.health().last_failure_category == "gateway_unavailable");
  service.stop();
}

TEST_CASE("Vox proof chime is exact bounded stereo PCM", "[vox][audio]") {
  const auto audio = sanguinius::make_vox_proof_chime();
  REQUIRE(audio.sample_rate == 48'000);
  REQUIRE(audio.channels == 2);
  REQUIRE(audio.bits_per_sample == 16);
  REQUIRE(audio.samples.size() == 57'600);
  REQUIRE(audio.samples.size() * sizeof(std::int16_t) == 115'200);
  REQUIRE((audio.samples.size() * sizeof(std::int16_t)) / 11'520 == 10);
  std::int32_t peak = 0;
  for (std::size_t frame = 0; frame < 28'800; ++frame) {
    REQUIRE(audio.samples[frame * 2] == audio.samples[frame * 2 + 1]);
    peak = std::max(
        peak, std::abs(static_cast<std::int32_t>(audio.samples[frame * 2])));
  }
  REQUIRE(peak <= 4'900);
  REQUIRE(audio.samples.front() == 0);
  REQUIRE(audio.samples[23'038] == 0);
  for (std::size_t frame = 11'520; frame < 17'280; ++frame)
    REQUIRE(audio.samples[frame * 2] == 0);
}

TEST_CASE("Vox fake gateway plays once and reconnects without replay",
          "[vox][gateway][reconnect]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000900",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "summon",
                          .correlation_id = "test",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(future.get().code == sanguinius::VoxResultCode::accepted);
  REQUIRE(gateway.wait_for_connects(1, 1s));

  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.send_count() == 1);
  gateway.emit(sanguinius::VoiceEventKind::track_marker);
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::played, 1s));

  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::reconnecting, 1s));
  REQUIRE(gateway.wait_for_connects(2, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(gateway.send_count() == 1);
  const auto requests = gateway.connect_requests();
  REQUIRE(requests.size() == 2);
  REQUIRE(requests[0].validate_member_channel);
  REQUIRE_FALSE(requests[1].validate_member_channel);
  REQUIRE_FALSE(requests[1].member_user_id.is_set());
  REQUIRE(requests[1].channel_id == 40);
  service.stop();
}

TEST_CASE("Vox marks interrupted proof playback failed before reconnecting",
          "[vox][gateway][reconnect][audio]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000911",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "summon-interrupted",
                          .correlation_id = "test-interrupted",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::reconnecting, 1s));
  REQUIRE(gateway.wait_for_connects(2, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  gateway.emit(sanguinius::VoiceEventKind::track_marker);
  REQUIRE(gateway.send_count() == 1);
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  service.stop();
}

TEST_CASE("Vox ready starts empty grace from the ready snapshot",
          "[vox][gateway][occupancy]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  gateway.set_resolution_human_count(0);
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  WaitableCounter scheduler_wakes;
  sanguinius::VoxService service{
      repository,
      gateway,
      clock,
      ids,
      diagnostics,
      {10, 20, 30},
      {},
      "00000000-0000-4000-8000-000000000906",
      [&scheduler_wakes] { scheduler_wakes.increment(); },
      [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "summon-empty-ready",
                          .correlation_id = "test-empty-ready",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready, true, 0);
  REQUIRE(repository.wait_for_empty(1s));
  REQUIRE(scheduler_wakes.wait_for_at_least(2, 1s));
  service.stop();
}

TEST_CASE("Vox leave remains active until the gateway confirms departure",
          "[vox][gateway][leave]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  gateway.set_disconnect_result(sanguinius::VoiceGatewaySubmit::unavailable);
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  WaitableCounter scheduler_wakes;
  sanguinius::VoxService service{
      repository,
      gateway,
      clock,
      ids,
      diagnostics,
      {10, 20, 30},
      {},
      "00000000-0000-4000-8000-000000000907",
      [&scheduler_wakes] { scheduler_wakes.increment(); },
      [] {}};
  service.start();

  const sanguinius::VoxCommandContext context{.guild_id = 10,
                                              .text_channel_id = 20,
                                              .actor_user_id = 31,
                                              .owner_user_id = 30,
                                              .interaction_idempotency_key =
                                                  "summon-before-leave",
                                              .correlation_id = "test-leave",
                                              .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  auto leave_context = context;
  leave_context.interaction_idempotency_key = "leave-pending-departure";
  std::promise<sanguinius::VoxCommandResult> left;
  auto leave_future = left.get_future();
  REQUIRE(service.leave(leave_context,
                        [&left](sanguinius::VoxCommandResult result) {
                          left.set_value(std::move(result));
                        }) == sanguinius::SubmitResult::accepted);
  REQUIRE(leave_future.wait_for(1s) == std::future_status::ready);
  const auto leave_result = leave_future.get();
  REQUIRE(leave_result.session->state == sanguinius::VoxState::leaving);
  REQUIRE(leave_result.session->fixture_state ==
          sanguinius::VoxFixtureState::failed);
  REQUIRE(leave_result.session->timeout_job_id.has_value());
  REQUIRE(leave_result.wake_scheduler);
  REQUIRE(scheduler_wakes.wait_for_at_least(2, 1s));
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::leaving, 1s));
  REQUIRE(gateway.wait_for_disconnects(1, 1s));

  auto competing_context = context;
  competing_context.interaction_idempotency_key = "summon-while-leaving";
  std::promise<sanguinius::VoxCommandResult> competing;
  auto competing_future = competing.get_future();
  REQUIRE(service.summon(competing_context,
                         [&competing](sanguinius::VoxCommandResult result) {
                           competing.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(competing_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(competing_future.get().code ==
          sanguinius::VoxResultCode::active_session);

  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::inactive, 1s));
  service.stop();
}

TEST_CASE("Vox preserves a repository replay result during summon completion",
          "[vox][summon][replay]") {
  FakeVoxRepository repository;
  repository.set_start_override(
      fake_result(sanguinius::VoxResultCode::replay, std::nullopt,
                  "The original accepted summon result."));
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000908",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(
      service.summon({.guild_id = 10,
                      .text_channel_id = 20,
                      .actor_user_id = 31,
                      .owner_user_id = 30,
                      .interaction_idempotency_key = "summon-overlap-replay",
                      .correlation_id = "test-overlap-replay",
                      .now_ms = 0},
                     [&completed](sanguinius::VoxCommandResult result) {
                       completed.set_value(std::move(result));
                     }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  const auto result = future.get();
  REQUIRE(result.code == sanguinius::VoxResultCode::replay);
  REQUIRE(result.message == "The original accepted summon result.");
  REQUIRE(gateway.connect_requests().empty());
  service.stop();
}

TEST_CASE("Vox shutdown persists interrupted proof playback as failed",
          "[vox][shutdown][audio]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000924",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto future = summoned.get_future();
  REQUIRE(
      service.summon({.guild_id = 10,
                      .text_channel_id = 20,
                      .actor_user_id = 31,
                      .owner_user_id = 30,
                      .interaction_idempotency_key = "summon-shutdown-audio",
                      .correlation_id = "shutdown-audio",
                      .now_ms = 0},
                     [&summoned](sanguinius::VoxCommandResult result) {
                       summoned.set_value(std::move(result));
                     }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  repository.set_throw_fixture(true);
  service.stop();
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
}

TEST_CASE("Vox fails closed when DAVE is unavailable", "[vox][dave]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000901",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon({.guild_id = 10,
                          .text_channel_id = 20,
                          .actor_user_id = 31,
                          .owner_user_id = 30,
                          .interaction_idempotency_key = "summon-dave",
                          .correlation_id = "test-dave",
                          .now_ms = 0},
                         [&completed](sanguinius::VoxCommandResult result) {
                           completed.set_value(std::move(result));
                         }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready, false);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  REQUIRE(gateway.send_count() == 0);
  REQUIRE(gateway.wait_for_disconnects(1, 1s));
  REQUIRE(service.health().last_failure_category == "dave_unavailable");
  service.stop();
}

TEST_CASE("Vox keeps a disconnect before first ready ephemeral",
          "[vox][gateway][disconnect][visibility]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000925",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(service.summon(
              {.guild_id = 10,
               .text_channel_id = 20,
               .actor_user_id = 31,
               .owner_user_id = 30,
               .interaction_idempotency_key = "summon-disconnect-before-ready",
               .correlation_id = "disconnect-before-ready",
               .now_ms = 0},
              [&completed](sanguinius::VoxCommandResult result) {
                completed.set_value(std::move(result));
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));

  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  const auto transition = repository.last_transition();
  REQUIRE(transition);
  REQUIRE(transition->target == sanguinius::VoxState::failed);
  REQUIRE_FALSE(transition->public_card);
  service.stop();
}

TEST_CASE("Vox publishes a terminal card for reconnect gateway errors",
          "[vox][gateway][reconnect][error][visibility]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000926",
      [] {},       [] {}};
  service.start();

  std::promise<sanguinius::VoxCommandResult> completed;
  auto future = completed.get_future();
  REQUIRE(
      service.summon({.guild_id = 10,
                      .text_channel_id = 20,
                      .actor_user_id = 31,
                      .owner_user_id = 30,
                      .interaction_idempotency_key = "summon-reconnect-error",
                      .correlation_id = "reconnect-error",
                      .now_ms = 0},
                     [&completed](sanguinius::VoxCommandResult result) {
                       completed.set_value(std::move(result));
                     }) == sanguinius::SubmitResult::accepted);
  REQUIRE(future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));

  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::reconnecting, 1s));
  REQUIRE(gateway.wait_for_connects(2, 1s));
  gateway.emit(sanguinius::VoiceEventKind::error);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  const auto transition = repository.last_transition();
  REQUIRE(transition);
  REQUIRE(transition->target == sanguinius::VoxState::failed);
  REQUIRE(transition->public_card);
  service.stop();
}

TEST_CASE("Vox callback saturation reconciles occupancy from the cache",
          "[vox][queue][reconciliation]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000902",
      [] {},       [] {},        1};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  const sanguinius::VoxCommandContext context{.guild_id = 10,
                                              .text_channel_id = 20,
                                              .actor_user_id = 31,
                                              .owner_user_id = 30,
                                              .interaction_idempotency_key =
                                                  "summon-queue",
                                              .correlation_id = "test-queue",
                                              .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(gateway.wait_for_sends(1, 1s));

  repository.block_status();
  auto status_context = context;
  status_context.interaction_idempotency_key = "status-blocked";
  REQUIRE(service.status(status_context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  status_context.interaction_idempotency_key = "status-queued";
  REQUIRE(service.status(status_context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  gateway.emit(sanguinius::VoiceEventKind::track_marker);
  gateway.emit(sanguinius::VoiceEventKind::occupancy_changed, true, 0);
  repository.release_status();

  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::played, 1s));
  REQUIRE(repository.wait_for_empty(1s));
  const auto health = service.health();
  REQUIRE(health.callback_drops == 2);
  REQUIRE(health.reconciliations == 1);
  service.stop();
}

TEST_CASE("Vox saturated gateway callback never waits on diagnostics",
          "[vox][queue][callback][diagnostics]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  BlockingDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000912",
      [] {},       [] {},        1};
  service.start();

  sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-nonblocking-diagnostics",
      .correlation_id = "test-nonblocking-diagnostics",
      .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  repository.block_status();
  context.interaction_idempotency_key = "status-diagnostics-blocked";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  context.interaction_idempotency_key = "status-diagnostics-queued";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);

  auto callback = std::async(std::launch::async, [&gateway] {
    gateway.emit(sanguinius::VoiceEventKind::occupancy_changed, true, 0);
  });
  REQUIRE(callback.wait_for(100ms) == std::future_status::ready);
  callback.get();
  repository.release_status();
  REQUIRE(diagnostics.wait_for_entry(1s));
  diagnostics.release();
  service.stop();
}

TEST_CASE("Vox callback saturation reconciles ready from the cache",
          "[vox][queue][reconciliation][ready]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000903",
      [] {},       [] {},        1};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-ready-reconcile",
      .correlation_id = "test-ready-reconcile",
      .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));

  repository.block_status();
  context.interaction_idempotency_key = "status-ready-blocked";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  context.interaction_idempotency_key = "status-ready-queued";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  repository.release_status();

  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  const auto health = service.health();
  REQUIRE(health.callback_drops == 1);
  REQUIRE(health.reconciliations == 1);
  service.stop();
}

TEST_CASE("Vox callback saturation reconciles disconnect from the cache",
          "[vox][queue][reconciliation][disconnect]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000904",
      [] {},       [] {},        1};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-disconnect-reconcile",
      .correlation_id = "test-disconnect-reconcile",
      .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  REQUIRE(gateway.wait_for_connects(1, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));

  repository.block_status();
  context.interaction_idempotency_key = "status-disconnect-blocked";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  context.interaction_idempotency_key = "status-disconnect-queued";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  repository.release_status();

  REQUIRE(repository.wait_for_state(sanguinius::VoxState::reconnecting, 1s));
  REQUIRE(gateway.wait_for_connects(2, 1s));
  const auto health = service.health();
  REQUIRE(health.callback_drops == 1);
  REQUIRE(health.reconciliations == 1);
  service.stop();
}

TEST_CASE("Vox callback saturation preserves a bot move across departure",
          "[vox][queue][reconciliation][move][disconnect]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoxService service{
      repository,  gateway,      clock, ids,
      diagnostics, {10, 20, 30}, {},    "00000000-0000-4000-8000-000000000909",
      [] {},       [] {},        1};
  service.start();

  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  sanguinius::VoxCommandContext context{.guild_id = 10,
                                        .text_channel_id = 20,
                                        .actor_user_id = 31,
                                        .owner_user_id = 30,
                                        .interaction_idempotency_key =
                                            "summon-move-reconcile",
                                        .correlation_id = "test-move-reconcile",
                                        .now_ms = 0};
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));

  repository.block_status();
  context.interaction_idempotency_key = "status-move-blocked";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  context.interaction_idempotency_key = "status-move-queued";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  gateway.emit(sanguinius::VoiceEventKind::bot_moved, true, 1, 41);
  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  repository.release_status();

  REQUIRE(repository.wait_for_state(sanguinius::VoxState::failed, 1s));
  REQUIRE(gateway.wait_for_disconnects(1, 1s));
  REQUIRE(gateway.connect_requests().size() == 1);
  const auto health = service.health();
  REQUIRE(health.callback_drops == 2);
  REQUIRE(health.reconciliations == 1);
  REQUIRE(health.last_failure_category == "bot_moved");
  service.stop();
}

TEST_CASE("Vox reconciles a dropped test departure while already reconnecting",
          "[vox][queue][reconciliation][test-disconnect]") {
  FakeVoxRepository repository;
  FakeVoiceGateway gateway;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  const sanguinius::ControlConfiguration controls{
      .admin_commands_enabled = true, .test_mode = true};
  sanguinius::VoxService service{repository,
                                 gateway,
                                 clock,
                                 ids,
                                 diagnostics,
                                 {10, 20, 30},
                                 controls,
                                 "00000000-0000-4000-8000-000000000910",
                                 [] {},
                                 [] {},
                                 1};
  service.start();

  sanguinius::VoxCommandContext context{
      .guild_id = 10,
      .text_channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .interaction_idempotency_key = "summon-test-departure",
      .correlation_id = "test-departure-reconcile",
      .now_ms = 0};
  std::promise<sanguinius::VoxCommandResult> summoned;
  auto summon_future = summoned.get_future();
  REQUIRE(
      service.summon(context, [&summoned](sanguinius::VoxCommandResult result) {
        summoned.set_value(std::move(result));
      }) == sanguinius::SubmitResult::accepted);
  REQUIRE(summon_future.wait_for(1s) == std::future_status::ready);
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(gateway.wait_for_sends(1, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));

  auto test_context = context;
  test_context.actor_user_id = 30;
  test_context.interaction_idempotency_key = "test-disconnect-command";
  repository.set_throw_fixture(true);
  std::promise<sanguinius::VoxCommandResult> requested;
  auto request_future = requested.get_future();
  REQUIRE(service.test_disconnect(
              test_context, [&requested](sanguinius::VoxCommandResult result) {
                requested.set_value(std::move(result));
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(request_future.wait_for(1s) == std::future_status::ready);
  const auto disconnect_result = request_future.get();
  REQUIRE(disconnect_result.code == sanguinius::VoxResultCode::unavailable);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::reconnecting, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::queued, 1s));
  repository.set_throw_fixture(false);

  repository.block_status();
  context.interaction_idempotency_key = "status-test-departure-blocked";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  REQUIRE(repository.wait_for_status_entry(1s));
  context.interaction_idempotency_key = "status-test-departure-queued";
  REQUIRE(service.status(context, [](sanguinius::VoxCommandResult) {}) ==
          sanguinius::SubmitResult::accepted);
  gateway.emit(sanguinius::VoiceEventKind::disconnected);
  repository.release_status();

  REQUIRE(gateway.wait_for_connects(2, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  const auto requests = gateway.connect_requests();
  REQUIRE(requests.size() == 2);
  REQUIRE_FALSE(requests[1].validate_member_channel);
  REQUIRE(repository.wait_for_status_calls(2, 1s));
  gateway.emit(sanguinius::VoiceEventKind::ready);
  REQUIRE(repository.wait_for_state(sanguinius::VoxState::ready, 1s));
  REQUIRE(repository.wait_for_fixture(sanguinius::VoxFixtureState::failed, 1s));
  REQUIRE(gateway.send_count() == 1);
  const auto health = service.health();
  REQUIRE(health.callback_drops == 1);
  REQUIRE(health.reconciliations == 1);
  service.stop();
}
