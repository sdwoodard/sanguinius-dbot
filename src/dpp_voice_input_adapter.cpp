#include "sanguinius/dpp_voice_input_adapter.hpp"

#include "sanguinius/callback_fence.hpp"
#include "sanguinius/dpp_cluster_host.hpp"

#include <dpp/dpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] dpp::cluster &
require_cluster(const std::shared_ptr<DppClusterHost> &host) {
  if (!host)
    throw std::invalid_argument{"D++ voice input requires a cluster host."};
  return host->native();
}

[[nodiscard]] bool matches(const VoiceInputArmRequest &left,
                           const VoiceInputArmRequest &right) noexcept {
  return left.session_id == right.session_id &&
         left.guild_id == right.guild_id &&
         left.voice_channel_id == right.voice_channel_id &&
         left.requester_user_id == right.requester_user_id &&
         left.generation == right.generation;
}

} // namespace

namespace dpp_voice_input_adapter_detail {

ReceiveClientDisposition
classify_receive_client(const void *expected_client,
                        const void *received_client,
                        const DiscordSnowflake expected_guild_id,
                        const DiscordSnowflake received_guild_id) noexcept {
  if (expected_client == nullptr)
    return ReceiveClientDisposition::ignore;
  if (received_client == expected_client)
    return ReceiveClientDisposition::accept;
  if (received_client == nullptr ||
      (expected_guild_id.is_set() && expected_guild_id == received_guild_id))
    return ReceiveClientDisposition::connection_changed;
  return ReceiveClientDisposition::ignore;
}

} // namespace dpp_voice_input_adapter_detail

class DppVoiceInputAdapter::Impl {
public:
  Impl(std::shared_ptr<DppClusterHost> host, VoiceGateway &gateway,
       Diagnostics &diagnostics, const bool configured_enabled)
      : host_{std::move(host)}, bot_{require_cluster(host_)}, gateway_{gateway},
        diagnostics_{diagnostics}, configured_enabled_{configured_enabled},
        callbacks_{std::make_shared<CallbackFence>()} {}

  VoiceInputCapability capability() const noexcept {
    if (!configured_enabled_)
      return VoiceInputCapability::disabled;
    if (privacy_blocked_.load())
      return VoiceInputCapability::unavailable_runtime;
#if defined(SANGUINIUS_HAVE_DPP_VOICE_RECEIVE) &&                              \
    SANGUINIUS_HAVE_DPP_VOICE_RECEIVE
    return dpp::utility::has_voice() ? VoiceInputCapability::ready
                                     : VoiceInputCapability::unsupported_build;
#else
    return VoiceInputCapability::unsupported_build;
#endif
  }

  void start(AudioCallback audio_callback, EventCallback event_callback) {
    if (!audio_callback || !event_callback || started_.exchange(true))
      throw std::logic_error{"D++ voice input may start only once."};
    audio_callback_ = std::move(audio_callback);
    event_callback_ = std::move(event_callback);
#if defined(SANGUINIUS_HAVE_DPP_VOICE_RECEIVE) &&                              \
    SANGUINIUS_HAVE_DPP_VOICE_RECEIVE
    receive_handle_ = bot_.on_voice_receive_combined(
        [this, fence = callbacks_](const dpp::voice_receive_t &event) {
          static_cast<void>(fence->invoke([this, &event] { receive(event); }));
        });
    voice_state_handle_ = bot_.on_voice_state_update(
        [this, fence = callbacks_](const dpp::voice_state_update_t &event) {
          static_cast<void>(
              fence->invoke([this, &event] { voice_state_changed(event); }));
        });
#endif
  }

