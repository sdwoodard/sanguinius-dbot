#include "sanguinius/chronicle.hpp"
#include "sanguinius/interaction_handler.hpp"
#include "sanguinius/transcription.hpp"
#include "sanguinius/voice_input.hpp"

#include "support/fake_chronicle_repository.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::uint32_t u32(const std::array<std::byte, 44> &value,
                                const std::size_t offset) {
  return std::to_integer<std::uint32_t>(value[offset]) |
         (std::to_integer<std::uint32_t>(value[offset + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(value[offset + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(value[offset + 3]) << 24U);
}

template <typename Predicate>
[[nodiscard]] bool eventually(Predicate predicate,
                              const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

class OrderingLog final {
public:
  void push(std::string value) {
    const std::scoped_lock lock{mutex_};
    values_.push_back(std::move(value));
  }

  [[nodiscard]] std::vector<std::string> values() const {
    const std::scoped_lock lock{mutex_};
    return values_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> values_;
};

class OrderingVoiceAdapter final : public sanguinius::VoiceInputAdapter {
public:
  explicit OrderingVoiceAdapter(OrderingLog &log) : log_{log} {}

  [[nodiscard]] sanguinius::VoiceInputCapability
  capability() const noexcept override {
    return sanguinius::VoiceInputCapability::ready;
  }
  void start(AudioCallback audio, EventCallback event) override {
    audio_ = std::move(audio);
    event_ = std::move(event);
  }
  [[nodiscard]] sanguinius::VoiceInputPresence
  preflight(const sanguinius::VoiceInputArmRequest &) const override {
    ++preflight_calls;
    if (on_preflight)
      on_preflight();
    return presence;
  }
  [[nodiscard]] bool
  enable_transport(const sanguinius::VoiceInputArmRequest &,
                   const std::stop_token stop_token) override {
    log_.push("undeaf");
    if (!enable_succeeds || stop_token.stop_requested())
      return false;
    transport_enabled_ = true;
    return true;
  }
  [[nodiscard]] bool arm(const sanguinius::VoiceInputArmRequest &) override {
    const auto permitted =
        arm_succeeds && presence.available && presence.requester_present &&
        !presence.other_bot_present && presence.human_count != 0;
    log_.push("arm");
    if (!permitted)
      return false;
    armed_ = true;
    return true;
  }
  void disarm() noexcept override {
    if (armed_.exchange(false))
      log_.push("disarm");
  }
  [[nodiscard]] bool disable_transport() noexcept override {
    if (transport_enabled_.exchange(false))
      log_.push(disable_succeeds ? "self-deaf" : "self-deaf-failed");
    return disable_succeeds;
  }
  void shutdown() noexcept override {}

  void emit_audio(const std::span<const std::byte> audio) const {
    audio_(session_id, generation, audio);
  }

  void emit_event(const sanguinius::VoiceInputEventKind kind,
                  const std::optional<std::uint64_t> event_generation =
                      std::nullopt) const {
    event_({.kind = kind,
            .session_id = session_id,
            .generation = event_generation.value_or(generation),
            .human_count = presence.human_count});
  }

  sanguinius::VoiceInputPresence presence{.available = true,
                                          .requester_present = true,
                                          .other_bot_present = false,
                                          .human_count = 1};
  bool enable_succeeds{true};
  bool arm_succeeds{true};
  bool disable_succeeds{true};
  std::string session_id{"00000000-0000-4000-8000-000000000090"};
  std::uint64_t generation{1};
  mutable std::atomic<std::size_t> preflight_calls{};
  std::function<void()> on_preflight;

private:
  OrderingLog &log_;
  AudioCallback audio_;
  EventCallback event_;
  std::atomic<bool> transport_enabled_{};
  std::atomic<bool> armed_{};
};

class OrderingDelivery final : public sanguinius::DiscordPublicDelivery {
public:
  explicit OrderingDelivery(OrderingLog &log) : log_{log} {}

  void send_public(const sanguinius::PublicMessageRequest &request,
                   std::string_view,
                   sanguinius::PublicDeliveryCallback callback) override {
    const auto arming = request.message.embed &&
                        request.message.embed->title.starts_with("ARMING");
    log_.push(arming ? "public-arming" : "public-ended-replacement");
    if (arming && on_arming)
      on_arming();
    if (arming && defer_arming_receipt) {
      const std::scoped_lock lock{deferred_mutex_};
      deferred_arming_receipt_ = std::move(callback);
      return;
    }
    if (!arming) {
      ++replacement_sends;
      remember_ended(request.message);
      if (defer_replacement_receipt) {
        const std::scoped_lock lock{deferred_mutex_};
        deferred_replacement_receipt_ = std::move(callback);
        return;
      }
    }
    const auto succeeds = arming ? send_succeeds : replacement_send_succeeds;
    if (arming && succeeds)
      remember_visible_status("ARMING");
    callback(
        succeeds
            ? sanguinius::
                  PublicDeliveryReceipt{sanguinius::DeliveryResult::success,
                                        sanguinius::DiscordId{arming ? 900U
                                                                     : 901U}}
            : sanguinius::PublicDeliveryReceipt{
                  sanguinius::DeliveryResult::permanent_failure, std::nullopt});
  }
  void edit_public(const sanguinius::PublicMessageEditRequest &request,
                   sanguinius::DeliveryCallback callback) override {
    log_.push(request.message.embed &&
                      request.message.embed->title.starts_with("LISTENING")
                  ? "public-listening"
                  : "public-ended");
    const auto listening =
        request.message.embed &&
        request.message.embed->title.starts_with("LISTENING");
    if (!listening)
      remember_ended(request.message);
    else if (!defer_listening_receipt)
      remember_visible_status("LISTENING");
    if (listening && on_listening)
      on_listening();
    if (listening && suppress_listening_receipt)
      return;
    if (listening && defer_listening_receipt) {
      const std::scoped_lock lock{deferred_mutex_};
      deferred_listening_receipt_ = std::move(callback);
      return;
    }
    if (!listening && defer_ended_receipt) {
      const std::scoped_lock lock{deferred_mutex_};
      deferred_ended_receipt_ = std::move(callback);
      return;
    }
    callback((listening && !listening_edit_succeeds) ||
                     (!listening && !ended_edit_succeeds)
                 ? sanguinius::DeliveryResult::permanent_failure
                 : sanguinius::DeliveryResult::success);
  }

  bool send_succeeds{true};
  bool replacement_send_succeeds{true};
  bool listening_edit_succeeds{true};
  bool ended_edit_succeeds{true};
  bool suppress_listening_receipt{};
  bool defer_arming_receipt{};
  bool defer_listening_receipt{};
  bool defer_ended_receipt{};
  bool defer_replacement_receipt{};
  std::atomic<std::size_t> replacement_sends{};
  std::function<void()> on_arming;
  std::function<void()> on_listening;

  [[nodiscard]] std::string ended_description() const {
    const std::scoped_lock lock{ended_mutex_};
    return ended_description_;
  }

  void release_listening_receipt(const sanguinius::DeliveryResult result =
                                     sanguinius::DeliveryResult::success) {
    sanguinius::DeliveryCallback callback;
    {
      const std::scoped_lock lock{deferred_mutex_};
      callback = std::move(deferred_listening_receipt_);
    }
    if (!callback)
      return;
    if (result == sanguinius::DeliveryResult::success)
      remember_visible_status("LISTENING");
    callback(result);
  }

  void release_arming_receipt(const sanguinius::DeliveryResult result =
                                  sanguinius::DeliveryResult::success) {
    sanguinius::PublicDeliveryCallback callback;
    {
      const std::scoped_lock lock{deferred_mutex_};
      callback = std::move(deferred_arming_receipt_);
    }
    if (!callback)
      return;
    if (result == sanguinius::DeliveryResult::success)
      remember_visible_status("ARMING");
    callback(
        result == sanguinius::DeliveryResult::success
            ? sanguinius::PublicDeliveryReceipt{result,
                                                sanguinius::DiscordId{900U}}
            : sanguinius::PublicDeliveryReceipt{result, std::nullopt});
  }

  void release_ended_receipt(const sanguinius::DeliveryResult result =
                                  sanguinius::DeliveryResult::success) {
    sanguinius::DeliveryCallback callback;
    {
      const std::scoped_lock lock{deferred_mutex_};
      callback = std::move(deferred_ended_receipt_);
    }
    if (callback)
      callback(result);
  }

  void release_replacement_receipt(const sanguinius::DeliveryResult result =
                                        sanguinius::DeliveryResult::success) {
    sanguinius::PublicDeliveryCallback callback;
    {
      const std::scoped_lock lock{deferred_mutex_};
      callback = std::move(deferred_replacement_receipt_);
    }
    if (!callback)
      return;
    callback(result == sanguinius::DeliveryResult::success
                 ? sanguinius::PublicDeliveryReceipt{
                       result, sanguinius::DiscordId{901U}}
                 : sanguinius::PublicDeliveryReceipt{result, std::nullopt});
  }

  [[nodiscard]] std::string visible_status() const {
    const std::scoped_lock lock{ended_mutex_};
    return visible_status_;
  }

private:
  void remember_ended(const sanguinius::InteractionMessage &message) {
    const std::scoped_lock lock{ended_mutex_};
    ended_description_ = message.embed ? message.embed->description : "";
    visible_status_ = "ENDED";
  }

  void remember_visible_status(std::string status) {
    const std::scoped_lock lock{ended_mutex_};
    visible_status_ = std::move(status);
  }

  OrderingLog &log_;
  mutable std::mutex ended_mutex_;
  mutable std::mutex deferred_mutex_;
  std::string ended_description_;
  std::string visible_status_;
  sanguinius::PublicDeliveryCallback deferred_arming_receipt_;
  sanguinius::PublicDeliveryCallback deferred_replacement_receipt_;
  sanguinius::DeliveryCallback deferred_listening_receipt_;
  sanguinius::DeliveryCallback deferred_ended_receipt_;
};

class OrderingRepository final : public sanguinius::VoiceListeningRepository {
public:
  void record_consent_attestation(bool, sanguinius::DiscordSnowflake,
                                  std::string, std::int64_t) override {}
  [[nodiscard]] sanguinius::VoiceWindowBeginResult
  begin(const sanguinius::VoiceWindowBeginRequest &request,
        const sanguinius::TranscriptionUsagePolicy &) override {
    ++begin_count;
    const std::scoped_lock lock{window_mutex_};
    window_ = request.window;
    return {.code = sanguinius::VoiceWindowBeginCode::created,
            .window = window_};
  }
  [[nodiscard]] std::optional<sanguinius::VoiceListeningWindow>
  active() override {
    const std::scoped_lock lock{window_mutex_};
    return window_;
  }
  [[nodiscard]] std::optional<sanguinius::VoiceListeningWindow>
  transition(const sanguinius::VoiceWindowTransitionRequest &request) override {
    const std::scoped_lock lock{window_mutex_};
    if (!window_ || window_->revision != request.expected_revision ||
        !sanguinius::voice_listening_transition_allowed(window_->state,
                                                        request.target))
      return std::nullopt;
    window_->state = request.target;
    ++window_->revision;
    if (request.target == sanguinius::VoiceListeningState::stopped ||
        request.target == sanguinius::VoiceListeningState::failed ||
        request.target == sanguinius::VoiceListeningState::abandoned ||
        request.target == sanguinius::VoiceListeningState::completed)
      window_->ended_at_ms = request.now_ms;
    return window_;
  }
  [[nodiscard]] std::optional<sanguinius::VoiceListeningWindow>
  complete_transcription(
      const sanguinius::VoiceWindowTransitionRequest &request,
      const sanguinius::VoiceTranscriptionUsage &usage) override {
    if (record_usage_throws)
      throw std::runtime_error{"usage persistence failed"};
    const std::scoped_lock lock{window_mutex_, accounting_mutex_};
    if (complete_transcription_fails || !window_ ||
        window_->revision != request.expected_revision ||
        window_->state != sanguinius::VoiceListeningState::transcribing ||
        request.target != sanguinius::VoiceListeningState::completed ||
        usage.window_id != window_->window_id || !usage.provider_sent ||
        usage.result_code != "completed")
      return std::nullopt;
    window_->state = sanguinius::VoiceListeningState::completed;
    ++window_->revision;
    window_->ended_at_ms = request.now_ms;
    accounting_events_.push_back("usage-sent");
    last_result_code_ = usage.result_code;
    last_provider_sent.store(true);
    last_captured_bytes.store(usage.captured_bytes);
    ++usage_count;
    return window_;
  }
  void record_public_message(std::string_view,
                             sanguinius::DiscordSnowflake message_id,
                             std::int64_t) override {
    const std::scoped_lock lock{window_mutex_};
    if (window_)
      window_->public_message_id = message_id;
  }
  void record_usage(const sanguinius::VoiceTranscriptionUsage &usage) override {
    if (record_usage_throws)
      throw std::runtime_error{"usage persistence failed"};
    {
      const std::scoped_lock lock{accounting_mutex_};
      accounting_events_.push_back(usage.provider_sent ? "usage-sent"
                                                       : "usage-unsent");
      last_result_code_ = usage.result_code;
    }
    last_provider_sent.store(usage.provider_sent);
    last_captured_bytes.store(usage.captured_bytes);
    ++usage_count;
    if (!usage.provider_sent) {
      const std::scoped_lock lock{accounting_mutex_};
      accounting_events_.push_back("release");
      ++releases;
    }
  }
  void record_provider_attempt(const std::string_view window_id,
                               std::int64_t) override {
    if (before_provider_attempt)
      before_provider_attempt(window_id);
    {
      const std::scoped_lock lock{accounting_mutex_};
      accounting_events_.push_back("provider-attempt");
      provider_window_id_ = window_id;
    }
    ++provider_attempts;
  }
  void release_reservation(std::string_view, std::int64_t) override {
    {
      const std::scoped_lock lock{accounting_mutex_};
      accounting_events_.push_back("release");
    }
    ++releases;
  }
  [[nodiscard]] bool kill_switch_enabled() override { return kill_switch; }
  void set_kill_switch(bool enabled, sanguinius::DiscordSnowflake, std::string,
                       std::int64_t) override {
    if (before_kill_switch_change)
      before_kill_switch_change();
    if (kill_switch_change_throws)
      throw std::runtime_error{"kill switch persistence failed"};
    kill_switch.store(enabled);
  }
  [[nodiscard]] std::size_t abandon_nonterminal(std::int64_t, std::string_view,
                                                std::string_view) override {
    return 0;
  }
  [[nodiscard]] sanguinius::VoiceListeningRepositoryHealth
  health(std::int64_t) override {
    const std::scoped_lock lock{window_mutex_};
    return {.active_windows = window_ && !window_->ended_at_ms ? 1U : 0U,
            .day_windows = 0,
            .day_micro_usd = 0,
            .month_micro_usd = 0,
            .kill_switch = kill_switch,
            .last_result_code = std::nullopt};
  }

  [[nodiscard]] std::vector<std::string> accounting_events() const {
    const std::scoped_lock lock{accounting_mutex_};
    return accounting_events_;
  }

  [[nodiscard]] std::optional<sanguinius::VoiceListeningState>
  window_state() const {
    const std::scoped_lock lock{window_mutex_};
    return window_ ? std::optional{window_->state} : std::nullopt;
  }

  [[nodiscard]] std::string last_result_code() const {
    const std::scoped_lock lock{accounting_mutex_};
    return last_result_code_;
  }

  [[nodiscard]] std::string provider_window_id() const {
    const std::scoped_lock lock{accounting_mutex_};
    return provider_window_id_;
  }

  std::atomic<std::size_t> usage_count{};
  std::atomic<std::size_t> releases{};
  std::atomic<std::size_t> begin_count{};
  std::atomic<std::size_t> provider_attempts{};
  std::atomic<bool> last_provider_sent{};
  std::atomic<std::size_t> last_captured_bytes{};
  std::atomic<bool> kill_switch{};
  std::function<void()> before_kill_switch_change;
  std::function<void(std::string_view)> before_provider_attempt;
  bool kill_switch_change_throws{};
  bool record_usage_throws{};
  bool complete_transcription_fails{};

private:
  mutable std::mutex window_mutex_;
  mutable std::mutex accounting_mutex_;
  std::vector<std::string> accounting_events_;
  std::string last_result_code_;
  std::string provider_window_id_;
  std::optional<sanguinius::VoiceListeningWindow> window_;
};

class CountingTranscription final : public sanguinius::TranscriptionClient {
public:
  [[nodiscard]] sanguinius::Transcript
  transcribe(const sanguinius::TranscriptionRequest &, std::stop_token,
             const std::function<void()> &transmission_started) const override {
    ++calls;
    if (mark_transmitted && transmission_started)
      transmission_started();
    return {.text = result_text, .provider_request_id = "request-id"};
  }
  mutable std::atomic<std::size_t> calls{};
  bool mark_transmitted{};
  std::string result_text{"This must not be called on manual stop."};
};

class BlockingTranscription final : public sanguinius::TranscriptionClient {
public:
  [[nodiscard]] sanguinius::Transcript
  transcribe(const sanguinius::TranscriptionRequest &,
             const std::stop_token stop_token,
             const std::function<void()> &transmission_started) const override {
    {
      const std::scoped_lock lock{mutex_};
      entered_ = true;
    }
    changed_.notify_all();
    std::unique_lock lock{mutex_};
    changed_.wait(lock, stop_token, [this] { return released_; });
    if (stop_token.stop_requested())
      throw sanguinius::TranscriptionError{
          sanguinius::TranscriptionFailureCategory::cancelled,
          "blocked transcription cancelled"};
    lock.unlock();
    if (transmission_started)
      transmission_started();
    return {.text = "The provider was released after the transport.",
            .provider_request_id = "request-id"};
  }

  [[nodiscard]] bool
  wait_until_entered(const std::chrono::milliseconds timeout = 2s) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() {
    {
      const std::scoped_lock lock{mutex_};
      released_ = true;
    }
    changed_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable_any changed_;
  mutable bool entered_{};
  mutable bool released_{};
};

class CancellationTransmission final : public sanguinius::TranscriptionClient {
public:
  [[nodiscard]] sanguinius::Transcript
  transcribe(const sanguinius::TranscriptionRequest &,
             std::stop_token stop_token,
             const std::function<void()> &transmission_started) const override {
    entered.store(true);
    while (!stop_token.stop_requested())
      std::this_thread::sleep_for(1ms);
    if (transmission_started)
      transmission_started();
    throw sanguinius::TranscriptionError{
        sanguinius::TranscriptionFailureCategory::cancelled,
        "cancelled after transmission began"};
  }

  mutable std::atomic<bool> entered{};
};

class PreTransmissionCancellation final
    : public sanguinius::TranscriptionClient {
public:
  [[nodiscard]] sanguinius::Transcript
  transcribe(const sanguinius::TranscriptionRequest &,
             std::stop_token stop_token,
             const std::function<void()> &) const override {
    entered.store(true);
    while (!stop_token.stop_requested())
      std::this_thread::sleep_for(1ms);
    throw sanguinius::TranscriptionError{
        sanguinius::TranscriptionFailureCategory::cancelled,
        "cancelled before transmission began"};
  }

  mutable std::atomic<bool> entered{};
};

[[nodiscard]] sanguinius::ServerScopeConfiguration voice_scope() {
  return {.guild_id = sanguinius::DiscordSnowflake{10},
          .primary_channel_id = sanguinius::DiscordSnowflake{20},
          .owner_user_id = sanguinius::DiscordSnowflake{30}};
}

[[nodiscard]] sanguinius::VoiceListeningConfiguration voice_configuration() {
  return {.enabled = true,
          .guild_consent_attested = true,
          .provider_enabled = true,
          .model = std::string{sanguinius::transcription_model},
          .request_timeout = 1s,
          .usage_policy = {},
          .queue_capacity = 4,
          .require_memory_lock = false};
}

[[nodiscard]] std::optional<sanguinius::ActiveVoxListeningContext>
ready_voice_context() {
  return sanguinius::ActiveVoxListeningContext{
      .session_id = "00000000-0000-4000-8000-000000000090",
      .guild_id = sanguinius::DiscordSnowflake{10},
      .text_channel_id = sanguinius::DiscordSnowflake{20},
      .voice_channel_id = sanguinius::DiscordSnowflake{40},
      .connection_generation = 1,
      .ready = true,
      .speech_idle = true};
}

[[nodiscard]] sanguinius::IncomingInteraction
voice_interaction(const std::uint64_t interaction_id = 100,
                  const std::uint64_t user_id = 31) {
  return {.correlation_id = "voice-order",
          .interaction_id = sanguinius::DiscordSnowflake{interaction_id},
          .guild_id = sanguinius::DiscordSnowflake{10},
          .channel_id = sanguinius::DiscordSnowflake{20},
          .user_id = sanguinius::DiscordSnowflake{user_id},
          .username = "requester",
          .display_name = "Requester",
          .kind = sanguinius::InteractionKind::slash_command,
          .command_name = "vox",
          .subcommand_group_name = {},
          .subcommand_name = "listen-start",
          .command_options = {},
          .resolved_users = {},
          .custom_id = {},
          .selected_values = {},
          .modal_fields = {},
          .context_message = std::nullopt,
          .responder = {}};
}

} // namespace

TEST_CASE("voice transcription cost and WAV shape are fixed",
          "[voice-input][transcription][privacy]") {
  REQUIRE(sanguinius::transcription_model == "gpt-transcribe");
  REQUIRE(sanguinius::transcription_endpoint ==
          "https://api.openai.com/v1/audio/transcriptions");
  REQUIRE(sanguinius::estimated_transcription_cost_micro_usd(5) == 375);
  REQUIRE(sanguinius::estimated_transcription_cost_micro_usd(15) == 1'125);
  REQUIRE_THROWS_AS(sanguinius::estimated_transcription_cost_micro_usd(16),
                    std::invalid_argument);

  const auto header = sanguinius::pcm_wav_header(192'000, 48'000, 2, 16);
  REQUIRE(std::to_integer<char>(header[0]) == 'R');
  REQUIRE(std::to_integer<char>(header[8]) == 'W');
  REQUIRE(u32(header, 4) == 192'036);
  REQUIRE(u32(header, 24) == 48'000);
  REQUIRE(u32(header, 28) == 192'000);
  REQUIRE(u32(header, 40) == 192'000);
}

TEST_CASE("secure audio buffer is fixed capacity and observably scrubbed",
          "[voice-input][buffer][privacy]") {
  sanguinius::SecureAudioBuffer buffer{16, false};
  std::array<std::byte, 12> first{};
  first.fill(std::byte{0x5a});
  REQUIRE(buffer.append(first));
  REQUIRE(buffer.size() == first.size());
  REQUIRE_FALSE(buffer.all_zero_for_test());
  std::array<std::byte, 8> overflow{};
  REQUIRE_FALSE(buffer.append(overflow));
  buffer.scrub();
  REQUIRE(buffer.size() == 0);
  REQUIRE(buffer.all_zero_for_test());
}

TEST_CASE("voice shutdown confirms a sequenced private transcript overwrite",
          "[voice-input][interaction][privacy][shutdown][ordering]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "Private shutdown sentinel.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto configuration = voice_configuration();
  configuration.public_status_timeout = 25ms;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), configuration,
      ready_voice_context};

  std::mutex delivery_mutex;
  std::condition_variable delivery_changed;
  std::vector<sanguinius::InteractionMessage> private_edits;
  std::vector<sanguinius::DeliveryCallback> private_receipts;
  const auto editor = std::make_shared<
      sanguinius::interaction_handler_detail::SequencedInteractionEditor>(
      [&](sanguinius::InteractionMessage message,
          sanguinius::DeliveryCallback receipt) {
        {
          const std::scoped_lock lock{delivery_mutex};
          private_edits.push_back(std::move(message));
          private_receipts.push_back(std::move(receipt));
        }
        delivery_changed.notify_all();
      });
  const auto completion_started = std::make_shared<std::atomic_bool>();

  service.start();
  REQUIRE(
      service.listen_start(
          voice_interaction(), 5,
          [editor, completion_started](sanguinius::InteractionMessage message,
                                       sanguinius::DeliveryCallback receipt) {
            editor->submit(std::move(message), std::move(receipt),
                           completion_started->exchange(true));
          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);

  {
    std::unique_lock lock{delivery_mutex};
    REQUIRE(delivery_changed.wait_for(
        lock, 2s, [&private_edits] { return private_edits.size() == 1; }));
    REQUIRE(private_edits.front().content.find("Private shutdown sentinel.") !=
            std::string::npos);
  }

  std::atomic<bool> shutdown_completed{};
  std::jthread shutdown{[&] {
    service.stop();
    shutdown_completed.store(true);
  }};
  std::this_thread::sleep_for(75ms);
  REQUIRE_FALSE(shutdown_completed.load());

  sanguinius::DeliveryCallback transcript_receipt;
  {
    const std::scoped_lock lock{delivery_mutex};
    REQUIRE(private_receipts.size() == 1);
    transcript_receipt = private_receipts.front();
  }
  transcript_receipt(sanguinius::DeliveryResult::success);

  sanguinius::DeliveryCallback overwrite_receipt;
  {
    std::unique_lock lock{delivery_mutex};
    REQUIRE(delivery_changed.wait_for(
        lock, 2s, [&private_edits] { return private_edits.size() == 2; }));
    REQUIRE(private_edits.back().content.find("Private shutdown sentinel.") ==
            std::string::npos);
    REQUIRE(private_edits.back().content.find("No transcript") !=
            std::string::npos);
    overwrite_receipt = private_receipts.back();
  }
  overwrite_receipt(sanguinius::DeliveryResult::success);
  shutdown.join();
  REQUIRE(shutdown_completed.load());
  {
    const std::scoped_lock lock{delivery_mutex};
    REQUIRE(private_edits.size() == 2);
  }
  REQUIRE_FALSE(
      diagnostics.contains_category("voice_input.private_privacy_overwrite"));
}

TEST_CASE("voice state machine permits only bounded forward and terminal paths",
          "[voice-input][state]") {
  using sanguinius::VoiceListeningState;
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::proposed, VoiceListeningState::arming_transport));
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::arming_transport,
      VoiceListeningState::arming_indicator));
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::arming_indicator, VoiceListeningState::active));
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::active, VoiceListeningState::transcribing));
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::transcribing, VoiceListeningState::completed));
  REQUIRE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::active, VoiceListeningState::stopped));
  REQUIRE_FALSE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::active, VoiceListeningState::completed));
  REQUIRE_FALSE(sanguinius::voice_listening_transition_allowed(
      VoiceListeningState::completed, VoiceListeningState::failed));
}

