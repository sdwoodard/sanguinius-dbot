#include "sanguinius/voice_input.hpp"

#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] bool terminal(const VoiceListeningState state) noexcept {
  return state == VoiceListeningState::completed ||
         state == VoiceListeningState::stopped ||
         state == VoiceListeningState::failed ||
         state == VoiceListeningState::abandoned;
}

struct EditWait {
  std::mutex mutex;
  std::condition_variable condition;
  bool done{};
  DeliveryResult result{DeliveryResult::permanent_failure};
};

[[nodiscard]] std::optional<std::string>
modal_field(const IncomingInteraction &interaction,
            const std::string_view name) {
  const auto found =
      std::ranges::find(interaction.modal_fields, name,
                        &std::pair<std::string, std::string>::first);
  if (found == interaction.modal_fields.end())
    return std::nullopt;
  return found->second;
}

[[nodiscard]] bool
exact_modal_fields(const IncomingInteraction &interaction,
                   const std::initializer_list<std::string_view> expected) {
  if (interaction.modal_fields.size() != expected.size())
    return false;
  for (const auto name : expected) {
    if (std::ranges::count(interaction.modal_fields, name,
                           &std::pair<std::string, std::string>::first) != 1)
      return false;
  }
  return true;
}

[[nodiscard]] std::string
bounded_transcript_excerpt(const std::string_view text) {
  auto excerpt = std::string{
      text.substr(0, std::min(text.size(), maximum_chronicle_body_size))};
  while (!excerpt.empty() && !valid_transcript_text(excerpt))
    excerpt.pop_back();
  return excerpt;
}

void scrub_string(std::string &value) noexcept {
  if (!value.empty())
    ::explicit_bzero(value.data(), value.size());
  value.clear();
  std::string{}.swap(value);
}

struct PendingTranscriptionResult {
  Transcript transcript;
  std::optional<TranscriptionError> failure;

  void scrub() noexcept {
    scrub_string(transcript.text);
    scrub_string(transcript.provider_request_id);
  }

  ~PendingTranscriptionResult() { scrub(); }
};

enum class OrderedEditSubmission { submitted, queued, rejected };

class OrderedPublicEditor final
    : public std::enable_shared_from_this<OrderedPublicEditor> {
public:
  explicit OrderedPublicEditor(DiscordPublicDelivery &delivery)
      : delivery_{delivery} {}

  [[nodiscard]] OrderedEditSubmission edit(PublicMessageEditRequest request,
                                           DeliveryCallback callback) {
    const auto key = request.message_id.value();
    bool dispatch{};
    {
      const std::scoped_lock lock{mutex_};
      if (closed_)
        return OrderedEditSubmission::rejected;
      auto &queue = queues_[key];
      queue.pending.push_back(
          {.request = std::move(request), .callback = std::move(callback)});
      if (!queue.in_flight) {
        queue.in_flight = true;
        dispatch = true;
      }
    }
    if (dispatch)
      dispatch_front(key);
    return dispatch ? OrderedEditSubmission::submitted
                    : OrderedEditSubmission::queued;
  }

  void close() noexcept {
    std::vector<DeliveryCallback> callbacks;
    {
      const std::scoped_lock lock{mutex_};
      if (closed_)
        return;
      closed_ = true;
      for (auto &[key, queue] : queues_) {
        static_cast<void>(key);
        for (auto &pending : queue.pending) {
          if (pending.callback)
            callbacks.push_back(std::move(pending.callback));
        }
      }
      queues_.clear();
    }
    for (auto &callback : callbacks) {
      try {
        callback(DeliveryResult::permanent_failure);
      } catch (...) {
      }
    }
  }

private:
  struct PendingEdit {
    PublicMessageEditRequest request;
    DeliveryCallback callback;
  };

  struct MessageQueue {
    std::deque<PendingEdit> pending;
    bool in_flight{};
  };

  void dispatch_front(const std::uint64_t key) noexcept {
    PublicMessageEditRequest request;
    {
      const std::scoped_lock lock{mutex_};
      const auto found = queues_.find(key);
      if (closed_ || found == queues_.end() || found->second.pending.empty())
        return;
      request = found->second.pending.front().request;
    }
    try {
      auto self = shared_from_this();
      delivery_.edit_public(
          request, [self = std::move(self), key](const DeliveryResult result) {
            self->complete(key, result);
          });
    } catch (...) {
      complete(key, DeliveryResult::unknown_outcome);
    }
  }

  void complete(const std::uint64_t key, const DeliveryResult result) noexcept {
    DeliveryCallback completed;
    std::vector<DeliveryCallback> cancelled;
    bool dispatch_next{};
    {
      const std::scoped_lock lock{mutex_};
      const auto found = queues_.find(key);
      if (found == queues_.end() || found->second.pending.empty())
        return;
      completed = std::move(found->second.pending.front().callback);
      found->second.pending.pop_front();
      if (closed_) {
        for (auto &pending : found->second.pending) {
          if (pending.callback)
            cancelled.push_back(std::move(pending.callback));
        }
        queues_.erase(found);
      } else if (found->second.pending.empty()) {
        queues_.erase(found);
      } else {
        dispatch_next = true;
      }
    }
    if (completed) {
      try {
        completed(result);
      } catch (...) {
      }
    }
    for (auto &callback : cancelled) {
      try {
        callback(DeliveryResult::permanent_failure);
      } catch (...) {
      }
    }
    if (dispatch_next)
      dispatch_front(key);
  }

  DiscordPublicDelivery &delivery_;
  std::mutex mutex_;
  std::unordered_map<std::uint64_t, MessageQueue> queues_;
  bool closed_{};
};

struct DeliveryWait {
  std::mutex mutex;
  std::condition_variable condition;
  bool done{};
  bool abandoned{};
  PublicDeliveryReceipt receipt;
  std::optional<InteractionMessage> late_success_repair;
  std::shared_ptr<OrderedPublicEditor> public_editor;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DeliveryCallback receipt_callback;
};

} // namespace

SecureAudioBuffer::SecureAudioBuffer(const std::size_t capacity,
                                     const bool require_memory_lock)
    : capacity_{capacity} {
  if (capacity == 0 || capacity > maximum_voice_pcm_bytes)
    throw std::invalid_argument{"Secure audio buffer capacity is invalid."};
  mapping_ = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr;
    throw std::runtime_error{"Unable to allocate secure audio memory."};
  }
  if (::madvise(mapping_, capacity_, MADV_DONTDUMP) != 0) {
    release();
    throw std::runtime_error{"Unable to exclude audio memory from dumps."};
  }
  if (::mlock(mapping_, capacity_) == 0) {
    locked_ = true;
  } else if (require_memory_lock) {
    release();
    throw std::runtime_error{"Unable to lock secure audio memory."};
  }
}

SecureAudioBuffer::~SecureAudioBuffer() { release(); }

SecureAudioBuffer::SecureAudioBuffer(SecureAudioBuffer &&other) noexcept
    : mapping_{std::exchange(other.mapping_, nullptr)},
      capacity_{std::exchange(other.capacity_, 0)},
      size_{std::exchange(other.size_, 0)},
      locked_{std::exchange(other.locked_, false)} {}

SecureAudioBuffer &
SecureAudioBuffer::operator=(SecureAudioBuffer &&other) noexcept {
  if (this != &other) {
    release();
    mapping_ = std::exchange(other.mapping_, nullptr);
    capacity_ = std::exchange(other.capacity_, 0);
    size_ = std::exchange(other.size_, 0);
    locked_ = std::exchange(other.locked_, false);
  }
  return *this;
}

bool SecureAudioBuffer::append(
    const std::span<const std::byte> audio) noexcept {
  if (mapping_ == nullptr || audio.empty() || size_ > capacity_ ||
      audio.size() > capacity_ - size_)
    return false;
  std::memcpy(static_cast<std::byte *>(mapping_) + size_, audio.data(),
              audio.size());
  size_ += audio.size();
  return true;
}

std::span<const std::byte> SecureAudioBuffer::bytes() const noexcept {
  return mapping_ == nullptr
             ? std::span<const std::byte>{}
             : std::span<const std::byte>{
                   static_cast<const std::byte *>(mapping_), size_};
}

std::size_t SecureAudioBuffer::size() const noexcept { return size_; }
std::size_t SecureAudioBuffer::capacity() const noexcept { return capacity_; }

void SecureAudioBuffer::scrub() noexcept {
  if (mapping_ != nullptr && capacity_ != 0)
    ::explicit_bzero(mapping_, capacity_);
  size_ = 0;
}

bool SecureAudioBuffer::all_zero_for_test() const noexcept {
  if (mapping_ == nullptr)
    return true;
  const auto *bytes = static_cast<const unsigned char *>(mapping_);
  unsigned char aggregate{};
  for (std::size_t index = 0; index < capacity_; ++index)
    aggregate = static_cast<unsigned char>(aggregate | bytes[index]);
  return aggregate == 0;
}

void SecureAudioBuffer::release() noexcept {
  if (mapping_ == nullptr)
    return;
  scrub();
  if (locked_)
    static_cast<void>(::munlock(mapping_, capacity_));
  static_cast<void>(::munmap(mapping_, capacity_));
  mapping_ = nullptr;
  capacity_ = 0;
  locked_ = false;
}

bool disable_process_core_dumps() noexcept {
  const rlimit limit{.rlim_cur = 0, .rlim_max = 0};
  return ::setrlimit(RLIMIT_CORE, &limit) == 0;
}

bool voice_listening_transition_allowed(const VoiceListeningState from,
                                        const VoiceListeningState to) noexcept {
  if (terminal(from))
    return false;
  if (to == VoiceListeningState::failed || to == VoiceListeningState::stopped ||
      to == VoiceListeningState::abandoned)
    return true;
  switch (from) {
  case VoiceListeningState::proposed:
    return to == VoiceListeningState::arming_transport;
  case VoiceListeningState::arming_transport:
    return to == VoiceListeningState::arming_indicator;
  case VoiceListeningState::arming_indicator:
    return to == VoiceListeningState::active;
  case VoiceListeningState::active:
    return to == VoiceListeningState::transcribing;
  case VoiceListeningState::transcribing:
    return to == VoiceListeningState::completed;
  case VoiceListeningState::completed:
  case VoiceListeningState::stopped:
  case VoiceListeningState::failed:
  case VoiceListeningState::abandoned:
    return false;
  }
  return false;
}

const char *
voice_listening_state_name(const VoiceListeningState state) noexcept {
  switch (state) {
  case VoiceListeningState::proposed:
    return "proposed";
  case VoiceListeningState::arming_transport:
    return "arming_transport";
  case VoiceListeningState::arming_indicator:
    return "arming_indicator";
  case VoiceListeningState::active:
    return "active";
  case VoiceListeningState::transcribing:
    return "transcribing";
  case VoiceListeningState::completed:
    return "completed";
  case VoiceListeningState::stopped:
    return "stopped";
  case VoiceListeningState::failed:
    return "failed";
  case VoiceListeningState::abandoned:
    return "abandoned";
  }
  return "failed";
}

const char *
voice_input_capability_name(const VoiceInputCapability capability) noexcept {
  switch (capability) {
  case VoiceInputCapability::disabled:
    return "disabled";
  case VoiceInputCapability::unsupported_build:
    return "unsupported_build";
  case VoiceInputCapability::unavailable_runtime:
    return "unavailable_runtime";
  case VoiceInputCapability::ready:
    return "ready";
  }
  return "unavailable_runtime";
}

std::optional<std::string> parse_voice_control(const std::string_view custom_id,
                                               const std::string_view prefix) {
  if (!custom_id.starts_with(prefix))
    return std::nullopt;
  const auto token = custom_id.substr(prefix.size());
  if (!valid_uuid_v4(std::string{token}))
    return std::nullopt;
  return std::string{token};
}

class VoiceListeningService::Impl {
public:
  enum class ProviderResultDisposition {
    pending,
    privacy_abort,
    finalizing,
    finalized,
  };