  VoiceInputPresence preflight(const VoiceInputArmRequest &request) const {
    if (capability() != VoiceInputCapability::ready ||
        request.session_id.empty() || !request.guild_id.is_set() ||
        !request.voice_channel_id.is_set() ||
        !request.requester_user_id.is_set() || request.generation == 0)
      return {};
    const auto snapshot = gateway_.snapshot(request.session_id);
    if (!snapshot.bound || !snapshot.connected || !snapshot.ready ||
        !snapshot.dave_active || snapshot.bot_moved ||
        snapshot.guild_id != request.guild_id ||
        snapshot.channel_id != request.voice_channel_id ||
        snapshot.observed_channel_id != request.voice_channel_id ||
        snapshot.generation != request.generation)
      return {};
    auto *channel =
        dpp::find_channel(dpp::snowflake{request.voice_channel_id.value()});
    if (channel == nullptr ||
        channel->guild_id != dpp::snowflake{request.guild_id.value()})
      return {};
    bool requester_present{};
    bool other_bot{};
    std::size_t humans{};
    for (const auto &[user_id, state] : channel->get_voice_members()) {
      static_cast<void>(state);
      if (user_id == bot_.me.id)
        continue;
      const auto *user = dpp::find_user(user_id);
      if (user == nullptr)
        return {};
      if (user_id == dpp::snowflake{request.requester_user_id.value()})
        requester_present = true;
      if (user->is_bot())
        other_bot = true;
      else
        ++humans;
    }
    return {.available = true,
            .requester_present = requester_present,
            .other_bot_present = other_bot,
            .human_count = humans};
  }

  bool enable_transport(const VoiceInputArmRequest &request,
                        const std::stop_token stop_token) {
    if (stop_token.stop_requested() || !preflight(request).available)
      return false;
    {
      const std::scoped_lock lock{mutex_};
      expected_ = request;
      armed_ = false;
      transport_target_self_deaf_ = false;
      transport_state_observed_ = false;
      transport_state_failed_ = false;
      transport_privacy_safe_ = false;
    }
    if (gateway_.set_receive_enabled(request.session_id, true) !=
        VoiceGatewaySubmit::accepted) {
      const std::scoped_lock lock{mutex_};
      transport_target_self_deaf_.reset();
      return false;
    }
    std::unique_lock lock{mutex_};
    const auto confirmed = transport_condition_.wait_for(
        lock, stop_token, std::chrono::seconds{5}, [this] {
          return transport_state_observed_ || transport_state_failed_ ||
                 shutdown_.load();
        });
    transport_target_self_deaf_.reset();
    return confirmed && transport_state_observed_ && !transport_state_failed_ &&
           !shutdown_.load();
  }

  bool arm(const VoiceInputArmRequest &request) {
    const auto presence = preflight(request);
    if (!presence.available || !presence.requester_present ||
        presence.other_bot_present || presence.human_count == 0)
      return false;
    const std::scoped_lock lock{mutex_};
    if (!expected_ || !matches(*expected_, request))
      return false;
    expected_voice_client_ = current_voice_client(request.guild_id);
    if (expected_voice_client_ == nullptr)
      return false;
    armed_ = true;
    return true;
  }

  void disarm() noexcept {
    const std::scoped_lock lock{mutex_};
    armed_ = false;
    expected_voice_client_ = nullptr;
  }

  bool disable_transport() noexcept {
    std::optional<VoiceInputArmRequest> request;
    {
      const std::scoped_lock lock{mutex_};
      armed_ = false;
      expected_voice_client_ = nullptr;
      request = expected_;
      if (request) {
        if (transport_privacy_safe_) {
          transport_target_self_deaf_.reset();
          expected_.reset();
          privacy_blocked_.store(false);
          return true;
        }
        transport_target_self_deaf_ = true;
        transport_state_observed_ = false;
        transport_state_failed_ = false;
      }
    }
    if (!request)
      return !privacy_blocked_.load();
    const auto submitted =
        gateway_.set_receive_enabled(request->session_id, false) ==
        VoiceGatewaySubmit::accepted;
    bool restored{};
    if (submitted && !shutdown_.load()) {
      std::unique_lock lock{mutex_};
      restored =
          transport_condition_.wait_for(lock, std::chrono::seconds{5},
                                        [this] {
                                          return transport_state_observed_ ||
                                                 transport_state_failed_ ||
                                                 shutdown_.load();
                                        }) &&
          transport_state_observed_ && !transport_state_failed_;
    }
    bool disconnected{};
    {
      const std::scoped_lock lock{mutex_};
      disconnected = transport_privacy_safe_ && !transport_state_observed_;
    }
    if (!restored && !disconnected && !shutdown_.load()) {
      const auto disconnect_submitted =
          gateway_.disconnect(request->session_id) ==
          VoiceGatewaySubmit::accepted;
      if (disconnect_submitted) {
        std::unique_lock lock{mutex_};
        disconnected = transport_condition_.wait_for(
                           lock, std::chrono::seconds{5},
                           [this] {
                             return transport_privacy_safe_ || shutdown_.load();
                           }) &&
                       transport_privacy_safe_;
      }
    }
    const auto privacy_safe = restored || disconnected;
    {
      const std::scoped_lock lock{mutex_};
      if (privacy_safe) {
        transport_target_self_deaf_.reset();
        expected_.reset();
      }
    }
    privacy_blocked_.store(!privacy_safe && !shutdown_.load());
    if (!privacy_safe && !shutdown_.load()) {
      try {
        diagnostics_.emit(
            {DiagnosticSeverity::warning, "voice_input.transport_restore",
             "Voice input could not confirm self-deaf transport or a forced "
             "disconnect; speech remains blocked.",
             std::nullopt});
      } catch (...) {
      }
    }
    return privacy_safe || shutdown_.load();
  }