TEST_CASE("transcript validation rejects empty oversized and invalid UTF-8",
          "[voice-input][transcription][privacy]") {
  REQUIRE(sanguinius::valid_transcript_text("The crimson star answers."));
  REQUIRE_FALSE(sanguinius::valid_transcript_text({}));
  REQUIRE_FALSE(sanguinius::valid_transcript_text(std::string(1'801, 'a')));
  REQUIRE_FALSE(sanguinius::valid_transcript_text("\xC0\xAF"));
}

TEST_CASE(
    "voice listening orders public indication transport and privacy abort",
    "[voice-input][lifecycle][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,
      adapter,
      &transcription,
      delivery,
      clock,
      ids,
      diagnostics,
      {.guild_id = sanguinius::DiscordSnowflake{10},
       .primary_channel_id = sanguinius::DiscordSnowflake{20},
       .owner_user_id = sanguinius::DiscordSnowflake{30}},
      {.enabled = true,
       .guild_consent_attested = true,
       .provider_enabled = true,
       .model = std::string{sanguinius::transcription_model},
       .request_timeout = 1s,
       .usage_policy = {},
       .queue_capacity = 4,
       .require_memory_lock = false},
      ready_voice_context,
      nullptr,
      [&log](const bool listening) {
        log.push(listening ? "speech-block" : "speech-release");
      }};
  service.start();

  std::atomic<std::size_t> initiator_completions{};
  sanguinius::IncomingInteraction start{
      .correlation_id = "voice-order",
      .interaction_id = sanguinius::DiscordSnowflake{100},
      .guild_id = sanguinius::DiscordSnowflake{10},
      .channel_id = sanguinius::DiscordSnowflake{20},
      .user_id = sanguinius::DiscordSnowflake{31},
      .username = "requester",
      .display_name = "Requester",
      .kind = sanguinius::InteractionKind::slash_command,
      .command_name = "vox",
      .subcommand_group_name = {},
      .subcommand_name = "listen-start",
      .command_options = {},
      .resolved_users = {},
      .custom_id = {},
      .selected_values = {},
      .modal_fields = {},
      .context_message = std::nullopt,
      .responder = {}};
  REQUIRE(
      service.listen_start(
          start, 5, [&initiator_completions](sanguinius::InteractionMessage) {
            ++initiator_completions;
          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  std::atomic<std::size_t> stop_completions{};
  auto stop = start;
  stop.interaction_id = sanguinius::DiscordSnowflake{101};
  stop.user_id = sanguinius::DiscordSnowflake{32};
  REQUIRE(service.listen_stop(
              stop, [&stop_completions](sanguinius::InteractionMessage) {
                ++stop_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));
  service.stop();

  const auto values = log.values();
  REQUIRE(values.size() >= 9);
  REQUIRE(values[0] == "speech-block");
  REQUIRE(values[1] == "public-arming");
  REQUIRE(values[2] == "undeaf");
  REQUIRE(values[3] == "public-listening");
  REQUIRE(values[4] == "arm");
  REQUIRE(values[5] == "disarm");
  REQUIRE(values[6] == "self-deaf");
  REQUIRE(values[7] == "public-ended");
  REQUIRE(values[8] == "speech-release");
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.releases.load() == 1);
  REQUIRE(repository.usage_count.load() == 1);
}

TEST_CASE("voice listening fails closed at each public indication boundary",
          "[voice-input][lifecycle][privacy][failure]") {
  SECTION("arming delivery fails before transport changes") {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    OrderingDelivery delivery{log};
    delivery.send_succeeds = false;
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    sanguinius::VoiceListeningService service{
        repository,         adapter,       &transcription,
        delivery,           clock,         ids,
        diagnostics,        voice_scope(), voice_configuration(),
        ready_voice_context};
    service.start();
    std::atomic<std::size_t> completions{};
    REQUIRE(
        service.listen_start(voice_interaction(), 5,
                             [&completions](sanguinius::InteractionMessage) {
                               ++completions;
                             }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&completions] { return completions.load() == 1; }));
    service.stop();
    REQUIRE(log.values() == std::vector<std::string>{"public-arming"});
    REQUIRE(transcription.calls.load() == 0);
    REQUIRE(repository.releases.load() == 1);
  }

  SECTION("listening edit fails after transport is restored self-deaf") {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    OrderingDelivery delivery{log};
    delivery.listening_edit_succeeds = false;
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    sanguinius::VoiceListeningService service{
        repository,         adapter,       &transcription,
        delivery,           clock,         ids,
        diagnostics,        voice_scope(), voice_configuration(),
        ready_voice_context};
    service.start();
    std::atomic<std::size_t> completions{};
    REQUIRE(
        service.listen_start(voice_interaction(), 5,
                             [&completions](sanguinius::InteractionMessage) {
                               ++completions;
                             }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&completions] { return completions.load() == 1; }));
    service.stop();
    const auto values = log.values();
    REQUIRE(values.size() >= 4);
    REQUIRE(values[0] == "public-arming");
    REQUIRE(values[1] == "undeaf");
    REQUIRE(values[2] == "public-listening");
    REQUIRE(values[3] == "self-deaf");
    REQUIRE(std::ranges::find(values, "arm") == values.end());
    REQUIRE(transcription.calls.load() == 0);
  }

  SECTION("membership changes after the listening edit but before arm") {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    OrderingDelivery delivery{log};
    delivery.on_listening = [&adapter] {
      adapter.presence.other_bot_present = true;
    };
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    sanguinius::VoiceListeningService service{
        repository,         adapter,       &transcription,
        delivery,           clock,         ids,
        diagnostics,        voice_scope(), voice_configuration(),
        ready_voice_context};
    service.start();
    std::atomic<std::size_t> completions{};
    REQUIRE(
        service.listen_start(voice_interaction(), 5,
                             [&completions](sanguinius::InteractionMessage) {
                               ++completions;
                             }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&completions] { return completions.load() == 1; }));
    service.stop();
    const auto values = log.values();
    REQUIRE(std::ranges::find(values, "public-listening") != values.end());
    REQUIRE(std::ranges::find(values, "arm") != values.end());
    REQUIRE(std::ranges::find(values, "self-deaf") != values.end());
    REQUIRE(std::ranges::find(values, "disarm") == values.end());
    REQUIRE(transcription.calls.load() == 0);
    REQUIRE(repository.releases.load() == 1);
  }
}

TEST_CASE("late arming delivery is repaired after receipt timeout",
          "[voice-input][arming][delivery][timeout][privacy][shutdown]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.defer_arming_receipt = true;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto configuration = voice_configuration();
  configuration.public_status_timeout = 25ms;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), configuration,
      ready_voice_context};
  service.start();

  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  REQUIRE(repository.window_state() == sanguinius::VoiceListeningState::failed);
  REQUIRE(log.values() == std::vector<std::string>{"public-arming"});
  REQUIRE(transcription.calls.load() == 0);

  service.stop();
  delivery.release_arming_receipt();
  REQUIRE(
      eventually([&delivery] { return delivery.visible_status() == "ENDED"; }));
  REQUIRE(log.values() ==
          std::vector<std::string>{"public-arming", "public-ended"});
}

