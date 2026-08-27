#pragma once

#include "sanguinius/voice_input.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <utility>

namespace sanguinius::test {

class FakeVoiceListeningRepository final : public VoiceListeningRepository {
public:
  void record_consent_attestation(bool, DiscordSnowflake, std::string,
                                  std::int64_t) override {}
  [[nodiscard]] VoiceWindowBeginResult
  begin(const VoiceWindowBeginRequest &,
        const TranscriptionUsagePolicy &) override {
    return {.code = VoiceWindowBeginCode::active_window,
            .window = std::nullopt};
  }
  [[nodiscard]] std::optional<VoiceListeningWindow> active() override {
    return std::nullopt;
  }
  [[nodiscard]] std::optional<VoiceListeningWindow>
  transition(const VoiceWindowTransitionRequest &) override {
    return std::nullopt;
  }
  [[nodiscard]] std::optional<VoiceListeningWindow>
  complete_transcription(const VoiceWindowTransitionRequest &,
                         const VoiceTranscriptionUsage &) override {
    return std::nullopt;
  }
  void record_public_message(std::string_view, DiscordSnowflake,
                             std::int64_t) override {}
  void record_usage(const VoiceTranscriptionUsage &) override {}
  void record_provider_attempt(std::string_view, std::int64_t) override {}
  void release_reservation(std::string_view, std::int64_t) override {}
  [[nodiscard]] bool kill_switch_enabled() override {
    return kill_switch.load();
  }
  void set_kill_switch(bool enabled, DiscordSnowflake, std::string,
                       std::int64_t) override {
    if (before_kill_switch_change)
      before_kill_switch_change(enabled);
    {
      const std::scoped_lock lock{kill_switch_mutex};
      kill_switch.store(enabled);
      ++kill_switch_changes;
    }
    kill_switch_changed.notify_all();
  }
  [[nodiscard]] std::size_t abandon_nonterminal(std::int64_t, std::string_view,
                                                std::string_view) override {
    ++abandon_calls;
    return abandoned_windows;
  }
  [[nodiscard]] VoiceListeningRepositoryHealth health(std::int64_t) override {
    return {};
  }

  [[nodiscard]] bool wait_for_kill_switch_changes(
      const std::size_t expected,
      const std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
    std::unique_lock lock{kill_switch_mutex};
    return kill_switch_changed.wait_for(lock, timeout, [this, expected] {
      return kill_switch_changes.load() >= expected;
    });
  }

  std::atomic<std::size_t> abandon_calls{};
  std::atomic<std::size_t> kill_switch_changes{};
  std::atomic<bool> kill_switch{};
  std::function<void(bool)> before_kill_switch_change;
  std::size_t abandoned_windows{};

private:
  std::mutex kill_switch_mutex;
  std::condition_variable kill_switch_changed;
};

class FakeVoiceInputAdapter final : public VoiceInputAdapter {
public:
  [[nodiscard]] VoiceInputCapability capability() const noexcept override {
    return VoiceInputCapability::disabled;
  }
  void start(AudioCallback audio, EventCallback events) override {
    audio_ = std::move(audio);
    events_ = std::move(events);
  }
  [[nodiscard]] VoiceInputPresence
  preflight(const VoiceInputArmRequest &) const override {
    return {};
  }
  [[nodiscard]] bool enable_transport(const VoiceInputArmRequest &,
                                      std::stop_token) override {
    return false;
  }
  [[nodiscard]] bool arm(const VoiceInputArmRequest &) override {
    return false;
  }
  void disarm() noexcept override { ++disarms; }
  [[nodiscard]] bool disable_transport() noexcept override { return true; }
  void shutdown() noexcept override {
    try {
      if (on_shutdown)
        on_shutdown();
    } catch (...) {
    }
  }

  std::atomic<std::size_t> disarms{};
  std::function<void()> on_shutdown;

private:
  AudioCallback audio_;
  EventCallback events_;
};

} // namespace sanguinius::test