  void shutdown() noexcept {
    if (shutdown_.exchange(true))
      return;
    transport_condition_.notify_all();
    disarm();
    static_cast<void>(disable_transport());
    callbacks_->close_and_wait();
#if defined(SANGUINIUS_HAVE_DPP_VOICE_RECEIVE) &&                              \
    SANGUINIUS_HAVE_DPP_VOICE_RECEIVE
    if (receive_handle_ != 0)
      static_cast<void>(bot_.on_voice_receive_combined.detach(receive_handle_));
    if (voice_state_handle_ != 0)
      static_cast<void>(bot_.on_voice_state_update.detach(voice_state_handle_));
#endif
  }

private:
  [[nodiscard]] dpp::discord_voice_client *
  current_voice_client(const DiscordSnowflake guild_id) const {
    auto *guild = dpp::find_guild(dpp::snowflake{guild_id.value()});
    if (guild == nullptr)
      return nullptr;
    auto *shard = bot_.get_shard(guild->shard_id);
    if (shard == nullptr)
      return nullptr;
    auto *connection = shard->get_voice(dpp::snowflake{guild_id.value()});
    return connection == nullptr ? nullptr : connection->voiceclient.get();
  }

  void receive(const dpp::voice_receive_t &event) noexcept {
    VoiceInputArmRequest request;
    AudioCallback callback;
    bool connection_changed{};
    {
      const std::scoped_lock lock{mutex_};
      if (!armed_ || !expected_ || expected_voice_client_ == nullptr)
        return;
      DiscordSnowflake received_guild_id;
      if (event.voice_client != nullptr) {
        const dpp::snowflake &server_id = event.voice_client->server_id;
        received_guild_id =
            DiscordSnowflake{static_cast<std::uint64_t>(server_id)};
      }
      const auto disposition =
          dpp_voice_input_adapter_detail::classify_receive_client(
              expected_voice_client_, event.voice_client, expected_->guild_id,
              received_guild_id);
      if (disposition ==
          dpp_voice_input_adapter_detail::ReceiveClientDisposition::ignore)
        return;
      request = *expected_;
      if (disposition == dpp_voice_input_adapter_detail::
                             ReceiveClientDisposition::connection_changed) {
        armed_ = false;
        expected_voice_client_ = nullptr;
        connection_changed = true;
      } else {
        if (event.audio_data.empty())
          return;
        callback = audio_callback_;
      }
    }
    if (connection_changed) {
      notify_event(VoiceInputEventKind::connection_changed, request);
      return;
    }
    try {
      callback(request.session_id, request.generation,
               std::as_bytes(std::span<const std::uint8_t>{
                   event.audio_data.data(), event.audio_data.size()}));
    } catch (...) {
    }
  }

