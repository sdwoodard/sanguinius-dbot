#include "sanguinius/vox.hpp"

#include "sanguinius/tts.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t timeout_retry_delay_ms(const std::size_t attempt) {
  constexpr std::int64_t delays[]{5'000, 10'000, 20'000, 40'000};
  const auto index =
      attempt == 0 ? 0 : std::min(attempt - 1, std::size(delays) - 1);
  return delays[index];
}

[[nodiscard]] std::chrono::milliseconds
reconciliation_retry_delay(const std::size_t attempt) noexcept {
  constexpr std::chrono::milliseconds delays[]{
      std::chrono::milliseconds{50}, std::chrono::milliseconds{100},
      std::chrono::milliseconds{250}, std::chrono::milliseconds{500},
      std::chrono::milliseconds{1'000}};
  return delays[std::min(attempt, std::size(delays) - 1)];
}

[[nodiscard]] VoxResultCode result_for(const VoiceResolveStatus status) {
  switch (status) {
  case VoiceResolveStatus::ready:
    return VoxResultCode::accepted;
  case VoiceResolveStatus::no_voice:
    return VoxResultCode::no_voice;
  case VoiceResolveStatus::unsupported_channel:
    return VoxResultCode::unsupported_channel;
  case VoiceResolveStatus::permission_denied:
    return VoxResultCode::permission_denied;
  case VoiceResolveStatus::channel_full:
    return VoxResultCode::channel_full;
  case VoiceResolveStatus::unavailable:
    return VoxResultCode::unavailable;
  }
  return VoxResultCode::unavailable;
}

[[nodiscard]] std::string resolution_message(const VoiceResolveStatus status) {
  switch (status) {
  case VoiceResolveStatus::ready:
    return "The Vox connection is being established.";
  case VoiceResolveStatus::no_voice:
    return "Join a voice channel before summoning Vox Sanguinius.";
  case VoiceResolveStatus::unsupported_channel:
    return "Milestone 14 supports ordinary voice channels, not Stage channels.";
  case VoiceResolveStatus::permission_denied:
    return "Sanguinius lacks View Channel, Connect, or Speak permission there.";
  case VoiceResolveStatus::channel_full:
    return "That voice channel is full.";
  case VoiceResolveStatus::unavailable:
    return "The voice channel could not be resolved safely.";
  }
  return "Vox is unavailable.";
}

[[nodiscard]] VoxCommandResult command_result(VoxResultCode code,
                                              std::string message) {
  VoxCommandResult result;
  result.code = code;
  result.message = std::move(message);
  return result;
}

[[nodiscard]] std::int16_t triangle_sample(const std::size_t frame,
                                           const std::size_t period,
                                           const std::size_t segment_frame,
                                           const std::size_t segment_frames) {
  constexpr std::int64_t amplitude = 4'900;
  constexpr std::size_t fade_frames = 576;
  const auto phase = frame % period;
  const auto half = period / 2;
  std::int64_t sample =
      phase < half
          ? -amplitude + (4 * amplitude * static_cast<std::int64_t>(phase)) /
                             static_cast<std::int64_t>(period)
          : 3 * amplitude - (4 * amplitude * static_cast<std::int64_t>(phase)) /
                                static_cast<std::int64_t>(period);
  const auto fade_in = std::min(segment_frame, fade_frames);
  const auto remaining = segment_frames - segment_frame - 1;
  const auto fade_out = std::min(remaining, fade_frames);
  const auto gain = std::min(fade_in, fade_out);
  sample = (sample * static_cast<std::int64_t>(gain)) /
           static_cast<std::int64_t>(fade_frames);
  return static_cast<std::int16_t>(sample);
}

} // namespace

const char *
voice_resolve_status_name(const VoiceResolveStatus status) noexcept {
  switch (status) {
  case VoiceResolveStatus::ready:
    return "ready";
  case VoiceResolveStatus::no_voice:
    return "no_voice";
  case VoiceResolveStatus::unsupported_channel:
    return "unsupported_channel";
  case VoiceResolveStatus::permission_denied:
    return "permission_denied";
  case VoiceResolveStatus::channel_full:
    return "channel_full";
  case VoiceResolveStatus::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

const char *vox_state_name(const VoxState state) noexcept {
  switch (state) {
  case VoxState::connecting:
    return "connecting";
  case VoxState::ready:
    return "ready";
  case VoxState::muted:
    return "muted";
  case VoxState::reconnecting:
    return "reconnecting";
  case VoxState::leaving:
    return "leaving";
  case VoxState::inactive:
    return "inactive";
  case VoxState::failed:
    return "failed";
  }
  return "failed";
}

const char *vox_fixture_state_name(const VoxFixtureState state) noexcept {
  switch (state) {
  case VoxFixtureState::pending:
    return "pending";
  case VoxFixtureState::queued:
    return "queued";
  case VoxFixtureState::played:
    return "played";
  case VoxFixtureState::failed:
    return "failed";
  }
  return "failed";
}

bool vox_transition_allowed(const VoxState from, const VoxState to,
                            const std::string_view reason,
                            const std::size_t reconnect_count) noexcept {
  if (from == VoxState::inactive || from == VoxState::failed)
    return false;
  if (reason == "restart_abandoned" || reason == "restart_cleanup" ||
      reason == "shutdown")
    return to == VoxState::inactive;
  switch (from) {
  case VoxState::connecting:
    return to == VoxState::ready || to == VoxState::failed ||
           to == VoxState::leaving;
  case VoxState::ready:
    return to == VoxState::muted ||
           (to == VoxState::reconnecting && reconnect_count == 0) ||
           to == VoxState::leaving || to == VoxState::failed ||
           (to == VoxState::inactive && reason == "empty_timeout");
  case VoxState::muted:
    return to == VoxState::ready ||
           (to == VoxState::reconnecting && reconnect_count == 0) ||
           to == VoxState::leaving || to == VoxState::failed ||
           (to == VoxState::inactive && reason == "empty_timeout");
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
}

PcmAudio make_vox_proof_chime() {
  constexpr std::size_t sample_rate = 48'000;
  constexpr std::size_t first_frames = 11'520;
  constexpr std::size_t silence_frames = 5'760;
  constexpr std::size_t second_frames = 11'520;
  constexpr std::size_t total_frames =
      first_frames + silence_frames + second_frames;
  PcmAudio audio;
  audio.samples.reserve(total_frames * 2);
  for (std::size_t frame = 0; frame < total_frames; ++frame) {
    std::int16_t sample{};
    if (frame < first_frames) {
      sample = triangle_sample(frame, sample_rate / 400, frame, first_frames);
    } else if (frame >= first_frames + silence_frames) {
      const auto segment_frame = frame - first_frames - silence_frames;
      sample = triangle_sample(segment_frame, sample_rate / 600, segment_frame,
                               second_frames);
    }
    audio.samples.push_back(sample);
    audio.samples.push_back(sample);
  }
  return audio;
}

std::string render_vox_status(const VoxSession *session,
                              const std::int64_t current_time_ms) {
  if (session == nullptr || session->state == VoxState::inactive ||
      session->state == VoxState::failed) {
    return "Vox Sanguinius is inactive.";
  }
  std::ostringstream output;
  output << "Vox Sanguinius is " << vox_state_name(session->state) << " in <#"
         << session->voice_channel_id.str() << ">. Static proof: "
         << vox_fixture_state_name(session->fixture_state)
         << ". Reconnects: " << session->reconnect_count << ". Elapsed: "
         << (current_time_ms < 0
                 ? 0
                 : std::max<std::int64_t>(0, current_time_ms -
                                                 session->started_at_ms) /
                       1'000)
         << "s.";
  if (session->state == VoxState::muted) {
    output << " Speech is muted";
    if (session->mute_until_ms)
      output << " until <t:" << *session->mute_until_ms / 1'000 << ":R>";
    else
      output << " for this session";
    output << ".";
  }
  return output.str();
}

class VoxService::Impl {
public:
  Impl(VoxRepository &repository, VoiceGateway &gateway, const Clock &clock,
       PersistentIdGenerator &ids, Diagnostics &diagnostics,
       ServerScopeConfiguration scope, ControlConfiguration controls,
       std::string instance_id, Wake wake_scheduler, Wake wake_outbox,
       const std::size_t queue_capacity, SpeechService *speech,
       const bool contextual_narration_enabled,
       std::function<bool()> automatic_quiet,
       PrepareSessionFlavor prepare_session_flavor)
      : repository_{repository}, gateway_{gateway}, clock_{clock}, ids_{ids},
        diagnostics_{diagnostics}, scope_{std::move(scope)},
        controls_{controls}, instance_id_{std::move(instance_id)},
        wake_scheduler_{std::move(wake_scheduler)},
        wake_outbox_{std::move(wake_outbox)}, worker_{queue_capacity, 1},
        callback_responder_{queue_capacity, 1},
        resolution_capacity_{queue_capacity}, speech_{speech},
        contextual_narration_enabled_{contextual_narration_enabled},
        automatic_quiet_{std::move(automatic_quiet)},
        prepare_session_flavor_{std::move(prepare_session_flavor)} {
    if (instance_id_.empty() || !wake_scheduler_ || !wake_outbox_)
      throw std::invalid_argument{"Vox service dependencies are incomplete."};
  }

  void start() {
    if (started_.exchange(true))
      throw std::logic_error{"Vox service may only be started once."};
    callback_responder_.start();
    worker_.start();
    reconciliation_retry_thread_ =
        std::jthread{[this](const std::stop_token stop_token) {
          run_reconciliation_retries(stop_token);
        }};
    gateway_.start([this](VoiceEvent event) {
      const auto submit = worker_.try_submit_front(
          [this, event = std::move(event)](std::stop_token) mutable {
            const std::scoped_lock operation_lock{operation_mutex_};
            if (stopped_.load())
              return;
            execute_background(
                "vox.gateway_event", "vox.gateway.callback",
                [this, &event] { handle_event(std::move(event)); });
          });
      if (submit != SubmitResult::accepted) {
        ++callback_drops_;
        reconcile_required_.store(true);
        request_reconciliation_retry();
      }
    });
  }

  void stop() noexcept {
    if (stopped_.exchange(true))
      return;
    reconciliation_retry_thread_.request_stop();
    reconciliation_retry_condition_.notify_all();
    if (reconciliation_retry_thread_.joinable()) {
      try {
        reconciliation_retry_thread_.join();
      } catch (...) {
      }
    }
    {
      const std::scoped_lock operation_lock{operation_mutex_};
      std::optional<VoxSession> current;
      try {
        current = repository_.active();
      } catch (...) {
      }
      if (current) {
        const auto session_id = current->session_id;
        const auto shutdown_fixture_failure_category =
            pending_fixture_failure_ &&
                    pending_fixture_failure_->session_id == session_id
                ? std::optional{pending_fixture_failure_->failure_category}
                : std::nullopt;
        if (pending_fixture_failure_) {
          try {
            current = persist_pending_fixture_failure(*current);
          } catch (...) {
          }
        }
        if (current) {
          try {
            current = fail_queued_fixture(*current, "playback_interrupted",
                                          "vox.shutdown");
          } catch (...) {
          }
        }
        try {
          static_cast<void>(repository_.shutdown(
              now_ms(clock_), ids_.next_id(), ids_.next_id(),
              shutdown_fixture_failure_category));
        } catch (...) {
        }
        if (speech_)
          speech_->session_closed(session_id);
        teardown_transport(session_id, true);
      }
      gateway_.shutdown();
    }
    worker_.stop();
    callback_responder_.stop();
  }

  SubmitResult summon(VoxCommandContext context, Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id, [this, &context, &guarded] {
                context.now_ms = now_ms(clock_);
                if (!in_scope(context)) {
                  guarded->complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                auto preflight = repository_.preflight_summon(context);
                if (preflight.code != VoxResultCode::accepted) {
                  guarded->complete(std::move(preflight));
                  return;
                }
                if (!try_acquire_resolution()) {
                  guarded->complete(
                      command_result(VoxResultCode::unavailable,
                                     "Vox is handling another channel "
                                     "resolution. Try again."));
                  return;
                }
                auto lease =
                    std::make_shared<ResolutionLease>(resolutions_inflight_);
                gateway_.resolve_member_channel(
                    context.guild_id, context.actor_user_id,
                    [this, context = std::move(context), guarded,
                     lease](VoiceResolvedChannel resolved) mutable {
                      const auto submit = worker_.try_submit_front(
                          [this, context = std::move(context), guarded, lease,
                           resolved =
                               std::move(resolved)](std::stop_token) mutable {
                            const std::scoped_lock callback_operation_lock{
                                operation_mutex_};
                            if (stopped_.load()) {
                              guarded->unavailable();
                              lease->release();
                              return;
                            }
                            execute_command(
                                guarded, context.correlation_id,
                                [this, &context, &resolved, &guarded] {
                                  context.now_ms = now_ms(clock_);
                                  complete_summon(std::move(context),
                                                  std::move(resolved), guarded);
                                });
                            lease->release();
                          },
                          [guarded, lease] {
                            guarded->unavailable();
                            lease->release();
                          });
                      if (submit != SubmitResult::accepted) {
                        ++callback_drops_;
                        reconcile_required_.store(true);
                        request_reconciliation_retry();
                        const auto fallback = callback_responder_.try_submit(
                            [guarded, lease](std::stop_token) {
                              guarded->unavailable(
                                  "Vox is busy; try the command again.");
                              lease->release();
                            },
                            [guarded, lease] {
                              guarded->unavailable();
                              lease->release();
                            });
                        if (fallback != SubmitResult::accepted)
                          lease->release();
                      }
                    });
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult status(VoxCommandContext context, Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id, [this, &context, &guarded] {
                context.now_ms = now_ms(clock_);
                if (!in_scope(context)) {
                  guarded->complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                auto result = repository_.command_status(context);
                if (result.message.empty())
                  result.message = render_vox_status(
                      result.session ? &*result.session : nullptr,
                      context.now_ms);
                if (speech_) {
                  const auto speech_health = speech_->health();
                  result.message +=
                      " Voice: " +
                      speech_->selected_voice(context.guild_id.str()) +
                      ". Speech service: available. Queue: " +
                      std::to_string(speech_health.repository.queued +
                                     speech_health.repository.synthesizing +
                                     speech_health.repository.ready +
                                     speech_health.repository.playing) +
                      ".";
                }
                guarded->complete(std::move(result));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult leave(VoxCommandContext context, Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id, [this, &context, &guarded] {
                context.now_ms = now_ms(clock_);
                if (!in_scope(context)) {
                  guarded->complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                auto result = repository_.command_leave(context, ids_.next_id(),
                                                        ids_.next_id());
                if (result.wake_scheduler)
                  wake_scheduler_();
                if (result.code == VoxResultCode::accepted && result.session) {
                  const bool allow_contextual =
                      contextual_narration_enabled_ &&
                      !automatic_speech_suppressed();
                  const auto farewell_queued =
                      speech_ &&
                      speech_->session_leaving(result.session->session_id,
                                               result.session->guild_id.str(),
                                               allow_contextual);
                  if (!farewell_queued)
                    teardown_transport(result.session->session_id, false);
                  result.session = fail_queued_fixture(*result.session,
                                                       "playback_interrupted",
                                                       "vox.command.leave");
                }
                if (result.wake_outbox)
                  wake_outbox_();
                guarded->complete(std::move(result));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult say(VoxCommandContext context, std::string text,
                   Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), text = std::move(text),
         guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id,
              [this, &context, &text, &guarded] {
                context.now_ms = now_ms(clock_);
                const auto fingerprint =
                    "sha256:" + sha256_hex(std::as_bytes(
                                    std::span{text.data(), text.size()}));
                if (auto receipt = repository_.command_receipt(context, "say",
                                                               fingerprint)) {
                  guarded->complete(std::move(*receipt));
                  return;
                }
                const auto complete = [this, &context, &fingerprint,
                                       &guarded](VoxCommandResult result) {
                  guarded->complete(repository_.record_command_receipt(
                      context, "say", fingerprint, std::move(result)));
                };
                if (!in_scope(context) ||
                    context.actor_user_id != scope_.owner_user_id) {
                  complete(
                      command_result(VoxResultCode::unauthorized,
                                     "Only the owner may ask Vox to speak."));
                  return;
                }
                const auto current = repository_.active();
                if (!current || (current->state != VoxState::ready &&
                                 current->state != VoxState::muted)) {
                  complete(command_result(
                      VoxResultCode::invalid_state,
                      "Vox must be connected before speech can be queued."));
                  return;
                }
                if (!speech_) {
                  complete(
                      command_result(VoxResultCode::unavailable,
                                     "Generated Vox speech is unavailable."));
                  return;
                }
                const auto admitted =
                    speech_->say(current->session_id, current->guild_id.str(),
                                 std::move(text),
                                 context.interaction_idempotency_key + ":say",
                                 context.now_ms);
                VoxResultCode code = VoxResultCode::accepted;
                if (admitted.status == SpeechEnqueueStatus::replay)
                  code = VoxResultCode::replay;
                else if (admitted.status != SpeechEnqueueStatus::accepted)
                  code = VoxResultCode::unavailable;
                auto response = command_result(code, admitted.message);
                response.session = current;
                complete(std::move(response));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult mute(VoxCommandContext context, std::string duration,
                    Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), duration = std::move(duration),
         guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id,
              [this, &context, &duration, &guarded] {
                context.now_ms = now_ms(clock_);
                if (!in_scope(context)) {
                  guarded->complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                bool unmute = false;
                std::optional<std::int64_t> until;
                if (duration == "off") {
                  unmute = true;
                } else if (duration == "15m") {
                  until = context.now_ms + 15 * 60'000;
                } else if (duration == "1h") {
                  until = context.now_ms + 60 * 60'000;
                } else if (duration == "4h") {
                  until = context.now_ms + 4 * 60 * 60'000;
                } else if (duration != "session") {
                  guarded->complete(
                      command_result(VoxResultCode::invalid_state,
                                     "Choose 15m, 1h, 4h, session, or off."));
                  return;
                }
                auto result = repository_.command_mute(
                    context, unmute, until, ids_.next_id(),
                    until ? std::optional{ids_.next_id()} : std::nullopt);
                if ((result.code == VoxResultCode::accepted ||
                     result.code == VoxResultCode::replay) &&
                    result.session) {
                  const auto muted = result.session->state == VoxState::muted;
                  if (speech_) {
                    if (muted && result.session->fixture_state ==
                                     VoxFixtureState::queued) {
                      static_cast<void>(
                          gateway_.stop_audio(result.session->session_id));
                      result.session = fail_queued_fixture(
                          *result.session, "muted", "vox.command.mute");
                      speech_->session_ready(result.session->session_id,
                                             result.session->guild_id.str(),
                                             true, false);
                    } else {
                      speech_->set_muted(result.session->session_id, muted);
                    }
                  }
                  if (result.message.empty()) {
                    result.message = muted ? "Vox speech is muted; the voice "
                                             "connection remains open."
                                           : "Vox speech is no longer muted.";
                  }
                }
                if (result.wake_scheduler)
                  wake_scheduler_();
                guarded->complete(std::move(result));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult voice(VoxCommandContext context,
                     std::optional<std::string> voice, Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), voice = std::move(voice),
         guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id,
              [this, &context, &voice, &guarded] {
                context.now_ms = now_ms(clock_);
                const auto fingerprint =
                    voice ? "select:" + *voice : std::string{"inspect"};
                if (auto receipt = repository_.command_receipt(context, "voice",
                                                               fingerprint)) {
                  guarded->complete(std::move(*receipt));
                  return;
                }
                const auto complete = [this, &context, &fingerprint,
                                       &guarded](VoxCommandResult result) {
                  guarded->complete(repository_.record_command_receipt(
                      context, "voice", fingerprint, std::move(result)));
                };
                if (!in_scope(context) || !speech_) {
                  complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                if (!voice) {
                  complete(command_result(
                      VoxResultCode::accepted,
                      "The selected Vox voice is " +
                          speech_->selected_voice(context.guild_id.str()) +
                          "."));
                  return;
                }
                if (context.actor_user_id != scope_.owner_user_id) {
                  complete(command_result(
                      VoxResultCode::unauthorized,
                      "Only the owner may change the Vox voice."));
                  return;
                }
                const auto status = speech_->select_voice(
                    context.guild_id.str(), *voice, context.actor_user_id.str(),
                    context.now_ms);
                complete(command_result(
                    status == SpeechMutationStatus::invalid_state
                        ? VoxResultCode::invalid_state
                    : status == SpeechMutationStatus::unchanged
                        ? VoxResultCode::replay
                        : VoxResultCode::accepted,
                    status == SpeechMutationStatus::invalid_state
                        ? "That Vox voice is not allowed."
                        : "The selected Vox voice is " + *voice + "."));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult test_disconnect(VoxCommandContext context,
                               Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id, [this, &context, &guarded] {
                context.now_ms = now_ms(clock_);
                if (!in_scope(context)) {
                  guarded->complete(command_result(
                      VoxResultCode::unauthorized,
                      "Use Vox in the configured primary channel."));
                  return;
                }
                if (!controls_.admin_commands_enabled || !controls_.test_mode ||
                    context.actor_user_id != scope_.owner_user_id) {
                  guarded->complete(command_result(
                      controls_.test_mode ? VoxResultCode::unauthorized
                                          : VoxResultCode::test_mode_disabled,
                      "This owner test control is unavailable."));
                  return;
                }
                auto result = repository_.command_test_disconnect(
                    context, ids_.next_id(), ids_.next_id());
                if (result.wake_scheduler)
                  wake_scheduler_();
                if (result.code == VoxResultCode::accepted && result.session) {
                  if (speech_)
                    speech_->session_reconnecting(result.session->session_id);
                  teardown_transport(result.session->session_id, false);
                  result.session = fail_queued_fixture(
                      *result.session, "playback_interrupted",
                      "vox.command.test_disconnect");
                }
                guarded->complete(std::move(result));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult speech_test(VoxCommandContext context, std::string scenario,
                           Completion completion) {
    context.owner_user_id = scope_.owner_user_id;
    const auto guarded =
        make_completion(std::move(completion), context.correlation_id);
    return worker_.try_submit(
        [this, context = std::move(context), scenario = std::move(scenario),
         guarded](std::stop_token) mutable {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            guarded->unavailable();
            return;
          }
          execute_command(
              guarded, context.correlation_id,
              [this, &context, &scenario, &guarded] {
                context.now_ms = now_ms(clock_);
                const auto fingerprint = "scenario:" + scenario;
                if (auto receipt = repository_.command_receipt(
                        context, "speech_test", fingerprint)) {
                  guarded->complete(std::move(*receipt));
                  return;
                }
                const auto complete = [this, &context, &fingerprint,
                                       &guarded](VoxCommandResult result) {
                  guarded->complete(repository_.record_command_receipt(
                      context, "speech_test", fingerprint, std::move(result)));
                };
                const bool valid_scenario = scenario == "queue" ||
                                            scenario == "provider-failure" ||
                                            scenario == "budget-limit" ||
                                            scenario == "narration-stale";
                if (!in_scope(context) ||
                    context.actor_user_id != scope_.owner_user_id ||
                    !controls_.admin_commands_enabled || !controls_.test_mode ||
                    !valid_scenario) {
                  complete(command_result(
                      controls_.test_mode ? VoxResultCode::unauthorized
                                          : VoxResultCode::test_mode_disabled,
                      "This owner speech test is unavailable."));
                  return;
                }
                const auto current = repository_.active();
                if (!current ||
                    (current->state != VoxState::ready &&
                     current->state != VoxState::muted) ||
                    !speech_) {
                  complete(command_result(
                      VoxResultCode::invalid_state,
                      "Vox must be connected for a speech test."));
                  return;
                }
                const auto queued = speech_->run_test_scenario(
                    current->session_id, current->guild_id.str(), scenario,
                    context.interaction_idempotency_key);
                complete(command_result(
                    queued ? VoxResultCode::accepted
                           : VoxResultCode::unavailable,
                    queued
                        ? "The deterministic " + scenario +
                              " speech scenario was queued without a provider "
                              "call."
                        : "The deterministic speech scenario queue is full."));
              });
        },
        [guarded] { guarded->unavailable(); });
  }

  SubmitResult handle_timeout(const ClaimedScheduledJob &job) {
    return worker_.try_submit(
        [this, job](std::stop_token) {
          const std::scoped_lock operation_lock{operation_mutex_};
          if (stopped_.load()) {
            settle_timeout_failure(job, "handler_stopping");
            return;
          }
          try {
            if (pending_fixture_failure_) {
              if (auto current = repository_.active())
                static_cast<void>(persist_pending_fixture_failure(*current));
            }
            std::optional<std::size_t> observed_humans;
            if (job.job_type == vox_empty_timeout_job_type) {
              if (const auto *payload =
                      std::get_if<VoxTimeoutJobPayload>(&job.payload)) {
                const auto snapshot = gateway_.snapshot(payload->session_id);
                if (snapshot.bound)
                  observed_humans = snapshot.human_count;
              }
            }
            auto result = repository_.handle_timeout(
                job, now_ms(clock_), ids_.next_id(), ids_.next_id(),
                ids_.next_id(), observed_humans);
            if (result.session && result.session->last_failure_category)
              record_failure(*result.session->last_failure_category);
            if (result.code == VoxResultCode::accepted && result.session) {
              if (job.job_type == vox_mute_expiry_job_type) {
                if (speech_)
                  speech_->set_muted(result.session->session_id, false);
              } else {
                if (speech_)
                  speech_->session_closed(result.session->session_id);
                teardown_transport(result.session->session_id, true);
              }
            }
            if (result.wake_outbox)
              wake_outbox_();
          } catch (...) {
            emit_internal_failure(
                "vox.timeout",
                "A Vox timeout failed inside its worker boundary.",
                job.correlation_id);
            try {
              record_failure("internal_error");
            } catch (...) {
            }
            settle_timeout_failure(job, "handler_exception");
          }
          reconcile_guarded(job.correlation_id);
        },
        [this, job] { settle_timeout_failure(job, "handler_stopping"); });
  }

  VoxHealth health() const {
    const auto current = repository_.active();
    const auto gateway = current ? gateway_.snapshot(current->session_id)
                                 : VoiceGatewaySnapshot{};
    std::optional<std::string> last_failure;
    {
      const std::scoped_lock lock{health_mutex_};
      last_failure = last_failure_category_;
    }
    return {.enabled = true,
            .state = current ? std::optional{current->state} : std::nullopt,
            .fixture_state =
                current ? std::optional{current->fixture_state} : std::nullopt,
            .dave_active = gateway.dave_active,
            .reconnect_count = current ? current->reconnect_count : 0,
            .callback_drops = callback_drops_.load(),
            .reconciliations = reconciliations_.load(),
            .queue = worker_.snapshot(),
            .last_failure_category = current && current->last_failure_category
                                         ? current->last_failure_category
                                         : std::move(last_failure),
            .speech =
                speech_ ? std::optional{speech_->health()} : std::nullopt};
  }

private:
  class CompletionState final {
  public:
    CompletionState(Completion completion, Diagnostics &diagnostics,
                    std::string correlation_id)
        : completion_{std::move(completion)}, diagnostics_{diagnostics},
          correlation_id_{std::move(correlation_id)} {}

    void complete(VoxCommandResult result) noexcept {
      if (completed_.exchange(true))
        return;
      try {
        completion_(std::move(result));
      } catch (...) {
        diagnostics_.emit({DiagnosticSeverity::error, "vox.completion",
                           "A Vox command response could not be delivered.",
                           correlation_id_});
      }
    }

    void unavailable(
        const std::string_view message =
            "Vox could not complete that request. Try again.") noexcept {
      VoxCommandResult result;
      result.code = VoxResultCode::unavailable;
      try {
        result.message = message;
      } catch (...) {
      }
      complete(std::move(result));
    }

  private:
    Completion completion_;
    Diagnostics &diagnostics_;
    std::optional<std::string> correlation_id_;
    std::atomic<bool> completed_{false};
  };

  class ResolutionLease final {
  public:
    explicit ResolutionLease(
        std::shared_ptr<std::atomic<std::size_t>> resolutions_inflight)
        : resolutions_inflight_{std::move(resolutions_inflight)} {}
    ~ResolutionLease() { release(); }

    ResolutionLease(const ResolutionLease &) = delete;
    ResolutionLease &operator=(const ResolutionLease &) = delete;

    void release() noexcept {
      if (!released_.exchange(true))
        --*resolutions_inflight_;
    }

  private:
    std::shared_ptr<std::atomic<std::size_t>> resolutions_inflight_;
    std::atomic<bool> released_{false};
  };

  [[nodiscard]] std::shared_ptr<CompletionState>
  make_completion(Completion completion, std::string correlation_id) {
    return std::make_shared<CompletionState>(
        std::move(completion), diagnostics_, std::move(correlation_id));
  }

  void emit_internal_failure(const std::string_view category,
                             const std::string_view message,
                             const std::string_view correlation_id) noexcept {
    diagnostics_.emit({DiagnosticSeverity::error, std::string{category},
                       std::string{message}, std::string{correlation_id}});
  }

  template <typename Operation>
  void execute_command(const std::shared_ptr<CompletionState> &completion,
                       const std::string_view correlation_id,
                       Operation &&operation) noexcept {
    try {
      std::forward<Operation>(operation)();
    } catch (...) {
      try {
        record_failure("internal_error");
      } catch (...) {
      }
      emit_internal_failure("vox.command",
                            "A Vox command failed inside its worker boundary.",
                            correlation_id);
      completion->unavailable();
    }
    reconcile_guarded(correlation_id);
  }

  template <typename Operation>
  void execute_background(const std::string_view category,
                          const std::string_view correlation_id,
                          Operation &&operation) noexcept {
    try {
      std::forward<Operation>(operation)();
    } catch (...) {
      reconcile_required_.store(true);
      emit_internal_failure(
          category,
          "A Vox background operation failed inside its worker boundary.",
          correlation_id);
      try {
        record_failure("internal_error");
      } catch (...) {
      }
    }
    reconcile_guarded(correlation_id);
  }

  void reconcile_guarded(const std::string_view correlation_id) noexcept {
    try {
      reconcile_if_required();
      reconciliation_retry_attempt_.store(0);
    } catch (...) {
      reconcile_required_.store(true);
      request_reconciliation_retry();
      emit_internal_failure(
          "vox.reconciliation",
          "Vox cache reconciliation failed inside its worker boundary.",
          correlation_id);
      try {
        record_failure("internal_error");
      } catch (...) {
      }
    }
  }

  void request_reconciliation_retry() noexcept {
    if (stopped_.load())
      return;
    {
      const std::scoped_lock lock{reconciliation_retry_mutex_};
      reconciliation_retry_requested_ = true;
    }
    reconciliation_retry_condition_.notify_one();
  }

  void run_reconciliation_retries(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      std::size_t attempt{};
      {
        std::unique_lock lock{reconciliation_retry_mutex_};
        reconciliation_retry_condition_.wait(lock, stop_token, [this] {
          return reconciliation_retry_requested_;
        });
        if (stop_token.stop_requested())
          return;
        reconciliation_retry_requested_ = false;
        attempt = reconciliation_retry_attempt_.fetch_add(1);
        static_cast<void>(reconciliation_retry_condition_.wait_for(
            lock, stop_token, reconciliation_retry_delay(attempt),
            [] { return false; }));
      }
      if (stop_token.stop_requested() || stopped_.load())
        return;
      const auto submit = worker_.try_submit_front([this](std::stop_token) {
        const std::scoped_lock operation_lock{operation_mutex_};
        if (stopped_.load())
          return;
        reconcile_guarded("vox.reconciliation.retry");
      });
      if (submit != SubmitResult::accepted)
        request_reconciliation_retry();
    }
  }

  void settle_timeout_failure(const ClaimedScheduledJob &job,
                              std::string error_code) noexcept {
    try {
      const auto current = now_ms(clock_);
      const auto status = repository_.fail_timeout_job(
          job, current, current + timeout_retry_delay_ms(job.attempt_count),
          std::move(error_code), true);
      if (status == WorkMutationStatus::applied)
        wake_scheduler_();
    } catch (...) {
      emit_internal_failure(
          "vox.timeout.settlement",
          "A Vox timeout claim could not be settled after worker failure.",
          job.correlation_id);
    }
  }

  [[nodiscard]] bool try_acquire_resolution() noexcept {
    auto current = resolutions_inflight_->load();
    while (current < resolution_capacity_) {
      if (resolutions_inflight_->compare_exchange_weak(current, current + 1))
        return true;
    }
    return false;
  }

  void complete_summon(VoxCommandContext context, VoiceResolvedChannel resolved,
                       const std::shared_ptr<CompletionState> &completion) {
    context.now_ms = now_ms(clock_);
    if (resolved.status != VoiceResolveStatus::ready) {
      completion->complete(repository_.record_summon_rejection(
          context, result_for(resolved.status),
          resolution_message(resolved.status)));
      return;
    }
    VoxStartRequest request{
        .context = context,
        .voice_channel_id = resolved.channel_id,
        .session_id = ids_.next_id(),
        .event_id = ids_.next_id(),
        .timeout_job_id = ids_.next_id(),
        .deployment_instance_id = instance_id_,
    };
    auto result = repository_.start(request);
    if (result.code == VoxResultCode::accepted && result.session) {
      if (contextual_narration_enabled_ && speech_ &&
          prepare_session_flavor_ && !automatic_speech_suppressed()) {
        speech_->begin_session_flavor(result.session->session_id);
        prepare_session_flavor_(result.session->session_id,
                                result.session->guild_id.str(),
                                result.session->summoner_user_id.str());
      }
      const auto wake_scheduler = result.wake_scheduler;
      const auto submit =
          gateway_.connect({.session_id = result.session->session_id,
                            .guild_id = result.session->guild_id,
                            .channel_id = result.session->voice_channel_id,
                            .member_user_id = result.session->summoner_user_id,
                            .generation = result.session->connection_generation,
                            .validate_member_channel = true});
      const bool gateway_accepted = submit == VoiceGatewaySubmit::accepted;
      if (!gateway_accepted)
        record_failure("gateway_unavailable");
      result = repository_.finalize_summon(context, result.session->session_id,
                                           result.session->revision,
                                           gateway_accepted, ids_.next_id());
      if (!gateway_accepted && result.session) {
        if (speech_)
          speech_->discard_session_flavor(result.session->session_id);
        gateway_.release_binding(result.session->session_id);
      }
      result.wake_scheduler = result.wake_scheduler || wake_scheduler;
    }
    if (result.wake_scheduler)
      wake_scheduler_();
    completion->complete(std::move(result));
  }

  void handle_event(VoiceEvent event) {
    auto current = repository_.active();
    if (!current || current->session_id != event.session_id)
      return;
    const bool prior_generation_event =
        (event.kind == VoiceEventKind::disconnected ||
         event.kind == VoiceEventKind::bot_moved) &&
        (current->state == VoxState::reconnecting ||
         current->state == VoxState::leaving) &&
        current->connection_generation == event.generation + 1;
    if (current->connection_generation != event.generation &&
        !prior_generation_event)
      return;
    const auto current_time = now_ms(clock_);
    if (event.kind == VoiceEventKind::ready) {
      const bool completing_connection =
          current->state == VoxState::connecting ||
          current->state == VoxState::reconnecting;
      if (!event.dave_active) {
        fail(*current, "dave_unavailable",
             current->state != VoxState::connecting);
        return;
      }
      if (event.channel_id != current->voice_channel_id) {
        fail(*current, "bot_moved", current->state != VoxState::connecting);
        return;
      }
      if (current->state == VoxState::connecting ||
          current->state == VoxState::reconnecting) {
        const bool initial = current->state == VoxState::connecting;
        if (!initial)
          current = fail_queued_fixture(*current, "playback_interrupted",
                                        "vox.gateway.ready");
        const auto restore_mute = !initial && current->muted_at_ms.has_value();
        auto result = repository_.transition(
            {.session_id = current->session_id,
             .expected_revision = current->revision,
             .target = restore_mute ? VoxState::muted : VoxState::ready,
             .reason = initial ? "voice_ready" : "voice_reconnected",
             .actor_user_id = std::nullopt,
             .event_id = ids_.next_id(),
             .idempotency_key =
                 "vox:ready:" + current->session_id +
                 (initial ? ":initial"
                          : ":reconnect:" +
                                std::to_string(current->reconnect_count)),
             .correlation_id = "vox.gateway.ready",
             .now_ms = current_time,
             .timeout_job_id = std::nullopt,
             .timeout_due_at_ms = std::nullopt,
             .failure_category = std::nullopt,
             .public_card = initial},
            initial ? std::optional{ids_.next_id()} : std::nullopt);
        if (result.wake_outbox)
          wake_outbox_();
        if (result.session)
          current = result.session;
        rejoin_dispatched_session_.clear();
      }
      if (current && current->state == VoxState::ready &&
          current->fixture_state == VoxFixtureState::pending) {
        const auto marker = "vox-proof:" + current->session_id + ":" +
                            std::to_string(current->connection_generation);
        auto queued = repository_.fixture(
            {.session_id = current->session_id,
             .expected_revision = current->revision,
             .target = VoxFixtureState::queued,
             .marker = marker,
             .event_id = ids_.next_id(),
             .idempotency_key = "vox:fixture-queued:" + current->session_id,
             .correlation_id = "vox.gateway.ready",
             .now_ms = current_time,
             .failure_category = std::nullopt});
        if (queued.code == VoxResultCode::accepted && queued.session) {
          auto entrance = speech_ && contextual_narration_enabled_ &&
                                  !automatic_speech_suppressed()
                              ? speech_->take_prepared_entrance(
                                    queued.session->session_id)
                              : std::nullopt;
          if (speech_ && !entrance)
            static_cast<void>(
                speech_->take_prepared_entrance(queued.session->session_id));
          if (gateway_.send_pcm(queued.session->session_id,
                                entrance ? *entrance
                                : speech_ ? speech_->entrance_clip()
                                          : make_vox_proof_chime(),
                                marker) != VoiceGatewaySubmit::accepted) {
            pending_fixture_failure_ =
                PendingFixtureFailure{.session_id = queued.session->session_id,
                                      .marker = marker,
                                      .failure_category = "audio_rejected",
                                      .correlation_id = "vox.gateway.send"};
            persist_pending_fixture_failure(*queued.session);
          }
        }
      }
      const auto ready_session = repository_.active();
      if (ready_session && (ready_session->state == VoxState::ready ||
                            ready_session->state == VoxState::muted)) {
        if (speech_ && completing_connection &&
            ready_session->fixture_state != VoxFixtureState::queued)
          speech_->session_ready(
              ready_session->session_id, ready_session->guild_id.str(),
              ready_session->state == VoxState::muted, false);
        update_occupancy(*ready_session, event.human_count,
                         "vox.gateway.ready.occupancy");
      }
      return;
    }
    if (event.kind == VoiceEventKind::track_marker) {
      if (speech_ &&
          !speech_->track_marker(current->session_id, event.marker)) {
        reconcile_required_.store(true);
        request_reconciliation_retry();
      }
      if (current->state == VoxState::leaving &&
          event.marker.starts_with("speech:farewell:")) {
        teardown_transport(current->session_id, false);
        return;
      }
      if (current->fixture_state == VoxFixtureState::queued &&
          current->fixture_marker == event.marker) {
        const auto played = repository_.fixture(
            {.session_id = current->session_id,
             .expected_revision = current->revision,
             .target = VoxFixtureState::played,
             .marker = event.marker,
             .event_id = ids_.next_id(),
             .idempotency_key = "vox:fixture-played:" + current->session_id,
             .correlation_id = "vox.gateway.marker",
             .now_ms = current_time,
             .failure_category = std::nullopt});
        if (speech_ && played.code == VoxResultCode::accepted &&
            played.session) {
          speech_->session_ready(played.session->session_id,
                                 played.session->guild_id.str(), false, false);
        }
      }
      return;
    }
    if (event.kind == VoiceEventKind::occupancy_changed) {
      update_occupancy(*current, event.human_count, "vox.gateway.occupancy");
      return;
    }
    if (event.kind == VoiceEventKind::bot_moved) {
      if (current->state == VoxState::leaving) {
        static_cast<void>(gateway_.disconnect(current->session_id));
        return;
      }
      fail(*current, "bot_moved", current->state != VoxState::connecting);
      return;
    }
    if (event.kind == VoiceEventKind::disconnected) {
      if (current->state == VoxState::leaving) {
        current = fail_queued_fixture(*current, "playback_interrupted",
                                      "vox.gateway.leave");
        auto result = repository_.transition(
            {.session_id = current->session_id,
             .expected_revision = current->revision,
             .target = VoxState::inactive,
             .reason = "commanded_leave_complete",
             .actor_user_id = std::nullopt,
             .event_id = ids_.next_id(),
             .idempotency_key = "vox:leave-complete:" + current->session_id,
             .correlation_id = "vox.gateway.leave",
             .now_ms = current_time,
             .timeout_job_id = std::nullopt,
             .timeout_due_at_ms = std::nullopt,
             .failure_category = std::nullopt,
             .public_card = true},
            ids_.next_id());
        if (result.wake_outbox)
          wake_outbox_();
        if (result.code == VoxResultCode::accepted && result.session) {
          if (speech_)
            speech_->session_closed(result.session->session_id);
          gateway_.release_binding(result.session->session_id);
        }
      } else if (current->state == VoxState::reconnecting) {
        current = fail_queued_fixture(*current, "playback_interrupted",
                                      "vox.gateway.reconnect");
        dispatch_rejoin_once(*current);
      } else if ((current->state == VoxState::ready ||
                  current->state == VoxState::muted) &&
                 current->reconnect_count == 0) {
        current = fail_queued_fixture(*current, "playback_interrupted",
                                      "vox.gateway.disconnect");
        auto result = repository_.transition(
            {.session_id = current->session_id,
             .expected_revision = current->revision,
             .target = VoxState::reconnecting,
             .reason = "unexpected_disconnect",
             .actor_user_id = std::nullopt,
             .event_id = ids_.next_id(),
             .idempotency_key = "vox:reconnecting:" + current->session_id,
             .correlation_id = "vox.gateway.disconnect",
             .now_ms = current_time,
             .timeout_job_id = ids_.next_id(),
             .timeout_due_at_ms = current_time + vox_connect_timeout_ms,
             .failure_category = std::nullopt,
             .public_card = false});
        if (result.wake_scheduler)
          wake_scheduler_();
        if (result.session) {
          if (speech_)
            speech_->session_reconnecting(result.session->session_id);
          dispatch_rejoin_once(*result.session);
        }
      } else if (current->state != VoxState::leaving) {
        fail(*current, "unexpected_disconnect",
             current->state != VoxState::connecting);
      }
      return;
    }
    if (event.kind == VoiceEventKind::error)
      fail(*current,
           event.failure_category.empty() ? "gateway_error"
                                          : event.failure_category,
           current->state != VoxState::connecting &&
               current->state != VoxState::leaving);
  }

  void fail(const VoxSession &current, std::string category,
            const bool public_card) {
    if (speech_)
      speech_->session_closed(current.session_id);
    teardown_transport(current.session_id, false);
    rejoin_dispatched_session_.clear();
    record_failure(category);
    const auto transition_session = fail_queued_fixture(
        current, "playback_interrupted", "vox.gateway.failure");
    auto result = repository_.transition(
        {.session_id = transition_session.session_id,
         .expected_revision = transition_session.revision,
         .target = VoxState::failed,
         .reason = "voice_failure",
         .actor_user_id = std::nullopt,
         .event_id = ids_.next_id(),
         .idempotency_key = "vox:failed:" + current.session_id + ":" + category,
         .correlation_id = "vox.gateway.failure",
         .now_ms = now_ms(clock_),
         .timeout_job_id = std::nullopt,
         .timeout_due_at_ms = std::nullopt,
         .failure_category = std::move(category),
         .public_card = public_card},
        public_card ? std::optional{ids_.next_id()} : std::nullopt);
    gateway_.release_binding(current.session_id);
    if (result.wake_outbox)
      wake_outbox_();
  }

  void teardown_transport(const std::string_view session_id,
                          const bool release_binding) noexcept {
    try {
      static_cast<void>(gateway_.stop_audio(session_id));
    } catch (...) {
    }
    try {
      static_cast<void>(gateway_.disconnect(session_id));
    } catch (...) {
    }
    if (release_binding)
      gateway_.release_binding(session_id);
  }

  struct PendingFixtureFailure {
    std::string session_id;
    std::string marker;
    std::string failure_category;
    std::string correlation_id;
  };

  [[nodiscard]] VoxSession
  fail_queued_fixture(const VoxSession &current,
                      const std::string_view failure_category,
                      const std::string_view correlation_id) {
    if (pending_fixture_failure_ &&
        pending_fixture_failure_->session_id == current.session_id)
      return persist_pending_fixture_failure(current);
    if (current.fixture_state != VoxFixtureState::queued ||
        !current.fixture_marker)
      return current;
    auto result = repository_.fixture(
        {.session_id = current.session_id,
         .expected_revision = current.revision,
         .target = VoxFixtureState::failed,
         .marker = *current.fixture_marker,
         .event_id = ids_.next_id(),
         .idempotency_key = "vox:fixture-interrupted:" + current.session_id,
         .correlation_id = std::string{correlation_id},
         .now_ms = now_ms(clock_),
         .failure_category = std::string{failure_category}});
    if (result.session)
      return *result.session;
    return current;
  }

  VoxSession persist_pending_fixture_failure(const VoxSession &current) {
    if (!pending_fixture_failure_)
      return current;
    const auto pending = *pending_fixture_failure_;
    if (current.session_id != pending.session_id) {
      pending_fixture_failure_.reset();
      return current;
    }
    if (current.fixture_state == VoxFixtureState::played) {
      pending_fixture_failure_.reset();
      return current;
    }
    if (current.fixture_state == VoxFixtureState::failed) {
      if (current.last_failure_category != pending.failure_category)
        throw std::runtime_error{
            "Rejected Vox audio was persisted with a different failure cause."};
      pending_fixture_failure_.reset();
      return current;
    }
    if (current.fixture_state != VoxFixtureState::queued ||
        current.fixture_marker != pending.marker) {
      throw std::runtime_error{
          "Rejected Vox audio no longer matches its queued fixture."};
    }
    const auto result = repository_.fixture(
        {.session_id = pending.session_id,
         .expected_revision = current.revision,
         .target = VoxFixtureState::failed,
         .marker = pending.marker,
         .event_id = ids_.next_id(),
         .idempotency_key = "vox:fixture-failed:" + pending.session_id,
         .correlation_id = pending.correlation_id,
         .now_ms = now_ms(clock_),
         .failure_category = pending.failure_category});
    if (result.session &&
        result.session->fixture_state == VoxFixtureState::failed &&
        result.session->last_failure_category == pending.failure_category) {
      pending_fixture_failure_.reset();
      return *result.session;
    }
    throw std::runtime_error{
        "Rejected Vox audio could not be persisted as failed."};
  }

  void record_failure(const std::string_view category) {
    const std::scoped_lock lock{health_mutex_};
    last_failure_category_ = std::string{category};
  }

  [[nodiscard]] bool automatic_speech_suppressed() noexcept {
    if (!automatic_quiet_)
      return false;
    try {
      return automatic_quiet_();
    } catch (...) {
      record_failure("narration_quiet_check_failed");
      return true;
    }
  }

  void update_occupancy(const VoxSession &session,
                        const std::size_t human_count,
                        const std::string_view correlation_id) {
    const bool becoming_empty = human_count == 0 && !session.empty_since_ms;
    const bool becoming_occupied = human_count > 0 && session.empty_since_ms;
    if (!becoming_empty && !becoming_occupied)
      return;
    auto result = repository_.occupancy(
        {.session_id = session.session_id,
         .expected_revision = session.revision,
         .human_count = human_count,
         .now_ms = now_ms(clock_),
         .empty_job_id =
             becoming_empty ? std::optional{ids_.next_id()} : std::nullopt,
         .event_id = ids_.next_id(),
         .idempotency_key = "vox:occupancy:" + session.session_id + ":" +
                            std::to_string(session.revision) + ":" +
                            std::to_string(human_count),
         .correlation_id = std::string{correlation_id}});
    if (result.wake_scheduler)
      wake_scheduler_();
  }

  [[nodiscard]] bool in_scope(const VoxCommandContext &context) const noexcept {
    return context.guild_id == scope_.guild_id &&
           context.text_channel_id == scope_.primary_channel_id;
  }

  void dispatch_rejoin_once(const VoxSession &session) {
    if (rejoin_dispatched_session_ == session.session_id)
      return;
    rejoin_dispatched_session_ = session.session_id;
    if (gateway_.connect({.session_id = session.session_id,
                          .guild_id = session.guild_id,
                          .channel_id = session.voice_channel_id,
                          .member_user_id = {},
                          .generation = session.connection_generation,
                          .validate_member_channel = false}) !=
        VoiceGatewaySubmit::accepted)
      fail(session, "reconnect_rejected", true);
  }

  void reconcile_if_required() {
    if (!reconcile_required_.exchange(false))
      return;
    ++reconciliations_;
    diagnostics_.emit({DiagnosticSeverity::warning, "vox.callback_queue",
                       "A Vox callback was coalesced for cache reconciliation.",
                       std::nullopt});
    auto current = repository_.active();
    if (!current) {
      pending_fixture_failure_.reset();
      return;
    }
    if (pending_fixture_failure_) {
      current = persist_pending_fixture_failure(*current);
    }
    const auto snapshot = gateway_.snapshot(current->session_id);
    if (snapshot.bound &&
        (snapshot.bot_moved ||
         (snapshot.observed_channel_id.is_set() &&
          snapshot.observed_channel_id != current->voice_channel_id)) &&
        current->state != VoxState::leaving) {
      handle_event({.kind = VoiceEventKind::bot_moved,
                    .session_id = current->session_id,
                    .guild_id = current->guild_id,
                    .channel_id = snapshot.observed_channel_id,
                    .generation = snapshot.generation,
                    .human_count = snapshot.human_count,
                    .dave_active = false,
                    .marker = {},
                    .failure_category = {}});
      return;
    }
    if (snapshot.ready &&
        snapshot.generation == current->connection_generation &&
        (current->state == VoxState::connecting ||
         current->state == VoxState::reconnecting ||
         (current->state == VoxState::ready &&
          current->fixture_state == VoxFixtureState::pending))) {
      handle_event({.kind = VoiceEventKind::ready,
                    .session_id = current->session_id,
                    .guild_id = current->guild_id,
                    .channel_id = snapshot.channel_id,
                    .generation = snapshot.generation,
                    .human_count = snapshot.human_count,
                    .dave_active = snapshot.dave_active,
                    .marker = {},
                    .failure_category = {}});
      return;
    }
    if (!snapshot.bound ||
        (!snapshot.connected && (current->state == VoxState::ready ||
                                 current->state == VoxState::reconnecting ||
                                 current->state == VoxState::leaving))) {
      auto departure_generation = current->connection_generation;
      if (snapshot.bound)
        departure_generation = snapshot.generation;
      handle_event({.kind = VoiceEventKind::disconnected,
                    .session_id = current->session_id,
                    .guild_id = current->guild_id,
                    .channel_id = current->voice_channel_id,
                    .generation = departure_generation,
                    .human_count = 0,
                    .dave_active = false,
                    .marker = {},
                    .failure_category = {}});
      return;
    }
    if (snapshot.marker_completed && !snapshot.completed_marker.empty()) {
      handle_event({.kind = VoiceEventKind::track_marker,
                    .session_id = current->session_id,
                    .guild_id = current->guild_id,
                    .channel_id = current->voice_channel_id,
                    .generation = current->connection_generation,
                    .human_count = snapshot.human_count,
                    .dave_active = snapshot.dave_active,
                    .marker = snapshot.completed_marker,
                    .failure_category = {}});
    }
    const auto refreshed = repository_.active();
    if (refreshed &&
        ((snapshot.human_count == 0 && !refreshed->empty_since_ms) ||
         (snapshot.human_count > 0 && refreshed->empty_since_ms))) {
      handle_event({.kind = VoiceEventKind::occupancy_changed,
                    .session_id = refreshed->session_id,
                    .guild_id = refreshed->guild_id,
                    .channel_id = refreshed->voice_channel_id,
                    .generation = refreshed->connection_generation,
                    .human_count = snapshot.human_count,
                    .dave_active = snapshot.dave_active,
                    .marker = {},
                    .failure_category = {}});
    }
  }

  VoxRepository &repository_;
  VoiceGateway &gateway_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  ServerScopeConfiguration scope_;
  ControlConfiguration controls_;
  std::string instance_id_;
  Wake wake_scheduler_;
  Wake wake_outbox_;
  BoundedExecutor worker_;
  BoundedExecutor callback_responder_;
  const std::size_t resolution_capacity_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<bool> reconcile_required_{false};
  std::atomic<std::size_t> reconciliation_retry_attempt_{0};
  std::mutex reconciliation_retry_mutex_;
  std::condition_variable_any reconciliation_retry_condition_;
  bool reconciliation_retry_requested_{};
  std::jthread reconciliation_retry_thread_;
  std::shared_ptr<std::atomic<std::size_t>> resolutions_inflight_{
      std::make_shared<std::atomic<std::size_t>>(0)};
  std::atomic<std::size_t> callback_drops_{0};
  std::atomic<std::size_t> reconciliations_{0};
  mutable std::mutex operation_mutex_;
  mutable std::mutex health_mutex_;
  std::optional<std::string> last_failure_category_;
  std::string rejoin_dispatched_session_;
  std::optional<PendingFixtureFailure> pending_fixture_failure_;
  SpeechService *speech_{};
  bool contextual_narration_enabled_{};
  std::function<bool()> automatic_quiet_;
  PrepareSessionFlavor prepare_session_flavor_;
};

VoxService::VoxService(VoxRepository &repository, VoiceGateway &gateway,
                       const Clock &clock, PersistentIdGenerator &ids,
                       Diagnostics &diagnostics, ServerScopeConfiguration scope,
                       ControlConfiguration controls, std::string instance_id,
                       Wake wake_scheduler, Wake wake_outbox,
                       const std::size_t queue_capacity, SpeechService *speech,
                       const bool contextual_narration_enabled,
                       std::function<bool()> automatic_quiet,
                       PrepareSessionFlavor prepare_session_flavor)
    : impl_{std::make_unique<Impl>(
          repository, gateway, clock, ids, diagnostics, std::move(scope),
          controls, std::move(instance_id), std::move(wake_scheduler),
          std::move(wake_outbox), queue_capacity, speech,
          contextual_narration_enabled, std::move(automatic_quiet),
          std::move(prepare_session_flavor))} {}

VoxService::~VoxService() { stop(); }
void VoxService::start() { impl_->start(); }
void VoxService::stop() noexcept { impl_->stop(); }
SubmitResult VoxService::summon(VoxCommandContext context,
                                Completion completion) {
  return impl_->summon(std::move(context), std::move(completion));
}
SubmitResult VoxService::status(VoxCommandContext context,
                                Completion completion) {
  return impl_->status(std::move(context), std::move(completion));
}
SubmitResult VoxService::leave(VoxCommandContext context,
                               Completion completion) {
  return impl_->leave(std::move(context), std::move(completion));
}
SubmitResult VoxService::say(VoxCommandContext context, std::string text,
                             Completion completion) {
  return impl_->say(std::move(context), std::move(text), std::move(completion));
}
SubmitResult VoxService::mute(VoxCommandContext context, std::string duration,
                              Completion completion) {
  return impl_->mute(std::move(context), std::move(duration),
                     std::move(completion));
}
SubmitResult VoxService::voice(VoxCommandContext context,
                               std::optional<std::string> voice,
                               Completion completion) {
  return impl_->voice(std::move(context), std::move(voice),
                      std::move(completion));
}
SubmitResult VoxService::test_disconnect(VoxCommandContext context,
                                         Completion completion) {
  return impl_->test_disconnect(std::move(context), std::move(completion));
}
SubmitResult VoxService::speech_test(VoxCommandContext context,
                                     std::string scenario,
                                     Completion completion) {
  return impl_->speech_test(std::move(context), std::move(scenario),
                            std::move(completion));
}
SubmitResult VoxService::handle_timeout(const ClaimedScheduledJob &job) {
  return impl_->handle_timeout(job);
}
VoxHealth VoxService::health() const { return impl_->health(); }

} // namespace sanguinius