TEST_CASE("voice listening gates requester presence and queued TTS",
          "[voice-input][authorization][tts]") {
  SECTION("requester is not verifiably present") {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    adapter.presence.requester_present = false;
    OrderingDelivery delivery{log};
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    sanguinius::VoiceListeningService service{
        repository,         adapter,       &transcription,
        delivery,           clock,         ids,
        diagnostics,        voice_scope(), voice_configuration(),
        ready_voice_context};
    service.start();
    std::atomic<std::size_t> completions{};
    REQUIRE(
        service.listen_start(voice_interaction(), 5,
                             [&completions](sanguinius::InteractionMessage) {
                               ++completions;
                             }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&completions] { return completions.load() == 1; }));
    service.stop();
    REQUIRE(log.values().empty());
  }

  SECTION("speech is playing or queued") {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    OrderingDelivery delivery{log};
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    const auto busy_context = [] {
      auto context = ready_voice_context();
      context->speech_idle = false;
      return context;
    };
    sanguinius::VoiceListeningService service{
        repository,  adapter,       &transcription,
        delivery,    clock,         ids,
        diagnostics, voice_scope(), voice_configuration(),
        busy_context};
    service.start();
    std::atomic<std::size_t> completions{};
    REQUIRE(
        service.listen_start(voice_interaction(), 5,
                             [&completions](sanguinius::InteractionMessage) {
                               ++completions;
                             }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&completions] { return completions.load() == 1; }));
    service.stop();
    REQUIRE(log.values().empty());
  }
}