  void voice_state_changed(const dpp::voice_state_update_t &event) noexcept {
    if (event.state.user_id == bot_.me.id) {
      VoiceInputEventKind event_kind{VoiceInputEventKind::membership_changed};
      VoiceInputArmRequest request;
      bool notify{};
      {
        const std::scoped_lock lock{mutex_};
        if (!expected_ ||
            event.state.guild_id != dpp::snowflake{expected_->guild_id.value()})
          return;
        if (transport_target_self_deaf_) {
          if (event.state.channel_id == 0 ||
              event.state.channel_id !=
                  dpp::snowflake{expected_->voice_channel_id.value()}) {
            transport_state_failed_ = true;
            if (event.state.channel_id == 0) {
              transport_privacy_safe_ = true;
              privacy_blocked_.store(false);
            }
          } else if (event.state.is_self_deaf() ==
                     *transport_target_self_deaf_) {
            transport_state_observed_ = true;
            if (*transport_target_self_deaf_) {
              transport_privacy_safe_ = true;
              privacy_blocked_.store(false);
            }
          }
          transport_condition_.notify_all();
          return;
        }
        notify = armed_;
        request = *expected_;
        if (event.state.channel_id == 0 ||
            event.state.channel_id !=
                dpp::snowflake{expected_->voice_channel_id.value()})
          event_kind = VoiceInputEventKind::connection_changed;
      }
      if (notify)
        notify_event(event_kind, request);
      return;
    }

    VoiceInputArmRequest request;
    {
      const std::scoped_lock lock{mutex_};
      if (!expected_ ||
          event.state.guild_id != dpp::snowflake{expected_->guild_id.value()})
        return;
      request = *expected_;
    }
    const auto expected_channel =
        dpp::snowflake{request.voice_channel_id.value()};
    const auto requester = dpp::snowflake{request.requester_user_id.value()};
    if (event.state.user_id == requester &&
        event.state.channel_id != expected_channel) {
      notify_event(VoiceInputEventKind::requester_left, request);
      return;
    }
    if (event.state.channel_id == expected_channel) {
      const auto *user = dpp::find_user(event.state.user_id);
      if (user == nullptr) {
        notify_event(VoiceInputEventKind::unavailable, request);
        return;
      }
      if (user->is_bot()) {
        notify_event(VoiceInputEventKind::other_bot_joined, request);
        return;
      }
    }
    notify_event(VoiceInputEventKind::membership_changed, request);
  }

  void notify_event(const VoiceInputEventKind kind,
                    const VoiceInputArmRequest &request) noexcept {
    EventCallback callback;
    {
      const std::scoped_lock lock{mutex_};
      if (!expected_ || !matches(*expected_, request))
        return;
      callback = event_callback_;
    }
    try {
      callback({.kind = kind,
                .session_id = request.session_id,
                .generation = request.generation,
                .human_count = 0});
    } catch (...) {
    }
  }

  std::shared_ptr<DppClusterHost> host_;
  dpp::cluster &bot_;
  VoiceGateway &gateway_;
  Diagnostics &diagnostics_;
  bool configured_enabled_{};
  std::shared_ptr<CallbackFence> callbacks_;
  mutable std::mutex mutex_;
  std::condition_variable_any transport_condition_;
  AudioCallback audio_callback_;
  EventCallback event_callback_;
  std::optional<VoiceInputArmRequest> expected_;
  dpp::discord_voice_client *expected_voice_client_{};
  std::optional<bool> transport_target_self_deaf_;
  bool transport_state_observed_{};
  bool transport_state_failed_{};
  bool transport_privacy_safe_{};
  bool armed_{};
  dpp::event_handle receive_handle_{};
  dpp::event_handle voice_state_handle_{};
  std::atomic<bool> started_{false};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> privacy_blocked_{false};
};

DppVoiceInputAdapter::DppVoiceInputAdapter(std::shared_ptr<DppClusterHost> host,
                                           VoiceGateway &gateway,
                                           Diagnostics &diagnostics,
                                           const bool configured_enabled)
    : impl_{std::make_unique<Impl>(std::move(host), gateway, diagnostics,
                                   configured_enabled)} {}

DppVoiceInputAdapter::~DppVoiceInputAdapter() { shutdown(); }
VoiceInputCapability DppVoiceInputAdapter::capability() const noexcept {
  return impl_->capability();
}
void DppVoiceInputAdapter::start(AudioCallback audio, EventCallback event) {
  impl_->start(std::move(audio), std::move(event));
}
VoiceInputPresence
DppVoiceInputAdapter::preflight(const VoiceInputArmRequest &request) const {
  return impl_->preflight(request);
}
bool DppVoiceInputAdapter::enable_transport(const VoiceInputArmRequest &request,
                                            const std::stop_token stop_token) {
  return impl_->enable_transport(request, stop_token);
}
bool DppVoiceInputAdapter::arm(const VoiceInputArmRequest &request) {
  return impl_->arm(request);
}
void DppVoiceInputAdapter::disarm() noexcept { impl_->disarm(); }
bool DppVoiceInputAdapter::disable_transport() noexcept {
  return impl_->disable_transport();
}
void DppVoiceInputAdapter::shutdown() noexcept { impl_->shutdown(); }

} // namespace sanguinius