  struct Capture {
    const std::size_t requested_pcm_bytes;
    const std::string window_id;
    VoiceListeningWindow window;
    VoiceInputArmRequest arm;
    SecureAudioBuffer audio;
    ConfirmedCompletion initiator_completion;
    std::mutex audio_mutex;
    std::stop_source stop_source;
    std::atomic<bool> completion_sent{false};
    std::atomic<bool> privacy_overwrite_confirmed{false};
    std::atomic<bool> failure_submitted{false};
    std::atomic<bool> provider_sent{false};
    std::atomic<bool> usage_recorded{false};
    std::atomic<bool> speech_excluded{false};
    std::atomic<bool> accepting_audio{false};
    std::atomic<bool> transport_privacy_safe{true};
    std::atomic<bool> ended_status_confirmed{false};
    std::atomic<bool> capacity_expiry_submitted{false};
    std::atomic<bool> membership_validation_pending{false};
    std::atomic<bool> preemption_cleanup_scheduled{false};
    std::atomic<std::size_t> captured_bytes_before_scrub{};
    std::atomic<std::uint64_t> stop_actor{};
    std::atomic<bool> kill_stop{false};
    std::mutex provider_result_mutex;
    std::atomic<ProviderResultDisposition> provider_result_disposition{
        ProviderResultDisposition::pending};
    std::optional<DiscordSnowflake> current_public_message_id;

    Capture(VoiceListeningWindow value, VoiceInputArmRequest arm_request,
            SecureAudioBuffer buffer, ConfirmedCompletion completion)
        : requested_pcm_bytes{value.requested_seconds * 48'000U * 2U * 2U},
          window_id{value.window_id}, window{std::move(value)},
          arm{std::move(arm_request)}, audio{std::move(buffer)},
          initiator_completion{std::move(completion)} {}
  };

  enum class DraftState { active, modal_delivering, modal_opened, completed };
  struct TranscriptDraft {
    std::string window_id;
    DiscordSnowflake guild_id;
    DiscordSnowflake channel_id;
    DiscordSnowflake user_id;
    std::string text;
    std::chrono::steady_clock::time_point expires_at;
    DraftState state{DraftState::active};
  };

  Impl(VoiceListeningRepository &repository, VoiceInputAdapter &adapter,
       TranscriptionClient *transcription,
       DiscordPublicDelivery &public_delivery, const Clock &clock,
       PersistentIdGenerator &ids, Diagnostics &diagnostics,
       ServerScopeConfiguration scope,
       VoiceListeningConfiguration configuration,
       VoxContextProvider vox_context, ChronicleService *chronicle,
       SpeechExclusion speech_exclusion)
      : repository_{repository}, adapter_{adapter},
        transcription_{transcription}, public_delivery_{public_delivery},
        public_editor_{std::make_shared<OrderedPublicEditor>(public_delivery)},
        clock_{clock}, ids_{ids}, diagnostics_{diagnostics},
        scope_{std::move(scope)}, configuration_{std::move(configuration)},
        vox_context_{std::move(vox_context)}, chronicle_{chronicle},
        speech_exclusion_{std::move(speech_exclusion)},
        control_worker_{configuration_.queue_capacity, 2},
        privacy_worker_{1, 1}, callback_worker_{2, 1},
        callback_cleanup_worker_{1, 1},
        transcription_worker_{configuration_.queue_capacity, 1} {
    if (!vox_context_ || configuration_.queue_capacity == 0 ||
        configuration_.transcript_draft_lifetime <=
            std::chrono::milliseconds::zero() ||
        configuration_.transcript_draft_lifetime > std::chrono::minutes{5} ||
        configuration_.public_status_timeout <=
            std::chrono::milliseconds::zero() ||
        configuration_.public_status_timeout > std::chrono::seconds{30})
      throw std::invalid_argument{"Voice listening configuration is invalid."};
  }

  ~Impl() {
    stop();
    late_delivery_callbacks_->close_and_wait();
    public_editor_->close();
  }

  void start() {
    if (started_.exchange(true))
      throw std::logic_error{"Voice listening service may start only once."};
    kill_switch_latched_.store(repository_.kill_switch_enabled());
    static_cast<void>(repository_.abandon_nonterminal(
        unix_milliseconds(clock_), "restart", "voice:startup:"));
    repository_.record_consent_attestation(
        configuration_.guild_consent_attested, scope_.owner_user_id,
        ids_.next_id(), unix_milliseconds(clock_));
    if (configuration_.enabled && !disable_process_core_dumps())
      throw std::runtime_error{"Unable to disable core dumps for voice input."};
    control_worker_.start();
    privacy_worker_.start();
    callback_worker_.start();
    callback_cleanup_worker_.start();
    transcription_worker_.start();
    preemption_worker_ = std::jthread{
        [this](const std::stop_token token) { drain_preemptions(token); }};
    adapter_.start(
        [this](const std::string_view session_id,
               const std::uint64_t generation,
               const std::span<const std::byte> audio) {
          receive_audio(session_id, generation, audio);
        },
        [this](VoiceInputEvent event) { receive_event(std::move(event)); });
    draft_reaper_ = std::jthread{
        [this](const std::stop_token token) { reap_drafts(token); }};
  }

  void stop() noexcept {
    if (stopped_.exchange(true))
      return;
    // A disable preempts capture before Discord acknowledges the interaction.
    // Persist that latch even when shutdown prevents the deferred interaction
    // callback from reaching InteractionHandler.
    persist_pending_disable();
    preemption_worker_.request_stop();
    preemption_condition_.notify_all();
    if (preemption_worker_.joinable()) {
      try {
        preemption_worker_.join();
      } catch (...) {
      }
    }
    draft_reaper_.request_stop();
    draft_condition_.notify_all();
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
      if (capture) {
        static_cast<void>(claim_privacy_abort(capture));
        capture->stop_source.request_stop();
        capture->accepting_audio.store(false);
      }
    }
    if (capture) {
      adapter_.disarm();
      scrub_capture(capture, true);
    }
    stop_timer();
    control_worker_.stop();
    callback_cleanup_worker_.stop();
    {
      std::unique_lock operation_lock{operation_mutex_};
      {
        const std::scoped_lock lock{state_mutex_};
        capture = active_;
        if (capture) {
          static_cast<void>(claim_privacy_abort(capture));
          capture->stop_source.request_stop();
          capture->accepting_audio.store(false);
        }
      }
      adapter_.disarm();
      if (capture) {
        scrub_capture(capture, false);
        static_cast<void>(confirm_transport_privacy(capture));
      } else {
        static_cast<void>(adapter_.disable_transport());
      }
      if (capture) {
        try {
          if (!terminal(capture->window.state))
            static_cast<void>(
                advance(capture, VoiceListeningState::abandoned, "shutdown"));
        } catch (...) {
        }
        const auto provider_sent = capture->provider_sent.load();
        record_terminal_usage(capture, "shutdown", provider_sent);
        publish_ended(
            capture,
            ended_message("Listening ended during application shutdown. "
                          "Buffered audio was discarded."));
        complete_initiator_privacy_abort(
            capture,
            text_message("Listening ended during application shutdown; no "
                         "transcript was produced."),
            true);
        clear_active(capture);
      }
    }
    adapter_.shutdown();
    transcription_worker_.stop();
    callback_worker_.stop();
    privacy_worker_.stop();
    if (draft_reaper_.joinable()) {
      try {
        draft_reaper_.join();
      } catch (...) {
      }
    }
    scrub_all_drafts();
  }

  SubmitResult listen_start(IncomingInteraction interaction,
                            const std::size_t duration_seconds,
                            ConfirmedCompletion completion) {
    auto cancellation = completion;
    return control_worker_.try_submit(
        [this, interaction = std::move(interaction), duration_seconds,
         completion =
             std::move(completion)](const std::stop_token worker_stop) mutable {
          begin(std::move(interaction), duration_seconds, std::move(completion),
                worker_stop);
        },
        [completion = std::move(cancellation)]() mutable {
          if (completion)
            completion(
                text_message(
                    "Voice listening ended because the application is shutting "
                    "down."),
                {});
        });
  }