TEST_CASE("late listening confirmation cannot extend advertised deadline",
          "[voice-input][deadline][public-status][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.on_listening = [] { std::this_thread::sleep_for(5'100ms); };
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }, 7s));
  service.stop();
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "public-listening") != values.end());
  REQUIRE(std::ranges::find(values, "arm") == values.end());
  REQUIRE(std::ranges::find(values, "self-deaf") != values.end());
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.releases.load() == 1);
}

TEST_CASE("voice membership joins continue but generation changes abort",
          "[voice-input][membership][generation][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  const auto initial_preflights = adapter.preflight_calls.load();
  adapter.presence.human_count = 2;
  adapter.emit_event(sanguinius::VoiceInputEventKind::membership_changed);
  REQUIRE(eventually([&adapter, initial_preflights] {
    return adapter.preflight_calls.load() > initial_preflights;
  }));
  REQUIRE(service.health().state == sanguinius::VoiceListeningState::active);
  adapter.emit_event(sanguinius::VoiceInputEventKind::membership_changed, 2);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  REQUIRE_FALSE(service.health().state.has_value());
  service.stop();
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.releases.load() == 1);
}

TEST_CASE("requester departure aborts active voice capture",
          "[voice-input][membership][requester][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  adapter.emit_event(sanguinius::VoiceInputEventKind::requester_left);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.releases.load() == 1);
}

TEST_CASE("another bot joining is a sticky voice capture violation",
          "[voice-input][membership][bot][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  adapter.emit_event(sanguinius::VoiceInputEventKind::other_bot_joined);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.releases.load() == 1);
}

TEST_CASE("active voice shutdown disarms scrubs and completes privately",
          "[voice-input][shutdown][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::array<std::byte, 1'920> audio{};
  audio.fill(std::byte{0x5a});
  adapter.emit_audio(audio);
  service.stop();
  REQUIRE(completions.load() == 1);
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE(repository.last_captured_bytes.load() == audio.size());
  REQUIRE(repository.releases.load() == 1);
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "disarm") != values.end());
  REQUIRE(std::ranges::find(values, "self-deaf") != values.end());
}