  SubmitResult listen_stop(IncomingInteraction interaction,
                           Completion completion,
                           std::optional<std::string> window_id) {
    bool matching{};
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
      matching = capture &&
                 (!window_id || *window_id == capture->window.window_id) &&
                 claim_privacy_abort(capture);
      if (matching) {
        capture->stop_source.request_stop();
        capture->accepting_audio.store(false);
        capture->stop_actor.store(interaction.user_id.value());
      }
    }
    if (matching) {
      adapter_.disarm();
      scrub_capture(capture, true);
    }
    auto abort = [this, interaction = std::move(interaction),
                  completion = std::move(completion),
                  window_id = std::move(window_id)]() mutable {
      abort_active(std::move(interaction), std::move(completion),
                   std::move(window_id), "user_stop");
    };
    const auto submitted = privacy_worker_.try_submit(
        [abort](std::stop_token) mutable { abort(); },
        [abort]() mutable { abort(); });
    if (submitted != SubmitResult::accepted) {
      abort();
      return SubmitResult::accepted;
    }
    return submitted;
  }

  [[nodiscard]] std::uint64_t
  preempt_privacy_abort(const DiscordSnowflake actor_user_id,
                        const bool latch_kill_switch,
                        const std::optional<std::string> &window_id) noexcept {
    std::uint64_t observed_generation{};
    {
      const std::scoped_lock disable_lock{disable_fence_mutex_};
      if (latch_kill_switch) {
        ++disable_generation_;
        pending_disable_actor_ = actor_user_id;
      }
      observed_generation = disable_generation_;
    }
    if (latch_kill_switch)
      kill_switch_latched_.store(true);
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
      if (!capture || (window_id && *window_id != capture->window.window_id) ||
          !claim_privacy_abort(capture)) {
        capture.reset();
      } else {
        capture->stop_source.request_stop();
        capture->accepting_audio.store(false);
        capture->stop_actor.store(actor_user_id.value());
        if (latch_kill_switch)
          capture->kill_stop.store(true);
      }
    }
    if (capture) {
      adapter_.disarm();
      scrub_capture(capture, true);
    }
    schedule_preemption(capture, latch_kill_switch);
    return observed_generation;
  }

  SubmitResult set_kill_switch(
      IncomingInteraction interaction, const bool disabled,
      Completion completion,
      const std::optional<std::uint64_t> observed_disable_generation) {
    std::uint64_t command_generation{};
    if (disabled && !observed_disable_generation) {
      command_generation =
          preempt_privacy_abort(interaction.user_id, true, std::nullopt);
    } else {
      command_generation =
          observed_disable_generation.value_or(current_disable_generation());
    }
    if (disabled)
      kill_switch_latched_.store(true);
    std::shared_ptr<Capture> capture;
    if (disabled) {
      {
        const std::scoped_lock state_lock{state_mutex_};
        capture = active_;
        if (capture && claim_privacy_abort(capture)) {
          capture->stop_source.request_stop();
          capture->accepting_audio.store(false);
          capture->stop_actor.store(interaction.user_id.value());
          capture->kill_stop.store(true);
        } else
          capture.reset();
      }
      if (capture) {
        adapter_.disarm();
        scrub_capture(capture, true);
      }
    }
    {
      std::unique_lock operation_lock{operation_mutex_};
      if (stopped_.load()) {
        completion(text_message("Voice listening is shutting down."));
        return SubmitResult::accepted;
      }
      if (disabled && disable_persisted(command_generation)) {
        completion(text_message("Voice listening is disabled immediately."));
        return SubmitResult::accepted;
      }
      if (!disabled && command_generation != current_disable_generation()) {
        completion(text_message(
            "A newer voice-listening disable superseded this enable request; "
            "the kill switch remains set."));
        return SubmitResult::accepted;
      }
      if (disabled) {
        const std::scoped_lock state_lock{state_mutex_};
        const auto current = active_;
        if (current && claim_privacy_abort(current)) {
          capture = current;
          capture->stop_source.request_stop();
          capture->accepting_audio.store(false);
          capture->stop_actor.store(interaction.user_id.value());
          capture->kill_stop.store(true);
        } else
          capture.reset();
      }
      if (capture) {
        adapter_.disarm();
        scrub_capture(capture, false);
        static_cast<void>(confirm_transport_privacy(capture));
      } else if (disabled && transport_privacy_blocked_.load()) {
        const auto privacy_safe = adapter_.disable_transport();
        transport_privacy_blocked_.store(!privacy_safe);
        if (privacy_safe && !public_status_blocked_.load() &&
            speech_exclusion_) {
          try {
            speech_exclusion_(false);
          } catch (...) {
          }
        }
      }
      try {
        repository_.set_kill_switch(disabled, interaction.user_id,
                                    ids_.next_id(), unix_milliseconds(clock_));
        if (!disabled && command_generation != current_disable_generation()) {
          std::uint64_t repair_generation{};
          DiscordSnowflake repair_actor;
          {
            const std::scoped_lock disable_lock{disable_fence_mutex_};
            repair_generation = disable_generation_;
            repair_actor = pending_disable_actor_;
          }
          if (!repair_actor.is_set())
            throw std::runtime_error{
                "A superseding voice disable has no audit actor."};
          repository_.set_kill_switch(true, repair_actor, ids_.next_id(),
                                      unix_milliseconds(clock_));
          kill_switch_latched_.store(true);
          mark_disable_persisted(repair_generation);
        }
      } catch (...) {
        operation_lock.unlock();
        if (disabled && capture)
          abort_active(interaction, {}, std::nullopt, "kill_switch");
        completion(text_message(
            "Voice listening was disarmed, but its durable kill-switch audit "
            "could not be updated."));
        return SubmitResult::accepted;
      }
      if (disabled) {
        mark_disable_persisted(command_generation);
      } else if (command_generation == current_disable_generation()) {
        kill_switch_latched_.store(false);
      } else {
        completion(text_message(
            "A newer voice-listening disable superseded this enable request; "
            "the kill switch remains set."));
        return SubmitResult::accepted;
      }
    }
    if (disabled && capture)
      abort_active(interaction, {}, std::nullopt, "kill_switch");
    completion(text_message(
        disabled ? "Voice listening is disabled immediately."
                 : "Voice listening kill switch is cleared; configured "
                   "privacy gates still apply."));
    return SubmitResult::accepted;
  }

  [[nodiscard]] std::uint64_t current_disable_generation() const noexcept {
    const std::scoped_lock lock{disable_fence_mutex_};
    return disable_generation_;
  }

  void mark_disable_persisted(const std::uint64_t generation) noexcept {
    const std::scoped_lock lock{disable_fence_mutex_};
    persisted_disable_generation_ =
        std::max(persisted_disable_generation_, generation);
  }

  [[nodiscard]] bool
  disable_persisted(const std::uint64_t generation) const noexcept {
    const std::scoped_lock lock{disable_fence_mutex_};
    return persisted_disable_generation_ >= generation;
  }

  void persist_pending_disable() noexcept {
    {
      const std::scoped_lock lock{disable_fence_mutex_};
      if (persisted_disable_generation_ >= disable_generation_)
        return;
    }
    try {
      const std::scoped_lock operation_lock{operation_mutex_};
      std::uint64_t generation{};
      DiscordSnowflake actor;
      {
        const std::scoped_lock lock{disable_fence_mutex_};
        if (persisted_disable_generation_ >= disable_generation_)
          return;
        generation = disable_generation_;
        actor = pending_disable_actor_;
      }
      if (!actor.is_set())
        return;
      repository_.set_kill_switch(true, actor, ids_.next_id(),
                                  unix_milliseconds(clock_));
      kill_switch_latched_.store(true);
      mark_disable_persisted(generation);
    } catch (...) {
      diagnostics_.emit(
          {DiagnosticSeverity::error,
           "voice.kill_switch_persist",
           "A preempted voice-listening disable could not be persisted.",
           {}});
    }
  }

  void schedule_preemption(const std::shared_ptr<Capture> &capture,
                           const bool persist_disable) noexcept {
    bool changed = persist_disable;
    std::shared_ptr<Capture> scheduled_capture;
    if (capture && !capture->preemption_cleanup_scheduled.exchange(true)) {
      scheduled_capture = capture;
      changed = true;
    }
    if (!changed)
      return;
    {
      const std::scoped_lock lock{preemption_mutex_};
      preemption_persist_disable_ =
          preemption_persist_disable_ || persist_disable;
      if (scheduled_capture)
        preemption_capture_ = std::move(scheduled_capture);
    }
    preemption_condition_.notify_one();
  }

  void drain_preemptions(const std::stop_token token) noexcept {
    for (;;) {
      bool persist_disable{};
      std::shared_ptr<Capture> capture;
      {
        std::unique_lock lock{preemption_mutex_};
        static_cast<void>(preemption_condition_.wait(lock, token, [this] {
          return preemption_persist_disable_ || preemption_capture_;
        }));
        if (!preemption_persist_disable_ && !preemption_capture_) {
          if (token.stop_requested())
            return;
          continue;
        }
        persist_disable = preemption_persist_disable_;
        preemption_persist_disable_ = false;
        capture = std::move(preemption_capture_);
      }
      if (persist_disable)
        persist_pending_disable();
      if (capture) {
        try {
          const std::scoped_lock operation_lock{operation_mutex_};
          privacy_abort_requested(capture);
        } catch (...) {
          fail(capture, "privacy_abort_failed",
               "Listening was disarmed after its stop audit failed.");
        }
      }
    }
  }

  std::optional<ModalPayload>
  transcript_modal(const IncomingInteraction &interaction) {
    const auto token = parse_voice_control(interaction.custom_id,
                                           voice_transcript_component_prefix);
    if (!token)
      return std::nullopt;
    std::scoped_lock lock{draft_mutex_};
    purge_drafts_locked(std::chrono::steady_clock::now());
    const auto found = drafts_.find(*token);
    if (found == drafts_.end() || found->second.state != DraftState::active ||
        found->second.guild_id != interaction.guild_id ||
        found->second.channel_id != interaction.channel_id ||
        found->second.user_id != interaction.user_id)
      return std::nullopt;
    found->second.state = DraftState::modal_delivering;
    return ModalPayload{
        .custom_id = std::string{voice_transcript_modal_prefix} + *token,
        .title = "Propose transcript excerpt",
        .fields = {{.custom_id = "title",
                    .label = "Chronicle title",
                    .value = "A voice upon the Vox",
                    .minimum_length = 1,
                    .maximum_length = maximum_chronicle_title_size,
                    .required = true,
                    .style = ModalFieldPayload::Style::short_text},
                   {.custom_id = "body",
                    .label = "Excerpt (edit before proposing)",
                    .value = bounded_transcript_excerpt(found->second.text),
                    .minimum_length = 1,
                    .maximum_length = maximum_chronicle_body_size,
                    .required = true,
                    .style = ModalFieldPayload::Style::paragraph}}};
  }

  void transcript_modal_delivery(const IncomingInteraction &interaction,
                                 const DeliveryResult result) noexcept {
    try {
      const auto token = parse_voice_control(interaction.custom_id,
                                             voice_transcript_component_prefix);
      if (!token)
        return;
      const std::scoped_lock lock{draft_mutex_};
      purge_drafts_locked(std::chrono::steady_clock::now());
      const auto found = drafts_.find(*token);
      if (found == drafts_.end() ||
          found->second.state != DraftState::modal_delivering ||
          found->second.guild_id != interaction.guild_id ||
          found->second.channel_id != interaction.channel_id ||
          found->second.user_id != interaction.user_id)
        return;
      found->second.state = result == DeliveryResult::success
                                ? DraftState::modal_opened
                                : DraftState::active;
    } catch (...) {
    }
  }

  InteractionMessage
  propose_transcript(const IncomingInteraction &interaction) {
    const auto token = parse_voice_control(interaction.custom_id,
                                           voice_transcript_modal_prefix);
    if (!token || !exact_modal_fields(interaction, {"title", "body"}))
      return text_message("That transcript proposal is invalid or expired.");
    const auto title = modal_field(interaction, "title");
    const auto body = modal_field(interaction, "body");
    if (!title || !body ||
        !valid_chronicle_text(*title, maximum_chronicle_title_size) ||
        !valid_chronicle_text(*body, maximum_chronicle_body_size))
      return text_message("That Chronicle excerpt is invalid or too long.");

    std::string window_id;
    {
      const std::scoped_lock lock{draft_mutex_};
      purge_drafts_locked(std::chrono::steady_clock::now());
      const auto found = drafts_.find(*token);
      if (found == drafts_.end() || found->second.state == DraftState::active ||
          found->second.guild_id != interaction.guild_id ||
          found->second.channel_id != interaction.channel_id ||
          found->second.user_id != interaction.user_id)
        return text_message("That transcript proposal is invalid or expired.");
      if (found->second.state == DraftState::completed)
        return text_message("That Chronicle proposal was already submitted.");
      window_id = found->second.window_id;
    }
    if (!chronicle_)
      return text_message("The Chronicle is currently unavailable.");
    try {
      auto result = chronicle_->propose_voice_transcript(interaction, window_id,
                                                         *title, *body);
      auto rendered = render_chronicle_proposal(result);
      const std::scoped_lock lock{draft_mutex_};
      const auto found = drafts_.find(*token);
      if (found != drafts_.end()) {
        scrub_string(found->second.text);
        found->second.state = DraftState::completed;
      }
      return rendered;
    } catch (...) {
      const std::scoped_lock lock{draft_mutex_};
      const auto found = drafts_.find(*token);
      if (found != drafts_.end() &&
          (found->second.state == DraftState::modal_delivering ||
           found->second.state == DraftState::modal_opened))
        found->second.state = DraftState::active;
      throw;
    }
  }

  VoiceListeningHealth health() const {
    std::optional<VoiceListeningState> state;
    std::size_t volatile_transcript_drafts{};
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_)
        state = active_->window.state;
    }
    {
      const std::scoped_lock lock{draft_mutex_};
      volatile_transcript_drafts = drafts_.size();
    }
    return {.capability = configured_capability(),
            .configured_enabled = configuration_.enabled,
            .consent_attested = configuration_.guild_consent_attested,
            .provider_enabled = configuration_.provider_enabled,
            .state = state,
            .repository = repository_.health(unix_milliseconds(clock_)),
            .control_queue = control_worker_.snapshot(),
            .privacy_queue = privacy_worker_.snapshot(),
            .transcription_queue = transcription_worker_.snapshot(),
            .callback_drops = callback_drops_.load(),
            .volatile_transcript_drafts = volatile_transcript_drafts,
            .last_failure_category = last_failure()};
  }

private:
  [[nodiscard]] static bool
  claim_privacy_abort(const std::shared_ptr<Capture> &capture) noexcept {
    const std::scoped_lock lock{capture->provider_result_mutex};
    const auto disposition = capture->provider_result_disposition.load();
    if (disposition == ProviderResultDisposition::privacy_abort)
      return true;
    if (disposition != ProviderResultDisposition::pending &&
        disposition != ProviderResultDisposition::finalizing)
      return false;
    capture->provider_result_disposition.store(
        ProviderResultDisposition::privacy_abort);
    return true;
  }

  [[nodiscard]] bool
  claim_provider_result(const std::shared_ptr<Capture> &capture) noexcept {
    const std::scoped_lock lock{capture->provider_result_mutex};
    if (capture->provider_result_disposition.load() !=
        ProviderResultDisposition::pending)
      return false;
    if (kill_switch_latched_.load() || stopped_.load() ||
        capture->stop_source.stop_requested()) {
      capture->provider_result_disposition.store(
          ProviderResultDisposition::privacy_abort);
      return false;
    }
    capture->provider_result_disposition.store(
        ProviderResultDisposition::finalizing);
    return true;
  }

  [[nodiscard]] bool
  finalize_provider_result(const std::shared_ptr<Capture> &capture) noexcept {
    const std::scoped_lock lock{capture->provider_result_mutex};
    if (capture->provider_result_disposition.load() !=
        ProviderResultDisposition::finalizing)
      return false;
    if (kill_switch_latched_.load() || stopped_.load() ||
        capture->stop_source.stop_requested()) {
      capture->provider_result_disposition.store(
          ProviderResultDisposition::privacy_abort);
      return false;
    }
    capture->provider_result_disposition.store(
        ProviderResultDisposition::finalized);
    return true;
  }

  [[nodiscard]] VoiceInputCapability configured_capability() const noexcept {
    if (!configuration_.enabled)
      return VoiceInputCapability::disabled;
    if (kill_switch_latched_.load() || transport_privacy_blocked_.load() ||
        public_status_blocked_.load())
      return VoiceInputCapability::unavailable_runtime;
    const auto adapter = adapter_.capability();
    if (adapter != VoiceInputCapability::ready)
      return adapter;
    if (!configuration_.guild_consent_attested ||
        !configuration_.provider_enabled || transcription_ == nullptr ||
        configuration_.model != transcription_model)
      return VoiceInputCapability::unavailable_runtime;
    return VoiceInputCapability::ready;
  }

  [[nodiscard]] std::optional<DiscordSnowflake> send_public(
      const InteractionMessage &message, const std::string &nonce,
      std::optional<InteractionMessage> late_success_repair = std::nullopt,
      DeliveryCallback receipt_callback = {}) {
    auto wait = std::make_shared<DeliveryWait>();
    wait->late_success_repair = std::move(late_success_repair);
    wait->public_editor = public_editor_;
    wait->guild_id = scope_.guild_id;
    wait->channel_id = scope_.primary_channel_id;
    wait->receipt_callback = std::move(receipt_callback);
    public_delivery_.send_public(
        {.guild_id = scope_.guild_id,
         .channel_id = scope_.primary_channel_id,
         .message = message},
        discord_nonce_from_uuid(nonce), [wait](PublicDeliveryReceipt receipt) {
          try {
            const auto result = receipt.result;
            std::optional<PublicMessageEditRequest> repair;
            {
              const std::scoped_lock lock{wait->mutex};
              if (wait->abandoned) {
                if (receipt.result == DeliveryResult::success &&
                    receipt.provider_message_id && wait->late_success_repair) {
                  repair.emplace(PublicMessageEditRequest{
                      .guild_id = wait->guild_id,
                      .channel_id = wait->channel_id,
                      .message_id = *receipt.provider_message_id,
                      .message = std::move(*wait->late_success_repair)});
                  wait->late_success_repair.reset();
                }
              } else {
                wait->receipt = std::move(receipt);
                wait->done = true;
                wait->condition.notify_all();
              }
            }
            if (repair && wait->public_editor)
              static_cast<void>(
                  wait->public_editor->edit(std::move(*repair), {}));
            if (wait->receipt_callback)
              wait->receipt_callback(result);
          } catch (...) {
          }
        });
    std::unique_lock lock{wait->mutex};
    if (!wait->condition.wait_for(lock, configuration_.public_status_timeout,
                                  [&wait] { return wait->done; })) {
      wait->abandoned = true;
      return std::nullopt;
    }
    if (wait->receipt.result != DeliveryResult::success ||
        !wait->receipt.provider_message_id)
      return std::nullopt;
    return wait->receipt.provider_message_id;
  }

  [[nodiscard]] bool
  edit_public(const DiscordSnowflake message_id,
              const InteractionMessage &message,
              const std::stop_token stop_token = std::stop_token{},
              DeliveryCallback receipt_callback = {}) {
    auto wait = std::make_shared<EditWait>();
    const auto submission =
        public_editor_->edit({.guild_id = scope_.guild_id,
                              .channel_id = scope_.primary_channel_id,
                              .message_id = message_id,
                              .message = message},
                             [wait, receipt_callback =
                                        std::move(receipt_callback)](
                                 const DeliveryResult result) {
                               {
                                 const std::scoped_lock lock{wait->mutex};
                                 wait->result = result;
                                 wait->done = true;
                                 wait->condition.notify_all();
                               }
                               if (receipt_callback) {
                                 try {
                                   receipt_callback(result);
                                 } catch (...) {
                                 }
                               }
                             });
    // A queued terminal edit will still repair the original card after the
    // earlier request completes. Report it as unconfirmed so publish_ended()
    // immediately emits a replacement ended card in the meantime.
    if (submission != OrderedEditSubmission::submitted)
      return false;
    const std::stop_callback stop_wait{
        stop_token, [wait] { wait->condition.notify_all(); }};
    std::unique_lock lock{wait->mutex};
    return wait->condition.wait_for(lock, configuration_.public_status_timeout,
                                    [&wait, stop_token] {
                                      return wait->done ||
                                             stop_token.stop_requested();
                                    }) &&
           wait->done && wait->result == DeliveryResult::success;
  }

  [[nodiscard]] bool
  confirm_transport_privacy(const std::shared_ptr<Capture> &capture) noexcept {
    if (!capture)
      return true;
    const auto privacy_safe = adapter_.disable_transport();
    capture->transport_privacy_safe.store(privacy_safe);
    transport_privacy_blocked_.store(!privacy_safe);
    if (privacy_safe)
      return true;
    {
      const std::scoped_lock lock{failure_mutex_};
      last_failure_category_ = "transport_restore_failed";
    }
    try {
      diagnostics_.emit(
          {DiagnosticSeverity::error, "voice_input.transport_restore",
           "Voice input could not confirm self-deaf transport or disconnect; "
           "voice speech remains excluded.",
           std::nullopt});
    } catch (...) {
    }
    return false;
  }

  void confirm_ended_status(const std::shared_ptr<Capture> &capture) noexcept {
    if (!capture)
      return;
    capture->ended_status_confirmed.store(true);
    {
      const std::scoped_lock lock{public_status_mutex_};
      if (const auto blocked = public_status_blocked_capture_.lock();
          blocked == capture) {
        public_status_blocked_capture_.reset();
        public_status_blocked_.store(false);
      }
    }
    release_speech_exclusion(capture);
  }

  [[nodiscard]] DeliveryCallback
  ended_status_receipt(const std::shared_ptr<Capture> &capture) {
    const auto callbacks = late_delivery_callbacks_;
    return [this, callbacks, capture](const DeliveryResult result) noexcept {
      if (result != DeliveryResult::success)
        return;
      try {
        static_cast<void>(callbacks->invoke(
            [this, capture] { confirm_ended_status(capture); }));
      } catch (...) {
      }
    };
  }

  [[nodiscard]] bool
  latch_unconfirmed_ended_status(const std::shared_ptr<Capture> &capture) {
    const std::scoped_lock lock{public_status_mutex_};
    if (capture->ended_status_confirmed.load())
      return false;
    public_status_blocked_capture_ = capture;
    public_status_blocked_.store(true);
    return true;
  }

  bool publish_ended(const std::shared_ptr<Capture> &capture,
                     InteractionMessage message) noexcept {
    if (!capture || !capture->window.public_message_id)
      return true;
    if (!capture->transport_privacy_safe.load() && message.embed) {
      message.embed->description +=
          " Voice transport safety could not be confirmed; bot speech remains "
          "blocked pending operator recovery.";
    }
    const auto message_id = capture->current_public_message_id.value_or(
        *capture->window.public_message_id);
    const auto receipt = ended_status_receipt(capture);
    try {
      if (edit_public(message_id, message, {}, receipt)) {
        confirm_ended_status(capture);
        return true;
      }
    } catch (...) {
    }
    try {
      const auto replacement =
          send_public(message, ids_.next_id(), std::nullopt, receipt);
      if (replacement) {
        capture->current_public_message_id = *replacement;
        confirm_ended_status(capture);
        return true;
      }
    } catch (...) {
    }
    const auto status_blocked = latch_unconfirmed_ended_status(capture);
    if (!status_blocked) {
      confirm_ended_status(capture);
      return true;
    }
    {
      const std::scoped_lock lock{failure_mutex_};
      last_failure_category_ = "public_status_recovery";
    }
    try {
      diagnostics_.emit(
          {DiagnosticSeverity::error,
           "voice_input.public_status_recovery",
           "The listening status edit and replacement ended-status delivery "
           "both failed.",
           std::nullopt});
    } catch (...) {
    }
    return false;
  }

  [[nodiscard]] InteractionMessage
  arming_message(const Capture &capture) const {
    return {
        .content = {},
        .embed =
            EmbedPayload{
                .color = 0xb42318,
                .title = "ARMING — no audio is being captured",
                .description =
                    "A short channel-mix listening window was requested. "
                    "Capture will begin only after this card says LISTENING. "
                    "Guild-wide prior consent is attested."},
        .buttons = {{.custom_id = std::string{voice_listening_stop_prefix} +
                                  capture.window.window_id,
                     .label = "Stop listening",
                     .disabled = false,
                     .style = ButtonStyle::secondary}},
        .allowed_user_mentions = {}};
  }

  [[nodiscard]] InteractionMessage
  listening_message(const Capture &capture, const std::int64_t now_ms) const {
    std::ostringstream description;
    description << "Requested by <@" << capture.window.requester_user_id.str()
                << ">. Channel-mix audio is being captured for at most "
                << capture.window.requested_seconds
                << " seconds and will be sent to OpenAI `gpt-transcribe`. "
                   "Raw audio is never persisted. Guild-wide prior consent "
                   "is attested for present and joining humans. Anyone may "
                   "stop this window. Started at "
                << now_ms << " ms UTC; expires no later than "
                << now_ms + static_cast<std::int64_t>(
                                capture.window.requested_seconds * 1'000U)
                << " ms UTC.";
    return {.content = {},
            .embed = EmbedPayload{.color = 0xd92d20,
                                  .title = "LISTENING — voice input active",
                                  .description = description.str()},
            .buttons = {{.custom_id = std::string{voice_listening_stop_prefix} +
                                      capture.window.window_id,
                         .label = "Stop listening",
                         .disabled = false,
                         .style = ButtonStyle::secondary}},
            .allowed_user_mentions = {}};
  }

  [[nodiscard]] InteractionMessage ended_message(std::string reason) const {
    return {.content = {},
            .embed = EmbedPayload{.color = 0x667085,
                                  .title = "Listening ended",
                                  .description = std::move(reason)},
            .buttons = {},
            .allowed_user_mentions = {}};
  }

  void begin(IncomingInteraction interaction,
             const std::size_t duration_seconds, ConfirmedCompletion completion,
             const std::stop_token worker_stop) noexcept {
    auto fallback = completion;
    try {
      begin_impl(std::move(interaction), duration_seconds,
                 std::move(completion), worker_stop);
    } catch (...) {
      std::shared_ptr<Capture> capture;
      {
        const std::scoped_lock lock{state_mutex_};
        capture = active_;
      }
      if (capture) {
        fail(capture, "internal_failure",
             "Listening stopped because its privacy lifecycle failed.");
      } else if (fallback) {
        try {
          fallback(text_message(
                       "Voice listening failed safely before capture began."),
                   {});
        } catch (...) {
        }
      }
    }
  }

  void begin_impl(IncomingInteraction interaction,
                  const std::size_t duration_seconds,
                  ConfirmedCompletion completion,
                  const std::stop_token worker_stop) {
    const std::scoped_lock operation_lock{operation_mutex_};
    if (stopped_.load() || worker_stop.stop_requested()) {
      completion(text_message("Voice listening is shutting down."), {});
      return;
    }
    if (duration_seconds != 5 && duration_seconds != 10 &&
        duration_seconds != 15) {
      completion(
          text_message("Choose a listening duration of 5, 10, or 15 seconds."),
          {});
      return;
    }
    if (configured_capability() != VoiceInputCapability::ready) {
      completion(text_message(
                     configuration_.enabled
                         ? "Voice listening is unavailable because a "
                           "capability, consent, "
                           "provider, or kill-switch prerequisite is not ready."
                         : "Voice listening is disabled by configuration."),
                 {});
      return;
    }
    if (kill_switch_latched_.load() || repository_.kill_switch_enabled()) {
      completion(
          text_message("Voice listening is disabled by the owner kill switch."),
          {});
      return;
    }
    const auto vox = vox_context_();
    if (!vox || !vox->ready || vox->guild_id != interaction.guild_id ||
        vox->text_channel_id != interaction.channel_id) {
      completion(text_message("Start an active, ready Vox session first."), {});
      return;
    }
    if (!vox->speech_idle) {
      completion(
          text_message("Wait for queued or playing Vox speech to finish."), {});
      return;
    }
    VoiceInputArmRequest arm{.session_id = vox->session_id,
                             .guild_id = vox->guild_id,
                             .voice_channel_id = vox->voice_channel_id,
                             .requester_user_id = interaction.user_id,
                             .generation = vox->connection_generation};
    const auto presence = adapter_.preflight(arm);
    if (!presence.available || !presence.requester_present ||
        presence.other_bot_present || presence.human_count == 0) {
      completion(text_message("Voice listening could not verify the requester "
                              "and channel membership."),
                 {});
      return;
    }
    if (stopped_.load() || worker_stop.stop_requested()) {
      completion(text_message("Voice listening is shutting down."), {});
      return;
    }
    SecureAudioBuffer buffer{maximum_voice_pcm_bytes,
                             configuration_.require_memory_lock};
    const auto now_ms = unix_milliseconds(clock_);
    VoiceListeningWindow proposed{
        .window_id = ids_.next_id(),
        .vox_session_id = vox->session_id,
        .guild_id = interaction.guild_id,
        .text_channel_id = interaction.channel_id,
        .voice_channel_id = vox->voice_channel_id,
        .requester_user_id = interaction.user_id,
        .state = VoiceListeningState::proposed,
        .revision = 1,
        .connection_generation = vox->connection_generation,
        .requested_seconds = duration_seconds,
        .initial_human_count = presence.human_count,
        .reserved_micro_usd = estimated_transcription_cost_micro_usd(
            static_cast<std::int64_t>(duration_seconds)),
        .provider_attempt_started = false,
        .provider_attempt_started_at_ms = std::nullopt,
        .created_at_ms = now_ms,
        .active_at_ms = std::nullopt,
        .ended_at_ms = std::nullopt,
        .public_message_id = std::nullopt,
        .terminal_reason = std::nullopt};
    const auto begin_result = repository_.begin(
        {.window = proposed,
         .interaction_idempotency_key =
             "voice:listen-start:" + interaction.interaction_id.str(),
         .request_fingerprint = std::to_string(duration_seconds),
         .transition_id = ids_.next_id()},
        configuration_.usage_policy);
    if (begin_result.code != VoiceWindowBeginCode::created ||
        !begin_result.window) {
      completion(text_message(begin_failure(begin_result.code)), {});
      return;
    }
    auto capture = std::make_shared<Capture>(
        *begin_result.window, arm, std::move(buffer), std::move(completion));
    {
      const std::scoped_lock lock{state_mutex_};
      active_ = capture;
    }
    if (handle_arming_cancellation(capture, worker_stop))
      return;
    if (kill_switch_latched_.load()) {
      capture->stop_source.request_stop();
      capture->kill_stop.store(true);
      privacy_abort_requested(capture);
      return;
    }
    try {
      if (speech_exclusion_) {
        capture->speech_excluded.store(true);
        speech_exclusion_(true);
      }
    } catch (...) {
      fail(capture, "speech_exclusion_failed",
           "Listening could not reserve the voice transport.");
      return;
    }
    std::optional<ActiveVoxListeningContext> gated_vox;
    try {
      gated_vox = vox_context_();
    } catch (...) {
      fail(capture, "speech_exclusion_failed",
           "Listening could not verify exclusive voice transport.");
      return;
    }
    if (!gated_vox || gated_vox->session_id != vox->session_id ||
        gated_vox->connection_generation != vox->connection_generation ||
        !gated_vox->ready || !gated_vox->speech_idle) {
      fail(capture, "speech_exclusion_failed",
           "Listening could not establish exclusive use of voice transport.");
      return;
    }
    const auto public_id = send_public(
        arming_message(*capture), ids_.next_id(),
        ended_message("Listening did not begin because its public arming "
                      "indicator was not confirmed in time."));
    if (!public_id) {
      fail(capture, "arming_delivery_failed",
           "The public listening indicator could not be confirmed.");
      return;
    }
    capture->window.public_message_id = *public_id;
    capture->current_public_message_id = *public_id;
    repository_.record_public_message(capture->window.window_id, *public_id,
                                      unix_milliseconds(clock_));
    if (!advance(capture, VoiceListeningState::arming_transport,
                 "public_arming_confirmed")) {
      fail(capture, "transport_enable_failed",
           "Voice receive transport could not be enabled.");
      return;
    }
    if (handle_arming_cancellation(capture, worker_stop))
      return;
    if (!adapter_.enable_transport(capture->arm,
                                   capture->stop_source.get_token()) ||
        !advance(capture, VoiceListeningState::arming_indicator,
                 "transport_enabled")) {
      fail(capture, "transport_enable_failed",
           "Voice receive transport could not be enabled.");
      return;
    }
    if (handle_arming_cancellation(capture, worker_stop))
      return;
    const auto armed_presence = adapter_.preflight(capture->arm);
    if (!armed_presence.available || !armed_presence.requester_present ||
        armed_presence.other_bot_present || armed_presence.human_count == 0) {
      fail(capture, "membership_changed_while_arming",
           "Listening stopped because voice-channel membership changed while "
           "arming.");
      return;
    }
    const auto advertised_at_ms = unix_milliseconds(clock_);
    const auto capture_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{duration_seconds};
    if (!edit_public(*public_id, listening_message(*capture, advertised_at_ms),
                     capture->stop_source.get_token())) {
      if (capture->stop_source.stop_requested()) {
        privacy_abort_requested(capture);
      } else {
        fail(capture, "listening_delivery_failed",
             "The active listening indicator could not be confirmed.");
      }
      return;
    }
    if (std::chrono::steady_clock::now() >= capture_deadline) {
      fail(capture, "listening_deadline_elapsed",
           "The advertised listening window elapsed before capture could be "
           "armed.");
      return;
    }
    if (handle_arming_cancellation(capture, worker_stop))
      return;
    if (!advance(capture, VoiceListeningState::active,
                 "listening_indicator_confirmed")) {
      fail(capture, "capture_arm_failed", "Voice capture could not be armed.");
      return;
    }
    if (handle_arming_cancellation(capture, worker_stop))
      return;
    if (!adapter_.arm(capture->arm)) {
      fail(capture, "capture_arm_failed", "Voice capture could not be armed.");
      return;
    }
    start_timer(capture, capture_deadline);
    bool shutdown_cancelled{};
    bool privacy_cancelled{};
    bool deadline_elapsed{};
    {
      const std::scoped_lock lock{state_mutex_};
      shutdown_cancelled = stopped_.load() || worker_stop.stop_requested();
      privacy_cancelled = capture->stop_source.stop_requested();
      deadline_elapsed = std::chrono::steady_clock::now() >= capture_deadline;
      if (!shutdown_cancelled && !privacy_cancelled && !deadline_elapsed &&
          active_ == capture)
        capture->accepting_audio.store(true);
    }
    if (shutdown_cancelled) {
      capture->stop_source.request_stop();
      capture->accepting_audio.store(false);
      adapter_.disarm();
      return;
    }
    if (privacy_cancelled) {
      capture->accepting_audio.store(false);
      if (capture->failure_submitted.load())
        return;
      privacy_abort_requested(capture);
      return;
    }
    if (deadline_elapsed) {
      fail(capture, "listening_deadline_elapsed",
           "The advertised listening window elapsed before capture could be "
           "armed.");
    }
  }

  [[nodiscard]] std::string
  begin_failure(const VoiceWindowBeginCode code) const {
    switch (code) {
    case VoiceWindowBeginCode::replay:
      return "That listening request was already handled.";
    case VoiceWindowBeginCode::active_window:
      return "Another listening window is already active.";
    case VoiceWindowBeginCode::window_limit:
      return "The rolling daily listening-window limit has been reached.";
    case VoiceWindowBeginCode::daily_budget:
      return "The rolling daily transcription budget has been reached.";
    case VoiceWindowBeginCode::monthly_budget:
      return "The monthly transcription budget has been reached.";
    case VoiceWindowBeginCode::kill_switch:
      return "Voice listening is disabled by the owner kill switch.";
    case VoiceWindowBeginCode::consent_missing:
      return "Voice listening is disabled because guild-wide prior consent is "
             "not attested.";
    case VoiceWindowBeginCode::created:
      break;
    }
    return "Voice listening is unavailable.";
  }

  [[nodiscard]] bool
  handle_arming_cancellation(const std::shared_ptr<Capture> &capture,
                             const std::stop_token worker_stop) {
    if (stopped_.load() || worker_stop.stop_requested()) {
      capture->stop_source.request_stop();
      capture->accepting_audio.store(false);
      adapter_.disarm();
      return true;
    }
    if (!capture->stop_source.stop_requested())
      return false;
    if (capture->failure_submitted.load()) {
      capture->accepting_audio.store(false);
      adapter_.disarm();
      return true;
    }
    privacy_abort_requested(capture);
    return true;
  }

  bool advance(const std::shared_ptr<Capture> &capture,
               const VoiceListeningState target, std::string reason,
               std::optional<DiscordSnowflake> actor = std::nullopt) {
    const auto updated = repository_.transition(
        {.window_id = capture->window.window_id,
         .expected_revision = capture->window.revision,
         .target = target,
         .reason = reason,
         .actor_user_id = actor,
         .transition_id = ids_.next_id(),
         .idempotency_key = "voice:transition:" + capture->window.window_id +
                            ":" + std::to_string(capture->window.revision + 1),
         .now_ms = unix_milliseconds(clock_)});
    if (!updated)
      return false;
    {
      const std::scoped_lock lock{state_mutex_};
      capture->window = *updated;
    }
    return true;
  }

  void fail(const std::shared_ptr<Capture> &capture, std::string reason,
            std::string public_reason) noexcept {
    capture->accepting_audio.store(false);
    capture->stop_source.request_stop();
    adapter_.disarm();
    stop_timer();
    scrub_capture(capture, false);
    static_cast<void>(confirm_transport_privacy(capture));
    const auto provider_sent = capture->provider_sent.load();
    try {
      if (!terminal(capture->window.state))
        static_cast<void>(
            advance(capture, VoiceListeningState::failed, reason));
    } catch (...) {
    }
    record_terminal_usage(capture, reason, provider_sent);
    publish_ended(capture, ended_message(std::move(public_reason)));
    {
      const std::scoped_lock lock{failure_mutex_};
      last_failure_category_ = reason;
    }
    try {
      complete_initiator(capture,
                         text_message("Listening ended without a transcript."));
    } catch (...) {
    }
    clear_active(capture);
  }

  void privacy_abort_requested(const std::shared_ptr<Capture> &capture) {
    if (!claim_privacy_abort(capture))
      return;
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_ != capture)
        return;
    }
    capture->accepting_audio.store(false);
    capture->stop_source.request_stop();
    adapter_.disarm();
    stop_timer();
    scrub_capture(capture, false);
    static_cast<void>(confirm_transport_privacy(capture));
    const auto actor = capture->stop_actor.load();
    const auto reason = capture->kill_stop.load() ? "kill_switch" : "user_stop";
    const auto provider_sent = capture->provider_sent.load();
    try {
      if (!terminal(capture->window.state))
        static_cast<void>(
            advance(capture, VoiceListeningState::stopped, reason,
                    actor == 0 ? std::nullopt
                               : std::optional{DiscordSnowflake{actor}}));
    } catch (...) {
    }
    record_terminal_usage(capture,
                          provider_sent ? "stopped_after_send" : "stopped",
                          provider_sent);
    publish_ended(
        capture,
        ended_message("Listening was stopped. Buffered audio was discarded; "
                      "no transcript was produced."));
    complete_initiator_privacy_abort(
        capture,
        text_message("Listening was stopped. No transcript was produced."));
    clear_active(capture);
  }

  void abort_active(IncomingInteraction interaction, Completion completion,
                    std::optional<std::string> window_id,
                    const std::string &reason) noexcept {
    auto fallback = completion;
    try {
      abort_active_impl(std::move(interaction), std::move(completion),
                        std::move(window_id), reason);
    } catch (...) {
      std::shared_ptr<Capture> capture;
      {
        const std::scoped_lock lock{state_mutex_};
        capture = active_;
      }
      if (capture)
        fail(capture, "privacy_abort_failed",
             "Listening was disarmed after its stop audit failed.");
      if (fallback) {
        try {
          fallback(text_message(
              "Listening was disarmed and buffered audio was discarded."));
        } catch (...) {
        }
      }
    }
  }

  void abort_active_impl(IncomingInteraction interaction, Completion completion,
                         std::optional<std::string> window_id,
                         const std::string &reason) {
    const std::scoped_lock operation_lock{operation_mutex_};
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
    }
    if (!capture || (window_id && *window_id != capture->window.window_id) ||
        !claim_privacy_abort(capture)) {
      if (completion)
        completion(text_message(
            "No matching listening window remains active; any matched capture "
            "was already disarmed."));
      return;
    }
    capture->stop_source.request_stop();
    capture->accepting_audio.store(false);
    stop_timer();
    adapter_.disarm();
    scrub_capture(capture, false);
    static_cast<void>(confirm_transport_privacy(capture));
    try {
      if (!terminal(capture->window.state))
        static_cast<void>(advance(capture, VoiceListeningState::stopped, reason,
                                  interaction.user_id.is_set()
                                      ? std::optional{interaction.user_id}
                                      : std::nullopt));
    } catch (...) {
    }
    const auto provider_sent = capture->provider_sent.load();
    record_terminal_usage(capture,
                          provider_sent ? "stopped_after_send" : "stopped",
                          provider_sent);
    publish_ended(
        capture,
        ended_message("Listening was stopped. Buffered audio was discarded; "
                      "no transcript was produced."));
    complete_initiator_privacy_abort(
        capture,
        text_message("Listening was stopped. No transcript was produced."));
    clear_active(capture);
    if (completion)
      completion(
          text_message("Listening stopped and buffered audio discarded."));
  }

  void expire(const std::shared_ptr<Capture> &capture) noexcept {
    try {
      expire_impl(capture);
    } catch (...) {
      fail(capture, "expiry_failure",
           "Listening ended because its expiry lifecycle failed.");
    }
  }

  void expire_impl(const std::shared_ptr<Capture> &capture) {
    const std::scoped_lock operation_lock{operation_mutex_};
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_ != capture ||
          capture->window.state != VoiceListeningState::active)
        return;
    }
    capture->accepting_audio.store(false);
    adapter_.disarm();
    if (!confirm_transport_privacy(capture)) {
      fail(capture, "transport_restore_failed",
           "Capture ended, but voice transport privacy could not be confirmed. "
           "Buffered audio was discarded without transcription.");
      return;
    }
    stop_timer(false);
    if (!advance(capture, VoiceListeningState::transcribing,
                 "window_expired")) {
      fail(capture, "transcription_transition_failed",
           "Listening ended because state could not be finalized.");
      return;
    }
    const auto ended_status_confirmed = publish_ended(
        capture,
        ended_message("Capture has ended. The short in-memory buffer is being "
                      "transcribed and will then be scrubbed."));
    if (ended_status_confirmed)
      release_speech_exclusion(capture);
    bool no_audio{};
    {
      const std::scoped_lock audio_lock{capture->audio_mutex};
      no_audio = capture->audio.size() == 0;
    }
    if (no_audio) {
      fail(capture, "no_audio", "No audio was received; nothing was sent.");
      return;
    }
    const auto submitted = transcription_worker_.try_submit(
        [this, capture](const std::stop_token worker_stop) {
          transcribe(capture, worker_stop);
        });
    if (submitted != SubmitResult::accepted)
      fail(capture, "transcription_queue_full",
           "The transcription worker was unavailable; nothing was retained.");
  }

  void transcribe(const std::shared_ptr<Capture> &capture,
                  const std::stop_token worker_stop) {
    const auto started = std::chrono::steady_clock::now();
    auto result = std::make_shared<PendingTranscriptionResult>();
    try {
      if (worker_stop.stop_requested() || capture->stop_source.stop_requested())
        throw TranscriptionError{TranscriptionFailureCategory::cancelled,
                                 "Transcription was cancelled."};
      // Persist an ambiguity marker before entering the network client. A
      // graceful no-send failure records provider_sent=false and releases the
      // reservation; a crash cannot incorrectly claim that no request began.
      repository_.record_provider_attempt(capture->window_id,
                                          unix_milliseconds(clock_));
      const std::scoped_lock audio_lock{capture->audio_mutex};
      result->transcript = transcription_->transcribe(
          {.pcm = capture->audio.bytes(),
           .sample_rate = 48'000,
           .channels = 2,
           .bits_per_sample = 16,
           .timeout = configuration_.request_timeout},
          capture->stop_source.get_token(),
          [capture] { capture->provider_sent.store(true); });
    } catch (const TranscriptionError &error) {
      result->failure.emplace(error.category(), error.what(),
                              error.provider_request_id());
    } catch (...) {
      result->failure.emplace(TranscriptionFailureCategory::transport,
                              "Unknown transcription failure.");
    }
    const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    auto finalize = [this, capture, result, latency](std::stop_token) mutable {
      finish_transcription(capture, result, latency);
    };
    SubmitResult submitted{SubmitResult::stopping};
    try {
      submitted =
          privacy_worker_.try_submit(finalize, [result] { result->scrub(); });
    } catch (...) {
    }
    if (submitted != SubmitResult::accepted) {
      ++callback_drops_;
      finalize({});
    }
  }

  void finish_transcription(
      const std::shared_ptr<Capture> &capture,
      const std::shared_ptr<PendingTranscriptionResult> &result,
      const std::int64_t latency_ms) noexcept {
    try {
      finish_transcription_impl(capture, result->transcript, result->failure,
                                latency_ms);
    } catch (...) {
      fail(capture, "transcription_finalize_failed",
           "Transcription ended but its privacy lifecycle failed.");
    }
  }

  void finish_transcription_impl(const std::shared_ptr<Capture> &capture,
                                 Transcript &transcript,
                                 std::optional<TranscriptionError> &failure,
                                 const std::int64_t latency_ms) {
    const std::scoped_lock operation_lock{operation_mutex_};
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_ != capture ||
          capture->window.state != VoiceListeningState::transcribing) {
        const std::scoped_lock audio_lock{capture->audio_mutex};
        capture->audio.scrub();
        scrub_string(transcript.text);
        return;
      }
    }
    scrub_capture(capture, false);
    if (!claim_provider_result(capture)) {
      scrub_string(transcript.text);
      if (stopped_.load() || capture->failure_submitted.load())
        return;
      privacy_abort_requested(capture);
      return;
    }
    if (failure) {
      scrub_string(transcript.text);
      if (!finalize_provider_result(capture)) {
        if (stopped_.load())
          complete_initiator_privacy_abort(
              capture,
              text_message("Voice listening shut down. No transcript "
                           "was retained or delivered."),
              true);
        else
          privacy_abort_requested(capture);
        return;
      }
      record_usage_once(
          capture, transcription_failure_category_name(failure->category()),
          capture->provider_sent.load(), failure->provider_request_id(),
          latency_ms);
      fail(capture,
           std::string{"transcription_"} +
               transcription_failure_category_name(failure->category()),
           "Transcription failed; the local audio buffer was scrubbed.");
      return;
    }
    std::optional<std::string> token;
    if (chronicle_) {
      token = ids_.next_id();
      {
        const std::scoped_lock lock{draft_mutex_};
        purge_drafts_locked(std::chrono::steady_clock::now());
        while (drafts_.size() >= 16) {
          auto evicted = drafts_.begin();
          scrub_draft(evicted->second);
          drafts_.erase(evicted);
        }
        drafts_.emplace(
            *token, TranscriptDraft{
                        .window_id = capture->window.window_id,
                        .guild_id = capture->window.guild_id,
                        .channel_id = capture->window.text_channel_id,
                        .user_id = capture->window.requester_user_id,
                        .text = transcript.text,
                        .expires_at = std::chrono::steady_clock::now() +
                                      configuration_.transcript_draft_lifetime,
                        .state = DraftState::active});
      }
      draft_condition_.notify_all();
    }
    InteractionMessage result;
    try {
      result.content = "Transcript:\n";
      result.content.append(transcript.text);
      scrub_string(transcript.text);
      if (token) {
        result.buttons.push_back(
            {.custom_id =
                 std::string{voice_transcript_component_prefix} + *token,
             .label = "Propose excerpt to Chronicle",
             .disabled = false,
             .style = ButtonStyle::secondary});
      }
      const auto delivery_result = deliver_initiator(capture, result);
      scrub_string(result.content);
      if (!finalize_provider_result(capture)) {
        discard_draft(token);
        if (stopped_.load())
          complete_initiator_privacy_abort(
              capture,
              text_message("Voice listening shut down. No transcript was "
                           "retained or delivered."),
              true);
        else
          privacy_abort_requested(capture);
        return;
      }
      if (delivery_result != DeliveryResult::success) {
        discard_draft(token);
        record_usage_once(capture, "transcript_delivery_failed", true,
                          transcript.provider_request_id, latency_ms);
        if (!advance(capture, VoiceListeningState::failed,
                     "transcript_delivery_failed")) {
          fail(capture, "delivery_failure_transition_failed",
               "Transcription ended, but its private delivery and audit state "
               "could not be confirmed.");
          return;
        }
        {
          const std::scoped_lock lock{failure_mutex_};
          last_failure_category_ = "transcript_delivery_failed";
        }
        diagnostics_.emit(
            {DiagnosticSeverity::warning, "voice_input.transcript_delivery",
             "Discord did not confirm the private transcript delivery; the "
             "volatile transcript was scrubbed.",
             std::nullopt});
        publish_ended(
            capture,
            ended_message("Transcription ended, but Discord did not confirm "
                          "private transcript delivery. The transcript was "
                          "scrubbed and is unavailable."));
        complete_initiator_privacy_abort(
            capture,
            text_message("Transcription ended without confirmed private "
                         "delivery. No transcript was retained."),
            true);
        clear_active(capture);
        return;
      }
      if (!complete_transcription_once(capture, transcript.provider_request_id,
                                       latency_ms)) {
        discard_draft(token);
        fail(capture, "completion_transition_failed",
             "The transcript was delivered privately, but its completion "
             "audit could not be committed.");
        return;
      }
      publish_ended(
          capture,
          ended_message(
              "Listening and transcription completed. The transcript was "
              "delivered ephemerally to the requester."));
      clear_active(capture);
    } catch (...) {
      scrub_string(result.content);
      scrub_string(transcript.text);
      discard_draft(token);
      throw;
    }
  }

  [[nodiscard]] VoiceTranscriptionUsage
  make_usage(const std::shared_ptr<Capture> &capture, std::string result_code,
             const bool provider_sent, std::string provider_request_id,
             const std::int64_t latency_ms) {
    std::size_t bytes{};
    {
      const std::scoped_lock audio_lock{capture->audio_mutex};
      bytes = capture->audio.size();
    }
    bytes = std::max(bytes, capture->captured_bytes_before_scrub.load());
    provider_request_id =
        sanitize_transcription_request_id(provider_request_id);
    return {.window_id = capture->window.window_id,
            .provider = "openai",
            .model = std::string{transcription_model},
            .provider_request_id =
                provider_request_id.empty()
                    ? std::nullopt
                    : std::optional{std::move(provider_request_id)},
            .captured_bytes = bytes,
            .captured_duration_ms = static_cast<std::int64_t>(
                (bytes * 1'000U) / (48'000U * 2U * 2U)),
            .estimated_micro_usd =
                provider_sent ? capture->window.reserved_micro_usd : 0,
            .latency_ms = latency_ms,
            .result_code = std::move(result_code),
            .provider_sent = provider_sent,
            .recorded_at_ms = unix_milliseconds(clock_)};
  }

  [[nodiscard]] bool
  complete_transcription_once(const std::shared_ptr<Capture> &capture,
                              std::string provider_request_id,
                              const std::int64_t latency_ms) {
    if (capture->usage_recorded.exchange(true))
      return capture->window.state == VoiceListeningState::completed;
    try {
      const auto usage = make_usage(capture, "completed", true,
                                    std::move(provider_request_id), latency_ms);
      const auto updated = repository_.complete_transcription(
          {.window_id = capture->window.window_id,
           .expected_revision = capture->window.revision,
           .target = VoiceListeningState::completed,
           .reason = "transcription_completed",
           .actor_user_id = std::nullopt,
           .transition_id = ids_.next_id(),
           .idempotency_key = "voice:transition:" + capture->window.window_id +
                              ":" +
                              std::to_string(capture->window.revision + 1),
           .now_ms = unix_milliseconds(clock_)},
          usage);
      if (!updated) {
        capture->usage_recorded.store(false);
        return false;
      }
      {
        const std::scoped_lock lock{state_mutex_};
        capture->window = *updated;
      }
      return true;
    } catch (...) {
      capture->usage_recorded.store(false);
      throw;
    }
  }

  void record_usage_once(const std::shared_ptr<Capture> &capture,
                         std::string result_code, const bool provider_sent,
                         std::string provider_request_id,
                         const std::int64_t latency_ms) {
    if (capture->usage_recorded.exchange(true))
      return;
    try {
      repository_.record_usage(
          make_usage(capture, std::move(result_code), provider_sent,
                     std::move(provider_request_id), latency_ms));
    } catch (...) {
      capture->usage_recorded.store(false);
      throw;
    }
  }

  void record_terminal_usage(const std::shared_ptr<Capture> &capture,
                             std::string result_code,
                             const bool provider_sent) noexcept {
    try {
      record_usage_once(capture, std::move(result_code), provider_sent, {}, 0);
      return;
    } catch (...) {
      try {
        diagnostics_.emit(
            {DiagnosticSeverity::error, "voice_input.usage_audit",
             "A terminal voice-listening usage audit could not be recorded.",
             std::nullopt});
      } catch (...) {
      }
    }
    if (provider_sent)
      return;
    try {
      repository_.release_reservation(capture->window.window_id,
                                      unix_milliseconds(clock_));
    } catch (...) {
      try {
        diagnostics_.emit(
            {DiagnosticSeverity::error, "voice_input.reservation_release",
             "An unsent voice-listening reservation could not be released "
             "after its usage audit failed.",
             std::nullopt});
      } catch (...) {
      }
    }
  }

  void complete_initiator(const std::shared_ptr<Capture> &capture,
                          InteractionMessage message) noexcept {
    if (capture->completion_sent.exchange(true) ||
        !capture->initiator_completion)
      return;
    try {
      capture->initiator_completion(std::move(message), {});
    } catch (...) {
    }
  }

  void complete_initiator_privacy_abort(
      const std::shared_ptr<Capture> &capture, InteractionMessage message,
      const bool wait_until_resolved = false) noexcept {
    if (capture->privacy_overwrite_confirmed.load())
      return;
    const auto overwrite_required = capture->completion_sent.exchange(true);
    if (!capture->initiator_completion)
      return;
    if (!overwrite_required) {
      try {
        capture->initiator_completion(std::move(message), {});
      } catch (...) {
      }
      return;
    }
    auto wait = std::make_shared<EditWait>();
    try {
      capture->initiator_completion(std::move(message),
                                    [wait](const DeliveryResult result) {
                                      const std::scoped_lock lock{wait->mutex};
                                      if (wait->done)
                                        return;
                                      wait->result = result;
                                      wait->done = true;
                                      wait->condition.notify_all();
                                    });
    } catch (...) {
      return;
    }
    std::unique_lock lock{wait->mutex};
    bool resolved{};
    if (wait_until_resolved) {
      wait->condition.wait(lock, [&wait] { return wait->done; });
      resolved = true;
    } else {
      resolved =
          wait->condition.wait_for(lock, configuration_.public_status_timeout,
                                   [&wait] { return wait->done; });
    }
    if (resolved && wait->result == DeliveryResult::success) {
      capture->privacy_overwrite_confirmed.store(true);
    } else {
      try {
        diagnostics_.emit(
            {DiagnosticSeverity::warning,
             "voice_input.private_privacy_overwrite",
             "Discord did not confirm the terminal private privacy "
             "overwrite.",
             std::nullopt});
      } catch (...) {
      }
    }
  }

  [[nodiscard]] DeliveryResult
  deliver_initiator(const std::shared_ptr<Capture> &capture,
                    const InteractionMessage &message) noexcept {
    auto wait = std::make_shared<EditWait>();
    {
      const std::scoped_lock result_lock{capture->provider_result_mutex};
      if (capture->provider_result_disposition.load() !=
          ProviderResultDisposition::finalizing)
        return DeliveryResult::permanent_failure;
      if (kill_switch_latched_.load() || stopped_.load() ||
          capture->stop_source.stop_requested()) {
        capture->provider_result_disposition.store(
            ProviderResultDisposition::privacy_abort);
        return DeliveryResult::permanent_failure;
      }
      if (capture->completion_sent.exchange(true) ||
          !capture->initiator_completion)
        return DeliveryResult::permanent_failure;
      try {
        capture->initiator_completion(
            message, [wait](const DeliveryResult result) {
              const std::scoped_lock lock{wait->mutex};
              if (wait->done)
                return;
              wait->result = result;
              wait->done = true;
              wait->condition.notify_all();
            });
      } catch (...) {
        return DeliveryResult::unknown_outcome;
      }
    }
    const auto stop_token = capture->stop_source.get_token();
    const std::stop_callback stop_wait{
        stop_token, [wait] { wait->condition.notify_all(); }};
    std::unique_lock lock{wait->mutex};
    if (!wait->condition.wait_for(
            lock, configuration_.public_status_timeout, [&wait, stop_token] {
              return wait->done || stop_token.stop_requested();
            }))
      return DeliveryResult::unknown_outcome;
    if (!wait->done)
      return DeliveryResult::permanent_failure;
    return wait->result;
  }

  void clear_active(const std::shared_ptr<Capture> &capture) {
    bool cleared{};
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_ == capture) {
        active_.reset();
        cleared = true;
      }
    }
    if (cleared)
      release_speech_exclusion(capture);
  }

  void
  release_speech_exclusion(const std::shared_ptr<Capture> &capture) noexcept {
    if (!speech_exclusion_ || !capture->transport_privacy_safe.load() ||
        transport_privacy_blocked_.load() || public_status_blocked_.load() ||
        !capture->speech_excluded.exchange(false))
      return;
    try {
      speech_exclusion_(false);
    } catch (...) {
      capture->speech_excluded.store(true);
    }
  }

  static void scrub_capture(const std::shared_ptr<Capture> &capture,
                            const bool try_only) noexcept {
    if (!capture)
      return;
    std::unique_lock lock{capture->audio_mutex, std::defer_lock};
    if (try_only) {
      if (!lock.try_lock())
        return;
    } else {
      lock.lock();
    }
    auto observed = capture->captured_bytes_before_scrub.load();
    const auto captured = capture->audio.size();
    while (observed < captured &&
           !capture->captured_bytes_before_scrub.compare_exchange_weak(
               observed, captured)) {
    }
    capture->audio.scrub();
  }

  void receive_audio(const std::string_view session_id,
                     const std::uint64_t generation,
                     const std::span<const std::byte> audio) noexcept {
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
    }
    if (!capture || !capture->accepting_audio.load() ||
        capture->stop_source.stop_requested() ||
        capture->arm.session_id != session_id ||
        capture->arm.generation != generation || audio.empty() ||
        audio.size() % 4U != 0)
      return;
    if (audio.size() > maximum_voice_callback_pcm_bytes) {
      submit_callback_failure(
          capture, "audio_frame_oversized",
          "Listening stopped because a received audio block was oversized.");
      return;
    }
    std::unique_lock audio_lock{capture->audio_mutex, std::try_to_lock};
    if (!audio_lock.owns_lock()) {
      ++callback_drops_;
      return;
    }
    if (!capture->accepting_audio.load())
      return;
    const auto requested_limit = capture->requested_pcm_bytes;
    const auto captured = capture->audio.size();
    if (captured >= requested_limit) {
      audio_lock.unlock();
      submit_capacity_expiry(capture);
      return;
    }
    const auto remaining = requested_limit - captured;
    if (audio.size() > remaining) {
      if (!capture->audio.append(audio.first(remaining))) {
        audio_lock.unlock();
        submit_callback_failure(
            capture, "audio_buffer_overflow",
            "Listening stopped because the bounded audio buffer failed.");
        return;
      }
      audio_lock.unlock();
      submit_capacity_expiry(capture);
      return;
    }
    if (!capture->audio.append(audio)) {
      audio_lock.unlock();
      submit_callback_failure(
          capture, "audio_buffer_overflow",
          "Listening stopped because the bounded audio buffer failed.");
    }
  }

  void receive_event(VoiceInputEvent event) noexcept {
    std::shared_ptr<Capture> capture;
    {
      const std::scoped_lock lock{state_mutex_};
      capture = active_;
    }
    if (!capture || capture->arm.session_id != event.session_id)
      return;
    if (event.generation != 0 && capture->arm.generation != event.generation) {
      submit_callback_failure(
          capture, "voice_connection_changed",
          "Listening stopped because the voice connection changed.");
      return;
    }
    if (event.kind == VoiceInputEventKind::membership_changed) {
      submit_membership_validation(capture);
      return;
    }
    submit_callback_failure(
        capture, std::string{"voice_"} + event_reason(event.kind),
        "Listening stopped because the voice session changed.");
  }

  void submit_membership_validation(
      const std::shared_ptr<Capture> &capture) noexcept {
    if (capture->membership_validation_pending.exchange(true))
      return;
    const auto submitted = callback_worker_.try_submit(
        [this, capture](std::stop_token) {
          capture->membership_validation_pending.store(false);
          validate_membership(capture);
        },
        [capture] { capture->membership_validation_pending.store(false); });
    if (submitted == SubmitResult::accepted)
      return;
    capture->membership_validation_pending.store(false);
    if (!stopped_.load())
      submit_callback_failure(capture, "voice_membership_uncertain",
                              "Listening stopped because channel membership "
                              "could not be verified.");
  }

  void validate_membership(const std::shared_ptr<Capture> &capture) noexcept {
    if (capture->stop_source.stop_requested())
      return;
    {
      const std::scoped_lock lock{state_mutex_};
      if (active_ != capture || terminal(capture->window.state))
        return;
    }
    VoiceInputPresence presence;
    try {
      presence = adapter_.preflight(capture->arm);
    } catch (...) {
      presence = {};
    }
    std::string reason;
    if (!presence.available)
      reason = "voice_membership_uncertain";
    else if (!presence.requester_present)
      reason = "voice_requester_left";
    else if (presence.human_count == 0)
      reason = "voice_empty";
    else if (presence.other_bot_present)
      reason = "voice_other_bot_joined";
    else
      return;
    if (capture->failure_submitted.exchange(true))
      return;
    if (!claim_privacy_abort(capture)) {
      capture->failure_submitted.store(false);
      return;
    }
    capture->stop_source.request_stop();
    capture->accepting_audio.store(false);
    adapter_.disarm();
    const std::scoped_lock operation_lock{operation_mutex_};
    fail(capture, std::move(reason),
         "Listening stopped because voice-channel membership changed.");
  }

  void submit_callback_failure(const std::shared_ptr<Capture> &capture,
                               std::string reason,
                               std::string public_reason) noexcept {
    if (capture->failure_submitted.exchange(true))
      return;
    if (!claim_privacy_abort(capture)) {
      capture->failure_submitted.store(false);
      return;
    }
    capture->stop_source.request_stop();
    capture->accepting_audio.store(false);
    adapter_.disarm();
    auto cleanup = [this, capture, reason,
                    public_reason](std::stop_token) mutable {
      const std::scoped_lock operation_lock{operation_mutex_};
      fail(capture, std::move(reason), std::move(public_reason));
    };
    if (callback_worker_.try_submit_front(std::move(cleanup)) ==
        SubmitResult::accepted)
      return;
    scrub_after_callback_rejection(capture);
    auto fallback = [this, capture, reason,
                     public_reason](std::stop_token) mutable {
      const std::scoped_lock operation_lock{operation_mutex_};
      fail(capture, std::move(reason), std::move(public_reason));
    };
    const auto fallback_submitted = callback_cleanup_worker_.try_submit(
        std::move(fallback),
        [this, capture] { scrub_after_callback_rejection(capture); });
    if (fallback_submitted == SubmitResult::accepted || stopped_.load())
      return;
    const std::scoped_lock operation_lock{operation_mutex_};
    fail(capture, std::move(reason), std::move(public_reason));
  }

  void
  submit_capacity_expiry(const std::shared_ptr<Capture> &capture) noexcept {
    if (capture->capacity_expiry_submitted.exchange(true))
      return;
    capture->accepting_audio.store(false);
    adapter_.disarm();
    auto expire_at_capacity = [this, capture](std::stop_token) {
      expire(capture);
    };
    if (callback_worker_.try_submit_front(std::move(expire_at_capacity)) ==
        SubmitResult::accepted)
      return;
    submit_callback_failure(capture, "audio_capacity_queue_rejected",
                            "Listening stopped because its bounded completion "
                            "work was unavailable.");
  }

  [[nodiscard]] static std::string
  event_reason(const VoiceInputEventKind kind) {
    switch (kind) {
    case VoiceInputEventKind::requester_left:
      return "requester_left";
    case VoiceInputEventKind::empty:
      return "empty";
    case VoiceInputEventKind::other_bot_joined:
      return "other_bot_joined";
    case VoiceInputEventKind::connection_changed:
      return "connection_changed";
    case VoiceInputEventKind::unavailable:
      return "unavailable";
    case VoiceInputEventKind::membership_changed:
      return "membership_changed";
    }
    return "unavailable";
  }

  void start_timer(const std::shared_ptr<Capture> &capture,
                   const std::chrono::steady_clock::time_point deadline) {
    stop_timer();
    timer_ = std::jthread([this, capture, deadline](std::stop_token token) {
      std::unique_lock lock{timer_mutex_};
      if (timer_condition_.wait_until(lock, token, deadline,
                                      [] { return false; }))
        return;
      if (token.stop_requested())
        return;
      capture->accepting_audio.store(false);
      adapter_.disarm();
      if (privacy_worker_.try_submit([this, capture](std::stop_token) {
            expire(capture);
          }) != SubmitResult::accepted) {
        ++callback_drops_;
        lock.unlock();
        expire(capture);
      }
    });
  }

  void scrub_after_callback_rejection(
      const std::shared_ptr<Capture> &capture) noexcept {
    ++callback_drops_;
    scrub_capture(capture, true);
  }

  void stop_timer(const bool join = true) noexcept {
    timer_.request_stop();
    timer_condition_.notify_all();
    if (join && timer_.joinable() &&
        timer_.get_id() != std::this_thread::get_id()) {
      try {
        timer_.join();
      } catch (...) {
      }
    }
  }

  static void scrub_draft(TranscriptDraft &draft) noexcept {
    scrub_string(draft.text);
  }

  void discard_draft(const std::optional<std::string> &token) noexcept {
    if (!token)
      return;
    const std::scoped_lock lock{draft_mutex_};
    const auto found = drafts_.find(*token);
    if (found == drafts_.end())
      return;
    scrub_draft(found->second);
    drafts_.erase(found);
  }

  void purge_drafts_locked(
      const std::chrono::steady_clock::time_point now) noexcept {
    for (auto item = drafts_.begin(); item != drafts_.end();) {
      if (item->second.expires_at > now) {
        ++item;
        continue;
      }
      scrub_draft(item->second);
      item = drafts_.erase(item);
    }
  }

  void reap_drafts(const std::stop_token token) noexcept {
    std::unique_lock lock{draft_mutex_};
    while (!token.stop_requested()) {
      purge_drafts_locked(std::chrono::steady_clock::now());
      if (token.stop_requested())
        break;
      if (drafts_.empty()) {
        static_cast<void>(draft_condition_.wait(
            lock, token, [this] { return !drafts_.empty(); }));
        continue;
      }
      const auto next = std::ranges::min_element(
          drafts_, {}, [](const auto &item) { return item.second.expires_at; });
      const auto next_expiry = next->second.expires_at;
      static_cast<void>(draft_condition_.wait_until(
          lock, token, next_expiry, [this, next_expiry] {
            return std::ranges::any_of(
                drafts_, [next_expiry](const auto &item) {
                  return item.second.expires_at < next_expiry;
                });
          }));
    }
  }

  void scrub_all_drafts() noexcept {
    const std::scoped_lock lock{draft_mutex_};
    for (auto &[token, draft] : drafts_) {
      static_cast<void>(token);
      scrub_draft(draft);
    }
    drafts_.clear();
  }

  [[nodiscard]] std::optional<std::string> last_failure() const {
    const std::scoped_lock lock{failure_mutex_};
    return last_failure_category_;
  }

  VoiceListeningRepository &repository_;
  VoiceInputAdapter &adapter_;
  TranscriptionClient *transcription_;
  DiscordPublicDelivery &public_delivery_;
  std::shared_ptr<OrderedPublicEditor> public_editor_;
  std::shared_ptr<CallbackFence> late_delivery_callbacks_{
      std::make_shared<CallbackFence>()};
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  ServerScopeConfiguration scope_;
  VoiceListeningConfiguration configuration_;
  VoxContextProvider vox_context_;
  ChronicleService *chronicle_;
  SpeechExclusion speech_exclusion_;
  mutable std::mutex operation_mutex_;
  mutable std::mutex state_mutex_;
  std::shared_ptr<Capture> active_;
  mutable std::mutex draft_mutex_;
  std::condition_variable_any draft_condition_;
  std::unordered_map<std::string, TranscriptDraft> drafts_;
  std::jthread draft_reaper_;
  mutable std::mutex failure_mutex_;
  std::optional<std::string> last_failure_category_;
  mutable std::mutex disable_fence_mutex_;
  std::uint64_t disable_generation_{};
  std::uint64_t persisted_disable_generation_{};
  DiscordSnowflake pending_disable_actor_;
  std::mutex preemption_mutex_;
  std::condition_variable_any preemption_condition_;
  std::shared_ptr<Capture> preemption_capture_;
  bool preemption_persist_disable_{};
  std::jthread preemption_worker_;
  BoundedExecutor control_worker_;
  BoundedExecutor privacy_worker_;
  BoundedExecutor callback_worker_;
  BoundedExecutor callback_cleanup_worker_;
  BoundedExecutor transcription_worker_;
  std::mutex timer_mutex_;
  std::condition_variable_any timer_condition_;
  std::jthread timer_;
  std::atomic<std::size_t> callback_drops_{};
  std::atomic<bool> kill_switch_latched_{false};
  std::atomic<bool> transport_privacy_blocked_{false};
  std::mutex public_status_mutex_;
  std::weak_ptr<Capture> public_status_blocked_capture_;
  std::atomic<bool> public_status_blocked_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};
};