TEST_CASE("voice shutdown cancels arming and completes queued commands",
          "[voice-input][shutdown][queue][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool arming_entered{};
  bool release_arming{};
  delivery.on_arming = [&] {
    std::unique_lock lock{gate_mutex};
    arming_entered = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_arming; });
  };
  auto configuration = voice_configuration();
  configuration.queue_capacity = 1;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), configuration,
      ready_voice_context};
  service.start();

  std::array<std::atomic<std::size_t>, 3> completions{};
  REQUIRE(service.listen_start(voice_interaction(100), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions[0];
                               }) == sanguinius::SubmitResult::accepted);
  {
    std::unique_lock lock{gate_mutex};
    REQUIRE(gate_condition.wait_for(lock, 2s, [&] { return arming_entered; }));
  }
  REQUIRE(service.listen_start(voice_interaction(101), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions[1];
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually(
      [&service] { return service.health().control_queue.active == 2; }));
  REQUIRE(service.listen_start(voice_interaction(102), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions[2];
                               }) == sanguinius::SubmitResult::accepted);

  std::atomic<bool> stop_finished{};
  std::jthread stopper{[&] {
    service.stop();
    stop_finished.store(true);
  }};
  REQUIRE(eventually([&completions] { return completions[2].load() == 1; }));
  {
    const std::scoped_lock lock{gate_mutex};
    release_arming = true;
  }
  gate_condition.notify_all();
  stopper.join();

  REQUIRE(stop_finished.load());
  REQUIRE(completions[0].load() == 1);
  REQUIRE(completions[1].load() == 1);
  REQUIRE(completions[2].load() == 1);
  REQUIRE(transcription.calls.load() == 0);
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "undeaf") == values.end());
  REQUIRE(std::ranges::find(values, "arm") == values.end());
}

TEST_CASE("voice callback overflow fails closed without retaining the frame",
          "[voice-input][buffer][overflow][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::vector<std::byte> oversized(sanguinius::maximum_voice_pcm_bytes + 4U,
                                   std::byte{0x5a});
  adapter.emit_audio(oversized);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.last_captured_bytes.load() == 0);
  REQUIRE(repository.releases.load() == 1);
}

TEST_CASE("voice callback queue saturation uses guaranteed cleanup worker",
          "[voice-input][callback][queue][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool validation_entered{};
  bool release_validation{};
  adapter.on_preflight = [&] {
    std::unique_lock lock{gate_mutex};
    validation_entered = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_validation; });
  };
  adapter.emit_event(sanguinius::VoiceInputEventKind::membership_changed);
  bool callback_blocked{};
  {
    std::unique_lock lock{gate_mutex};
    callback_blocked =
        gate_condition.wait_for(lock, 2s, [&] { return validation_entered; });
  }
  if (!callback_blocked) {
    {
      const std::scoped_lock lock{gate_mutex};
      release_validation = true;
    }
    gate_condition.notify_all();
    service.stop();
    REQUIRE(callback_blocked);
  }

  adapter.emit_event(sanguinius::VoiceInputEventKind::membership_changed);
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  adapter.emit_event(sanguinius::VoiceInputEventKind::connection_changed);
  const auto cleanup_completed =
      eventually([&completions] { return completions.load() == 1; });

  {
    const std::scoped_lock lock{gate_mutex};
    release_validation = true;
  }
  gate_condition.notify_all();
  service.stop();

  REQUIRE(cleanup_completed);
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(repository.last_captured_bytes.load() ==
          5U * sanguinius::maximum_voice_callback_pcm_bytes);
  REQUIRE(repository.releases.load() == 1);
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "disarm") != values.end());
  REQUIRE(std::ranges::find(values, "self-deaf") != values.end());
}

TEST_CASE("voice owner kill immediately disarms and produces no transcript",
          "[voice-input][kill][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::atomic<std::size_t> admin_completions{};
  REQUIRE(service.set_kill_switch(
              voice_interaction(101, 30), true,
              [&admin_completions](sanguinius::InteractionMessage) {
                ++admin_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && admin_completions.load() == 1;
  }));
  service.stop();
  REQUIRE(repository.kill_switch);
  REQUIRE(transcription.calls.load() == 0);
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "disarm") != values.end());
  REQUIRE(std::ranges::find(values, "self-deaf") != values.end());
}

TEST_CASE("voice kill latch rejects starts when durable disable fails",
          "[voice-input][kill][concurrency][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool persistence_entered{};
  bool release_persistence{};
  repository.kill_switch_change_throws = true;
  repository.before_kill_switch_change = [&] {
    std::unique_lock lock{gate_mutex};
    persistence_entered = true;
    gate_condition.notify_all();
    gate_condition.wait(lock, [&] { return release_persistence; });
  };
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> admin_completions{};
  std::jthread disable_thread{[&] {
    static_cast<void>(service.set_kill_switch(
        voice_interaction(101, 30), true,
        [&admin_completions](sanguinius::InteractionMessage) {
          ++admin_completions;
        }));
  }};
  bool entered_in_time{};
  {
    std::unique_lock lock{gate_mutex};
    entered_in_time =
        gate_condition.wait_for(lock, 2s, [&] { return persistence_entered; });
  }
  REQUIRE(service.health().capability ==
          sanguinius::VoiceInputCapability::unavailable_runtime);
  std::atomic<std::size_t> start_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&start_completions](sanguinius::InteractionMessage) {
                ++start_completions;
              }) == sanguinius::SubmitResult::accepted);
  {
    const std::scoped_lock lock{gate_mutex};
    release_persistence = true;
  }
  gate_condition.notify_all();
  disable_thread.join();
  REQUIRE(entered_in_time);
  REQUIRE(eventually([&] {
    return admin_completions.load() == 1 && start_completions.load() == 1;
  }));
  service.stop();
  REQUIRE(repository.begin_count.load() == 0);
  REQUIRE_FALSE(repository.kill_switch.load());
  REQUIRE(service.health().capability ==
          sanguinius::VoiceInputCapability::unavailable_runtime);
  const auto kill_values = log.values();
  REQUIRE(std::ranges::find(kill_values, "arm") == kill_values.end());
}

TEST_CASE("unconfirmed self deaf restoration keeps speech blocked",
          "[voice-input][transport][privacy][speech]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  adapter.disable_succeeds = false;
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  OrderingLog speech_log;
  sanguinius::VoiceListeningService service{
      repository,
      adapter,
      &transcription,
      delivery,
      clock,
      ids,
      diagnostics,
      voice_scope(),
      voice_configuration(),
      ready_voice_context,
      nullptr,
      [&speech_log](const bool listening) {
        speech_log.push(listening ? "speech-block" : "speech-unblock");
      }};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));
  REQUIRE(service.health().capability ==
          sanguinius::VoiceInputCapability::unavailable_runtime);
  service.stop();
  REQUIRE(speech_log.values() == std::vector<std::string>{"speech-block"});
  REQUIRE(diagnostics.contains_category("voice_input.transport_restore"));
}

TEST_CASE("preemptive privacy abort completes without Discord acknowledgement",
          "[voice-input][privacy][stop][defer]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> start_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&start_completions](sanguinius::InteractionMessage) {
                ++start_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::array<std::byte, 1'920> audio{};
  audio.fill(std::byte{0x5a});
  adapter.emit_audio(audio);

  static_cast<void>(
      service.preempt_privacy_abort(sanguinius::DiscordSnowflake{32}, false));
  const auto preempted = log.values();
  REQUIRE_FALSE(preempted.empty());
  REQUIRE(preempted.back() == "disarm");
  REQUIRE(transcription.calls.load() == 0);
  REQUIRE(eventually([&] { return start_completions.load() == 1; }));
  const auto completed = log.values();
  REQUIRE(std::ranges::find(completed, "self-deaf") != completed.end());
  REQUIRE(std::ranges::find(completed, "public-ended") != completed.end());
  service.stop();
  REQUIRE(transcription.calls.load() == 0);
}

TEST_CASE("preempting an unconfirmed listening indicator restores self deaf",
          "[voice-input][privacy][stop][arming][delivery]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.suppress_listening_receipt = true;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "public-listening") != values.end();
  }));

  static_cast<void>(
      service.preempt_privacy_abort(sanguinius::DiscordSnowflake{32}, false));

  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "self-deaf") != values.end();
  }));
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  const auto values = log.values();
  REQUIRE(std::ranges::find(values, "arm") == values.end());
  REQUIRE(std::ranges::find(values, "public-ended-replacement") !=
          values.end());
  REQUIRE(transcription.calls.load() == 0);
  service.stop();
}

TEST_CASE("late listening success is repaired after privacy abort",
          "[voice-input][privacy][stop][arming][delivery][race]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.defer_listening_receipt = true;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "public-listening") != values.end();
  }));

  static_cast<void>(
      service.preempt_privacy_abort(sanguinius::DiscordSnowflake{32}, false));
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "public-ended-replacement") !=
           values.end();
  }));
  REQUIRE(delivery.visible_status() == "ENDED");

  service.stop();
  delivery.release_listening_receipt();
  REQUIRE(
      eventually([&delivery] { return delivery.visible_status() == "ENDED"; }));
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "public-ended") != values.end();
  }));
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  REQUIRE(transcription.calls.load() == 0);
}

TEST_CASE("failed final status edit publishes replacement ended card",
          "[voice-input][public-status][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.ended_edit_succeeds = false;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));
  service.stop();
  REQUIRE(delivery.replacement_sends.load() == 1);
  const auto delivery_values = log.values();
  REQUIRE(std::ranges::find(delivery_values, "public-ended-replacement") !=
          delivery_values.end());
}