VoiceListeningService::VoiceListeningService(
    VoiceListeningRepository &repository, VoiceInputAdapter &adapter,
    TranscriptionClient *transcription, DiscordPublicDelivery &public_delivery,
    const Clock &clock, PersistentIdGenerator &ids, Diagnostics &diagnostics,
    ServerScopeConfiguration scope, VoiceListeningConfiguration configuration,
    VoxContextProvider vox_context, ChronicleService *chronicle,
    SpeechExclusion speech_exclusion)
    : impl_{std::make_unique<Impl>(
          repository, adapter, transcription, public_delivery, clock, ids,
          diagnostics, std::move(scope), std::move(configuration),
          std::move(vox_context), chronicle, std::move(speech_exclusion))} {}

VoiceListeningService::~VoiceListeningService() { stop(); }
void VoiceListeningService::start() { impl_->start(); }
void VoiceListeningService::stop() noexcept { impl_->stop(); }

SubmitResult
VoiceListeningService::listen_start(IncomingInteraction interaction,
                                    const std::size_t duration,
                                    Completion completion) {
  ConfirmedCompletion confirmed =
      [completion = std::move(completion)](InteractionMessage message,
                                           DeliveryCallback receipt) mutable {
        try {
          if (!completion) {
            if (receipt)
              receipt(DeliveryResult::permanent_failure);
            return;
          }
          completion(std::move(message));
          if (receipt)
            receipt(DeliveryResult::success);
        } catch (...) {
          if (receipt)
            receipt(DeliveryResult::unknown_outcome);
        }
      };
  return impl_->listen_start(std::move(interaction), duration,
                             std::move(confirmed));
}

SubmitResult
VoiceListeningService::listen_start(IncomingInteraction interaction,
                                    const std::size_t duration,
                                    ConfirmedCompletion completion) {
  return impl_->listen_start(std::move(interaction), duration,
                             std::move(completion));
}

SubmitResult
VoiceListeningService::listen_stop(IncomingInteraction interaction,
                                   Completion completion,
                                   std::optional<std::string> window_id) {
  return impl_->listen_stop(std::move(interaction), std::move(completion),
                            std::move(window_id));
}

SubmitResult VoiceListeningService::set_kill_switch(
    IncomingInteraction interaction, const bool disabled, Completion completion,
    const std::optional<std::uint64_t> disable_generation) {
  return impl_->set_kill_switch(std::move(interaction), disabled,
                                std::move(completion), disable_generation);
}

std::uint64_t VoiceListeningService::preempt_privacy_abort(
    const DiscordSnowflake actor_user_id, const bool latch_kill_switch,
    std::optional<std::string> window_id) noexcept {
  return impl_->preempt_privacy_abort(actor_user_id, latch_kill_switch,
                                      window_id);
}

std::uint64_t VoiceListeningService::disable_generation() const noexcept {
  return impl_->current_disable_generation();
}

std::optional<ModalPayload> VoiceListeningService::transcript_modal(
    const IncomingInteraction &interaction) {
  return impl_->transcript_modal(interaction);
}

void VoiceListeningService::transcript_modal_delivery(
    const IncomingInteraction &interaction,
    const DeliveryResult result) noexcept {
  impl_->transcript_modal_delivery(interaction, result);
}

InteractionMessage VoiceListeningService::propose_transcript(
    const IncomingInteraction &interaction) {
  return impl_->propose_transcript(interaction);
}

VoiceListeningHealth VoiceListeningService::health() const {
  return impl_->health();
}

} // namespace sanguinius