TEST_CASE("unconfirmed ended status keeps voice input and speech fail closed",
          "[voice-input][public-status][privacy][speech]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.ended_edit_succeeds = false;
  delivery.replacement_send_succeeds = false;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  OrderingLog speech_log;
  sanguinius::VoiceListeningService service{
      repository,
      adapter,
      &transcription,
      delivery,
      clock,
      ids,
      diagnostics,
      voice_scope(),
      voice_configuration(),
      ready_voice_context,
      nullptr,
      [&speech_log](const bool listening) {
        speech_log.push(listening ? "speech-block" : "speech-release");
      }};
  service.start();

  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));

  REQUIRE(service.health().capability ==
          sanguinius::VoiceInputCapability::unavailable_runtime);
  REQUIRE(speech_log.values() == std::vector<std::string>{"speech-block"});
  REQUIRE(delivery.replacement_sends.load() == 1);
  REQUIRE(diagnostics.contains_category("voice_input.public_status_recovery"));
  service.stop();
  REQUIRE(speech_log.values() == std::vector<std::string>{"speech-block"});
}

TEST_CASE("late ended status receipts restore voice input and speech",
          "[voice-input][public-status][privacy][speech][late-receipt]") {
  const auto exercise = [](const bool release_replacement) {
    OrderingLog log;
    OrderingRepository repository;
    OrderingVoiceAdapter adapter{log};
    OrderingDelivery delivery{log};
    delivery.defer_ended_receipt = true;
    delivery.defer_replacement_receipt = true;
    CountingTranscription transcription;
    sanguinius::test::FakeClock clock;
    sanguinius::test::FakePersistentIdGenerator ids;
    sanguinius::test::FakeDiagnostics diagnostics;
    OrderingLog speech_log;
    auto configuration = voice_configuration();
    configuration.public_status_timeout = 25ms;
    sanguinius::VoiceListeningService service{
        repository,
        adapter,
        &transcription,
        delivery,
        clock,
        ids,
        diagnostics,
        voice_scope(),
        configuration,
        ready_voice_context,
        nullptr,
        [&speech_log](const bool listening) {
          speech_log.push(listening ? "speech-block" : "speech-release");
        }};
    service.start();

    std::atomic<std::size_t> initiator_completions{};
    REQUIRE(service.listen_start(
                voice_interaction(), 5,
                [&initiator_completions](sanguinius::InteractionMessage) {
                  ++initiator_completions;
                }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&log] {
      const auto values = log.values();
      return std::ranges::find(values, "arm") != values.end();
    }));
    std::atomic<std::size_t> stop_completions{};
    REQUIRE(service.listen_stop(
                voice_interaction(102, 32),
                [&stop_completions](sanguinius::InteractionMessage) {
                  ++stop_completions;
                }) == sanguinius::SubmitResult::accepted);
    REQUIRE(eventually([&] {
      return initiator_completions.load() == 1 &&
             stop_completions.load() == 1 &&
             delivery.replacement_sends.load() == 1;
    }));
    REQUIRE(service.health().capability ==
            sanguinius::VoiceInputCapability::unavailable_runtime);
    REQUIRE(speech_log.values() == std::vector<std::string>{"speech-block"});

    if (release_replacement)
      delivery.release_replacement_receipt();
    else
      delivery.release_ended_receipt();
    REQUIRE(eventually([&service] {
      return service.health().capability ==
             sanguinius::VoiceInputCapability::ready;
    }));
    REQUIRE(speech_log.values() ==
            std::vector<std::string>{"speech-block", "speech-release"});

    if (release_replacement)
      delivery.release_ended_receipt();
    else
      delivery.release_replacement_receipt();
    REQUIRE(speech_log.values() ==
            std::vector<std::string>{"speech-block", "speech-release"});
    service.stop();
  };

  exercise(false);
  exercise(true);
}

TEST_CASE("five second PCM limit expires normally without overflow",
          "[voice-input][buffer][boundary][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "Five seconds complete.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually(
      [&initiator_completions] { return initiator_completions.load() == 1; }));
  service.stop();
  REQUIRE(transcription.calls.load() == 1);
  REQUIRE(repository.last_captured_bytes.load() ==
          5U * sanguinius::maximum_voice_callback_pcm_bytes);
  REQUIRE(repository.releases.load() == 0);
}

TEST_CASE("provider attempt identity remains stable during a concurrent stop",
          "[voice-input][transcription][stop][race]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  std::mutex provider_mutex;
  std::condition_variable provider_condition;
  bool provider_entered{};
  bool release_provider{};
  std::string identity_before_stop;
  std::string identity_after_stop;
  repository.before_provider_attempt = [&](const std::string_view window_id) {
    std::unique_lock lock{provider_mutex};
    identity_before_stop = window_id;
    provider_entered = true;
    provider_condition.notify_all();
    provider_condition.wait(lock,
                            [&release_provider] { return release_provider; });
    identity_after_stop = window_id;
  };
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  bool observed_provider{};
  {
    std::unique_lock lock{provider_mutex};
    observed_provider = provider_condition.wait_for(
        lock, 2s, [&provider_entered] { return provider_entered; });
    if (!observed_provider)
      release_provider = true;
  }
  provider_condition.notify_all();
  REQUIRE(observed_provider);

  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(
      eventually([&stop_completions] { return stop_completions.load() == 1; }));
  {
    const std::scoped_lock lock{provider_mutex};
    release_provider = true;
  }
  provider_condition.notify_all();
  REQUIRE(eventually(
      [&repository] { return repository.provider_attempts.load() == 1; }));
  service.stop();
  REQUIRE_FALSE(identity_before_stop.empty());
  REQUIRE(identity_after_stop == identity_before_stop);
  REQUIRE(repository.provider_window_id() == identity_before_stop);
  REQUIRE(initiator_completions.load() == 1);
}

TEST_CASE("stop waits for provider transmission accounting",
          "[voice-input][transcription][stop][accounting][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CancellationTransmission transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  std::mutex initiator_mutex;
  std::string initiator_result;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions, &initiator_mutex,
               &initiator_result](sanguinius::InteractionMessage message) {
                const std::scoped_lock lock{initiator_mutex};
                initiator_result = std::move(message.content);
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::array<std::byte, 1'920> audio{};
  audio.fill(std::byte{0x5a});
  adapter.emit_audio(audio);
  REQUIRE(eventually([&transcription] { return transcription.entered.load(); },
                     7s));
  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));
  service.stop();
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE(repository.last_provider_sent.load());
  REQUIRE(repository.releases.load() == 0);
  {
    const std::scoped_lock lock{initiator_mutex};
    REQUIRE(initiator_result.find("Transcript:") == std::string::npos);
    REQUIRE(initiator_result.find("No transcript") != std::string::npos);
  }
}

TEST_CASE("pre-transmission stop atomically records usage and releases budget",
          "[voice-input][transcription][stop][accounting][billing]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  PreTransmissionCancellation transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually([&transcription] { return transcription.entered.load(); },
                     7s));

  std::atomic<std::size_t> stop_completions{};
  REQUIRE(
      service.listen_stop(voice_interaction(102, 32),
                          [&stop_completions](sanguinius::InteractionMessage) {
                            ++stop_completions;
                          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&] {
    return initiator_completions.load() == 1 && stop_completions.load() == 1;
  }));
  service.stop();

  REQUIRE(repository.provider_attempts.load() == 1);
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE_FALSE(repository.last_provider_sent.load());
  REQUIRE(repository.releases.load() == 1);
  REQUIRE(
      repository.accounting_events() ==
      std::vector<std::string>{"provider-attempt", "usage-unsent", "release"});
}

TEST_CASE("pre-transmission shutdown records unsent usage before release",
          "[voice-input][transcription][shutdown][accounting][billing]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  PreTransmissionCancellation transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> initiator_completions{};
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&initiator_completions](sanguinius::InteractionMessage) {
                ++initiator_completions;
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually([&transcription] { return transcription.entered.load(); },
                     7s));

  service.stop();

  REQUIRE(initiator_completions.load() == 1);
  REQUIRE(repository.provider_attempts.load() == 1);
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE_FALSE(repository.last_provider_sent.load());
  REQUIRE(repository.releases.load() == 1);
  REQUIRE(
      repository.accounting_events() ==
      std::vector<std::string>{"provider-attempt", "usage-unsent", "release"});
}

TEST_CASE("provider transmission reservation survives usage audit failure",
          "[voice-input][transcription][accounting][failure]") {
  OrderingLog log;
  OrderingRepository repository;
  repository.record_usage_throws = true;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();
  REQUIRE(transcription.calls.load() == 1);
  REQUIRE(repository.provider_attempts.load() == 1);
  REQUIRE(repository.usage_count.load() == 0);
  REQUIRE(repository.releases.load() == 0);
}

TEST_CASE("pre-provider usage audit failure releases its reservation",
          "[voice-input][accounting][failure]") {
  OrderingLog log;
  OrderingRepository repository;
  repository.record_usage_throws = true;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  delivery.send_succeeds = false;
  CountingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();

  REQUIRE(repository.provider_attempts.load() == 0);
  REQUIRE(repository.usage_count.load() == 0);
  REQUIRE(repository.releases.load() == 1);
  REQUIRE(diagnostics.contains_category("voice_input.usage_audit"));
  REQUIRE(transcription.calls.load() == 0);
}

TEST_CASE("normal expiry releases speech before provider work completes",
          "[voice-input][expiry][tts][ordering]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  BlockingTranscription transcription;
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,
      adapter,
      &transcription,
      delivery,
      clock,
      ids,
      diagnostics,
      voice_scope(),
      voice_configuration(),
      ready_voice_context,
      nullptr,
      [&log](const bool listening) {
        log.push(listening ? "speech-block" : "speech-release");
      }};
  service.start();

  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x51});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x51}, std::byte{0x51}, std::byte{0x51}, std::byte{0x51}};
  adapter.emit_audio(boundary_frame);

  REQUIRE(transcription.wait_until_entered(7s));
  const auto while_blocked = log.values();
  const auto ended = std::ranges::find(while_blocked, "public-ended");
  const auto released = std::ranges::find(while_blocked, "speech-release");
  REQUIRE(ended != while_blocked.end());
  REQUIRE(released != while_blocked.end());
  REQUIRE(ended < released);
  REQUIRE(repository.window_state() ==
          sanguinius::VoiceListeningState::transcribing);
  REQUIRE(completions.load() == 0);

  transcription.release();
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  service.stop();
  REQUIRE(std::ranges::count(log.values(), "speech-release") == 1);
}

TEST_CASE("normal voice expiry transcribes once and returns only volatile text",
          "[voice-input][expiry][transcription][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = std::string(999, 'a') + "\xE2\x9C\xA8";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();
  std::mutex result_mutex;
  std::optional<sanguinius::InteractionMessage> result;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&result_mutex, &result](sanguinius::InteractionMessage message) {
                const std::scoped_lock lock{result_mutex};
                result = std::move(message);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::array<std::byte, 19'200> audio{};
  audio.fill(std::byte{0x41});
  adapter.emit_audio(audio);
  REQUIRE(eventually(
      [&] {
        const std::scoped_lock lock{result_mutex};
        return result.has_value();
      },
      7s));
  {
    const std::scoped_lock lock{result_mutex};
    REQUIRE(result->content == "Transcript:\n" + transcription.result_text);
    REQUIRE(result->buttons.empty());
  }
  auto modal_interaction = voice_interaction(103, 31);
  modal_interaction.custom_id =
      std::string{sanguinius::voice_transcript_component_prefix} +
      ids.last_issued_id();
  const auto modal = service.transcript_modal(modal_interaction);
  REQUIRE_FALSE(modal.has_value());
  REQUIRE(service.health().volatile_transcript_drafts == 0);
  service.stop();
  REQUIRE(transcription.calls.load() == 1);
  REQUIRE(repository.provider_attempts.load() == 1);
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE(repository.last_provider_sent.load());
  REQUIRE(repository.last_captured_bytes.load() == audio.size());
  REQUIRE(repository.releases.load() == 0);
}

TEST_CASE("completion audit failure cannot retain completed provider usage",
          "[voice-input][transcription][accounting][transaction]") {
  OrderingLog log;
  OrderingRepository repository;
  repository.complete_transcription_fails = true;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "This delivery cannot precommit completion.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();

  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x52});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x52}, std::byte{0x52}, std::byte{0x52}, std::byte{0x52}};
  adapter.emit_audio(boundary_frame);

  REQUIRE(eventually([&service] {
    return service.health().last_failure_category ==
           "completion_transition_failed";
  }));
  REQUIRE(completions.load() == 1);
  REQUIRE(repository.window_state() == sanguinius::VoiceListeningState::failed);
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE(repository.last_result_code() == "completion_transition_failed");
  REQUIRE(std::ranges::count(repository.accounting_events(), "usage-sent") ==
          1);
  service.stop();
}

TEST_CASE("transcription finalization exceptions discard Chronicle drafts",
          "[voice-input][transcription][chronicle][exception][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  repository.record_usage_throws = true;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "This volatile draft must roll back.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeChronicleRepository chronicle_repository;
  sanguinius::ChronicleService chronicle{
      chronicle_repository, clock, ids, voice_scope(), {}, [] {}, [] {}};
  sanguinius::VoiceListeningService service{repository,
                                            adapter,
                                            &transcription,
                                            delivery,
                                            clock,
                                            ids,
                                            diagnostics,
                                            voice_scope(),
                                            voice_configuration(),
                                            ready_voice_context,
                                            &chronicle};
  service.start();

  std::mutex result_mutex;
  std::optional<sanguinius::InteractionMessage> result;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&result_mutex, &result](sanguinius::InteractionMessage message) {
                const std::scoped_lock lock{result_mutex};
                result = std::move(message);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x54});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x54}, std::byte{0x54}, std::byte{0x54}, std::byte{0x54}};
  adapter.emit_audio(boundary_frame);

  REQUIRE(eventually([&service] {
    return service.health().last_failure_category ==
           "transcription_finalize_failed";
  }));
  std::string custom_id;
  {
    const std::scoped_lock lock{result_mutex};
    REQUIRE(result.has_value());
    REQUIRE(result->buttons.size() == 1);
    custom_id = result->buttons.front().custom_id;
  }
  REQUIRE(service.health().volatile_transcript_drafts == 0);
  auto modal_interaction = voice_interaction(103, 31);
  modal_interaction.custom_id = std::move(custom_id);
  REQUIRE_FALSE(service.transcript_modal(modal_interaction).has_value());
  REQUIRE(repository.window_state() == sanguinius::VoiceListeningState::failed);
  service.stop();
}

TEST_CASE("transcript completion waits for confirmed ephemeral delivery",
          "[voice-input][transcription][delivery][privacy][audit]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "A delivery receipt must precede completion.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();

  std::mutex receipt_mutex;
  std::optional<sanguinius::DeliveryCallback> pending_receipt;
  std::string attempted_transcript;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&receipt_mutex, &pending_receipt,
               &attempted_transcript](sanguinius::InteractionMessage message,
                                      sanguinius::DeliveryCallback receipt) {
                const std::scoped_lock lock{receipt_mutex};
                attempted_transcript = std::move(message.content);
                pending_receipt = std::move(receipt);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  REQUIRE(eventually([&service] {
    const auto health = service.health();
    return health.state == sanguinius::VoiceListeningState::active &&
           health.control_queue.active == 0;
  }));

  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);

  REQUIRE(eventually([&] {
    const std::scoped_lock lock{receipt_mutex};
    return pending_receipt.has_value();
  }));
  REQUIRE(repository.window_state() ==
          sanguinius::VoiceListeningState::transcribing);
  REQUIRE(delivery.ended_description().find("delivered") == std::string::npos);

  sanguinius::DeliveryCallback receipt;
  {
    const std::scoped_lock lock{receipt_mutex};
    REQUIRE(attempted_transcript ==
            "Transcript:\n" + transcription.result_text);
    receipt = std::move(*pending_receipt);
    pending_receipt.reset();
  }
  receipt(sanguinius::DeliveryResult::transient_failure);

  REQUIRE(eventually([&] {
    const std::scoped_lock lock{receipt_mutex};
    return pending_receipt.has_value() &&
           attempted_transcript.find(transcription.result_text) ==
               std::string::npos &&
           attempted_transcript.find("No transcript was retained") !=
               std::string::npos;
  }));
  sanguinius::DeliveryCallback terminal_receipt;
  {
    const std::scoped_lock lock{receipt_mutex};
    terminal_receipt = std::move(*pending_receipt);
    pending_receipt.reset();
  }
  terminal_receipt(sanguinius::DeliveryResult::success);

  REQUIRE(eventually([&service] {
    return service.health().last_failure_category ==
           "transcript_delivery_failed";
  }));
  REQUIRE(repository.window_state() == sanguinius::VoiceListeningState::failed);
  REQUIRE(repository.last_result_code() == "transcript_delivery_failed");
  REQUIRE(repository.usage_count.load() == 1);
  REQUIRE(delivery.ended_description().find("did not confirm") !=
          std::string::npos);
  REQUIRE(delivery.ended_description().find("delivered ephemerally") ==
          std::string::npos);
  service.stop();
}

TEST_CASE("unknown transcript delivery is terminally scrubbed before release",
          "[voice-input][transcription][delivery][privacy][ordering]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "Late private delivery sentinel.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  auto configuration = voice_configuration();
  configuration.public_status_timeout = 25ms;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), configuration,
      ready_voice_context};

  std::mutex delivery_mutex;
  std::condition_variable delivery_changed;
  std::vector<sanguinius::InteractionMessage> private_edits;
  std::vector<sanguinius::DeliveryCallback> private_receipts;
  const auto editor = std::make_shared<
      sanguinius::interaction_handler_detail::SequencedInteractionEditor>(
      [&](sanguinius::InteractionMessage message,
          sanguinius::DeliveryCallback receipt) {
        {
          const std::scoped_lock lock{delivery_mutex};
          private_edits.push_back(std::move(message));
          private_receipts.push_back(std::move(receipt));
        }
        delivery_changed.notify_all();
      });
  const auto completion_started = std::make_shared<std::atomic_bool>();

  service.start();
  REQUIRE(
      service.listen_start(
          voice_interaction(), 5,
          [editor, completion_started](sanguinius::InteractionMessage message,
                                       sanguinius::DeliveryCallback receipt) {
            editor->submit(std::move(message), std::move(receipt),
                           completion_started->exchange(true));
          }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);

  {
    std::unique_lock lock{delivery_mutex};
    REQUIRE(delivery_changed.wait_for(
        lock, 2s, [&private_edits] { return private_edits.size() == 1; }));
    REQUIRE(private_edits.front().content.find(transcription.result_text) !=
            std::string::npos);
  }
  REQUIRE(eventually([&repository] {
    return repository.window_state() == sanguinius::VoiceListeningState::failed;
  }));
  {
    const std::scoped_lock lock{delivery_mutex};
    REQUIRE(private_edits.size() == 1);
  }

  sanguinius::DeliveryCallback transcript_receipt;
  {
    const std::scoped_lock lock{delivery_mutex};
    REQUIRE(private_receipts.size() == 1);
    transcript_receipt = private_receipts.front();
  }
  transcript_receipt(sanguinius::DeliveryResult::success);

  sanguinius::DeliveryCallback terminal_receipt;
  {
    std::unique_lock lock{delivery_mutex};
    REQUIRE(delivery_changed.wait_for(
        lock, 2s, [&private_edits] { return private_edits.size() == 2; }));
    REQUIRE(private_edits.back().content.find(transcription.result_text) ==
            std::string::npos);
    REQUIRE(private_edits.back().content.find("No transcript was retained") !=
            std::string::npos);
    terminal_receipt = private_receipts.back();
  }
  terminal_receipt(sanguinius::DeliveryResult::success);

  REQUIRE(
      eventually([&service] { return !service.health().state.has_value(); }));
  REQUIRE(repository.last_result_code() == "transcript_delivery_failed");
  REQUIRE_FALSE(
      diagnostics.contains_category("voice_input.private_privacy_overwrite"));
  service.stop();
}

TEST_CASE("voice kill supersedes pending transcript delivery finalization",
          "[voice-input][transcription][delivery][kill][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "This pending transcript must be discarded.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::VoiceListeningService service{
      repository,         adapter,       &transcription,
      delivery,           clock,         ids,
      diagnostics,        voice_scope(), voice_configuration(),
      ready_voice_context};
  service.start();

  std::mutex completion_mutex;
  std::vector<std::string> completion_messages;
  std::optional<sanguinius::DeliveryCallback> pending_receipt;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&completion_mutex, &completion_messages,
               &pending_receipt](sanguinius::InteractionMessage message,
                                 sanguinius::DeliveryCallback receipt) {
                const std::scoped_lock lock{completion_mutex};
                completion_messages.push_back(std::move(message.content));
                if (receipt)
                  pending_receipt = std::move(receipt);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));

  std::vector<std::byte> one_second_audio(
      sanguinius::maximum_voice_callback_pcm_bytes, std::byte{0x5a});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(one_second_audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}, std::byte{0x5a}};
  adapter.emit_audio(boundary_frame);

  REQUIRE(eventually([&] {
    const std::scoped_lock lock{completion_mutex};
    return pending_receipt.has_value();
  }));
  REQUIRE(repository.window_state() ==
          sanguinius::VoiceListeningState::transcribing);

  std::atomic<std::size_t> admin_completions{};
  std::jthread disable_thread{[&] {
    static_cast<void>(service.set_kill_switch(
        voice_interaction(104, 30), true,
        [&admin_completions](sanguinius::InteractionMessage) {
          ++admin_completions;
        }));
  }};

  REQUIRE(eventually([&] {
    const std::scoped_lock lock{completion_mutex};
    return completion_messages.size() == 2;
  }));
  sanguinius::DeliveryCallback terminal_receipt;
  {
    const std::scoped_lock lock{completion_mutex};
    REQUIRE(pending_receipt.has_value());
    terminal_receipt = std::move(*pending_receipt);
    pending_receipt.reset();
  }
  terminal_receipt(sanguinius::DeliveryResult::success);
  REQUIRE(eventually(
      [&admin_completions] { return admin_completions.load() == 1; }));
  disable_thread.join();

  {
    const std::scoped_lock lock{completion_mutex};
    REQUIRE(completion_messages.front() ==
            "Transcript:\n" + transcription.result_text);
    REQUIRE(completion_messages.back().find("No transcript") !=
            std::string::npos);
  }
  REQUIRE(repository.kill_switch.load());
  REQUIRE(repository.window_state() ==
          sanguinius::VoiceListeningState::stopped);
  REQUIRE(repository.last_result_code() == "stopped_after_send");
  REQUIRE(delivery.ended_description().find("stopped") != std::string::npos);
  service.stop();
}

TEST_CASE("failed transcript modal delivery restores its one-use control",
          "[voice-input][transcription][chronicle][delivery]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "A modal delivery failure may be retried.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeChronicleRepository chronicle_repository;
  sanguinius::ChronicleService chronicle{
      chronicle_repository, clock, ids, voice_scope(), {}, [] {}, [] {}};
  sanguinius::VoiceListeningService service{repository,
                                            adapter,
                                            &transcription,
                                            delivery,
                                            clock,
                                            ids,
                                            diagnostics,
                                            voice_scope(),
                                            voice_configuration(),
                                            ready_voice_context,
                                            &chronicle};
  service.start();

  std::mutex result_mutex;
  std::optional<sanguinius::InteractionMessage> result;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&result_mutex, &result](sanguinius::InteractionMessage message) {
                const std::scoped_lock lock{result_mutex};
                result = std::move(message);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return std::ranges::find(values, "arm") != values.end();
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x53});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x53}, std::byte{0x53}, std::byte{0x53}, std::byte{0x53}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually(
      [&service] { return service.health().volatile_transcript_drafts == 1; }));

  auto modal_interaction = voice_interaction(103, 31);
  {
    const std::scoped_lock lock{result_mutex};
    REQUIRE(result.has_value());
    REQUIRE(result->buttons.size() == 1);
    modal_interaction.custom_id = result->buttons.front().custom_id;
  }
  REQUIRE(service.transcript_modal(modal_interaction).has_value());
  service.transcript_modal_delivery(
      modal_interaction, sanguinius::DeliveryResult::permanent_failure);
  REQUIRE(service.transcript_modal(modal_interaction).has_value());
  service.transcript_modal_delivery(modal_interaction,
                                    sanguinius::DeliveryResult::success);
  REQUIRE_FALSE(service.transcript_modal(modal_interaction).has_value());
  service.stop();
}

TEST_CASE("Chronicle transcript drafts expire proactively and clear on stop",
          "[voice-input][transcription][chronicle][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "A short-lived private transcript.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeChronicleRepository chronicle_repository;
  sanguinius::ChronicleService chronicle{
      chronicle_repository, clock, ids, voice_scope(), {}, [] {}, [] {}};
  auto configuration = voice_configuration();
  configuration.transcript_draft_lifetime = 100ms;
  sanguinius::VoiceListeningService service{repository,     adapter,
                                            &transcription, delivery,
                                            clock,          ids,
                                            diagnostics,    voice_scope(),
                                            configuration,  ready_voice_context,
                                            &chronicle};
  service.start();

  std::mutex result_mutex;
  std::optional<sanguinius::InteractionMessage> result;
  REQUIRE(service.listen_start(
              voice_interaction(), 5,
              [&result_mutex, &result](sanguinius::InteractionMessage message) {
                const std::scoped_lock lock{result_mutex};
                result = std::move(message);
              }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x41});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x41}, std::byte{0x41}, std::byte{0x41}, std::byte{0x41}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually([&] {
    const std::scoped_lock lock{result_mutex};
    return result.has_value();
  }));

  std::string custom_id;
  {
    const std::scoped_lock lock{result_mutex};
    REQUIRE(result->buttons.size() == 1);
    custom_id = result->buttons.front().custom_id;
  }
  REQUIRE(service.health().volatile_transcript_drafts == 1);
  REQUIRE(eventually(
      [&service] { return service.health().volatile_transcript_drafts == 0; },
      2s));
  auto modal_interaction = voice_interaction(103, 31);
  modal_interaction.custom_id = std::move(custom_id);
  REQUIRE_FALSE(service.transcript_modal(modal_interaction).has_value());
  service.stop();
  REQUIRE(service.health().volatile_transcript_drafts == 0);
}

TEST_CASE("service shutdown scrubs unexpired Chronicle transcript drafts",
          "[voice-input][transcription][chronicle][shutdown][privacy]") {
  OrderingLog log;
  OrderingRepository repository;
  OrderingVoiceAdapter adapter{log};
  OrderingDelivery delivery{log};
  CountingTranscription transcription;
  transcription.mark_transmitted = true;
  transcription.result_text = "A private transcript removed at shutdown.";
  sanguinius::test::FakeClock clock;
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::test::FakeDiagnostics diagnostics;
  sanguinius::test::FakeChronicleRepository chronicle_repository;
  sanguinius::ChronicleService chronicle{
      chronicle_repository, clock, ids, voice_scope(), {}, [] {}, [] {}};
  sanguinius::VoiceListeningService service{repository,
                                            adapter,
                                            &transcription,
                                            delivery,
                                            clock,
                                            ids,
                                            diagnostics,
                                            voice_scope(),
                                            voice_configuration(),
                                            ready_voice_context,
                                            &chronicle};
  service.start();

  std::atomic<std::size_t> completions{};
  REQUIRE(service.listen_start(voice_interaction(), 5,
                               [&completions](sanguinius::InteractionMessage) {
                                 ++completions;
                               }) == sanguinius::SubmitResult::accepted);
  REQUIRE(eventually([&log] {
    const auto values = log.values();
    return !values.empty() && values.back() == "arm";
  }));
  std::vector<std::byte> audio(sanguinius::maximum_voice_callback_pcm_bytes,
                               std::byte{0x42});
  for (std::size_t second = 0; second < 5; ++second)
    adapter.emit_audio(audio);
  const std::array<std::byte, 4> boundary_frame{
      std::byte{0x42}, std::byte{0x42}, std::byte{0x42}, std::byte{0x42}};
  adapter.emit_audio(boundary_frame);
  REQUIRE(eventually([&completions] { return completions.load() == 1; }));
  REQUIRE(service.health().volatile_transcript_drafts == 1);
  service.stop();
  REQUIRE(service.health().volatile_transcript_drafts == 0);
}
