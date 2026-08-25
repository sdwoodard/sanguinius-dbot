#include "sanguinius/dpp_voice_gateway.hpp"

#include "sanguinius/callback_fence.hpp"
#include "sanguinius/dpp_cluster_host.hpp"

#include <dpp/dpp.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] DiscordSnowflake id(const dpp::snowflake value) {
  return DiscordSnowflake{static_cast<std::uint64_t>(value)};
}

[[nodiscard]] dpp::cluster &
require_cluster(const std::shared_ptr<DppClusterHost> &cluster_host) {
  if (!cluster_host)
    throw std::invalid_argument{"D++ voice gateway requires a cluster host."};
  return cluster_host->native();
}

template <typename Callback>
void fenced(const std::shared_ptr<CallbackFence> &callbacks,
            Callback &&callback) noexcept {
  try {
    static_cast<void>(callbacks->invoke(
        std::function<void()>{std::forward<Callback>(callback)}));
  } catch (...) {
  }
}

[[nodiscard]] std::size_t human_count(dpp::channel &channel,
                                      const dpp::snowflake bot_id) {
  std::size_t result = 0;
  for (const auto &[user_id, state] : channel.get_voice_members()) {
    static_cast<void>(state);
    if (user_id == bot_id)
      continue;
    const auto *user = dpp::find_user(user_id);
    if (user == nullptr || !user->is_bot())
      ++result;
  }
  return result;
}

[[nodiscard]] VoiceResolvedChannel
resolved_channel(const dpp::guild &guild, dpp::channel &channel,
                 const dpp::guild_member &bot_member,
                 const dpp::snowflake bot_id) {
  if (!channel.is_voice_channel() || channel.is_stage_channel())
    return {.status = VoiceResolveStatus::unsupported_channel,
            .channel_id = {},
            .human_count = 0,
            .failure_category = "unsupported_channel"};
  const auto permissions = guild.permission_overwrites(bot_member, channel);
  if (!permissions.has(dpp::p_view_channel, dpp::p_connect, dpp::p_speak))
    return {.status = VoiceResolveStatus::permission_denied,
            .channel_id = {},
            .human_count = 0,
            .failure_category = "missing_voice_permission"};
  const auto members = channel.get_voice_members();
  if (channel.user_limit != 0 && members.size() >= channel.user_limit &&
      !permissions.has(dpp::p_move_members))
    return {.status = VoiceResolveStatus::channel_full,
            .channel_id = {},
            .human_count = 0,
            .failure_category = "channel_full"};
  return {.status = VoiceResolveStatus::ready,
          .channel_id = id(channel.id),
          .human_count = human_count(channel, bot_id),
          .failure_category = {}};
}

struct CachedResolution {
  dpp::guild *guild{};
  dpp::channel *channel{};
  dpp::guild_member *bot_member{};
  VoiceResolvedChannel failure;
};

[[nodiscard]] CachedResolution
resolve_cache(const DiscordSnowflake guild_id,
              const DiscordSnowflake member_user_id,
              const dpp::snowflake bot_id) {
  auto *guild = dpp::find_guild(dpp::snowflake{guild_id.value()});
  if (guild == nullptr)
    return {.failure = {.status = VoiceResolveStatus::unavailable,
                        .channel_id = {},
                        .human_count = 0,
                        .failure_category = "guild_cache_miss"}};
  const auto voice =
      guild->voice_members.find(dpp::snowflake{member_user_id.value()});
  if (voice == guild->voice_members.end() || voice->second.channel_id == 0)
    return {.guild = guild,
            .channel = nullptr,
            .bot_member = nullptr,
            .failure = {.status = VoiceResolveStatus::no_voice,
                        .channel_id = {},
                        .human_count = 0,
                        .failure_category = "member_not_in_voice"}};
  auto *channel = dpp::find_channel(voice->second.channel_id);
  if (channel == nullptr || channel->guild_id != guild->id)
    return {.guild = guild,
            .channel = nullptr,
            .bot_member = nullptr,
            .failure = {.status = VoiceResolveStatus::unavailable,
                        .channel_id = {},
                        .human_count = 0,
                        .failure_category = "voice_channel_cache_miss"}};
  auto member = guild->members.find(bot_id);
  return {.guild = guild,
          .channel = channel,
          .bot_member =
              member == guild->members.end() ? nullptr : &member->second,
          .failure = {.status = VoiceResolveStatus::unavailable,
                      .channel_id = {},
                      .human_count = 0,
                      .failure_category = "bot_member_cache_miss"}};
}

} // namespace

VoiceEvent dpp_voice_gateway_detail::translate_ready(
    const VoiceGatewaySnapshot &binding,
    const DiscordSnowflake observed_channel_id, const bool client_ready,
    const bool dave_active) {
  VoiceEvent event{.kind = VoiceEventKind::ready,
                   .session_id = binding.session_id,
                   .guild_id = binding.guild_id,
                   .channel_id = binding.channel_id,
                   .generation = binding.generation,
                   .human_count = binding.human_count,
                   .dave_active = dave_active,
                   .marker = {},
                   .failure_category = {}};
  if (observed_channel_id != binding.channel_id) {
    event.kind = VoiceEventKind::bot_moved;
    event.channel_id = observed_channel_id;
    event.dave_active = false;
  } else if (!client_ready) {
    event.kind = VoiceEventKind::error;
    event.dave_active = false;
    event.failure_category = "voice_not_ready";
  }
  return event;
}

bool dpp_voice_gateway_detail::may_replace_binding(
    const VoiceGatewaySnapshot &binding,
    const std::string_view session_id) noexcept {
  return !binding.bound || binding.session_id == session_id ||
         !binding.connected;
}

bool dpp_voice_gateway_detail::matches_voice_session(
    const std::string_view expected_session_id,
    const std::string_view observed_session_id) noexcept {
  return !expected_session_id.empty() &&
         expected_session_id == observed_session_id;
}

bool dpp_voice_gateway_detail::matches_voice_client(
    const void *const current_voice_client,
    const void *const observed_voice_client) noexcept {
  return current_voice_client != nullptr &&
         current_voice_client == observed_voice_client;
}

class DppVoiceGateway::Impl {
public:
  struct Binding {
    std::string session_id;
    DiscordSnowflake guild_id;
    DiscordSnowflake channel_id;
    DiscordSnowflake observed_channel_id;
    DiscordSnowflake member_user_id;
    std::string discord_session_id;
    std::uint64_t generation{};
    std::uint32_t shard_id{};
    std::size_t human_count{};
    bool connected{};
    bool ready{};
    bool dave_active{};
    bool marker_completed{};
    bool bot_moved{};
    std::string expected_marker;
  };

  Impl(std::shared_ptr<DppClusterHost> cluster_host, Diagnostics &diagnostics)
      : cluster_host_{std::move(cluster_host)},
        bot_{require_cluster(cluster_host_)}, diagnostics_{diagnostics},
        callbacks_{std::make_shared<CallbackFence>()} {
    if ((cluster_host_->intents() & dpp::i_guild_voice_states) == 0U)
      throw std::invalid_argument{
          "D++ voice gateway requires the Guild Voice States intent."};
    if (!dpp::utility::has_voice())
      throw std::invalid_argument{
          "D++ was built without required voice support."};
  }

  void start(EventCallback callback) {
    if (!callback || started_.exchange(true))
      throw std::logic_error{"D++ voice gateway may only be started once."};
    event_callback_ = std::move(callback);
    voice_ready_handle_ = bot_.on_voice_ready(
        [this, callbacks = callbacks_](const dpp::voice_ready_t &event) {
          fenced(callbacks, [this, &event] { on_ready(event); });
        });
    voice_state_handle_ = bot_.on_voice_state_update(
        [this, callbacks = callbacks_](const dpp::voice_state_update_t &event) {
          fenced(callbacks, [this, &event] { on_voice_state(event); });
        });
    marker_handle_ = bot_.on_voice_track_marker(
        [this, callbacks = callbacks_](const dpp::voice_track_marker_t &event) {
          fenced(callbacks, [this, &event] { on_marker(event); });
        });
  }

  void resolve_member_channel(const DiscordSnowflake guild_id,
                              const DiscordSnowflake user_id,
                              ResolveCallback callback) {
    if (!callback || shutdown_.load())
      return;
    const auto cached = resolve_cache(guild_id, user_id, bot_.me.id);
    if (cached.channel == nullptr || cached.guild == nullptr) {
      callback(cached.failure);
      return;
    }
    if (cached.bot_member != nullptr) {
      callback(resolved_channel(*cached.guild, *cached.channel,
                                *cached.bot_member, bot_.me.id));
      return;
    }
    const auto expected_channel = id(cached.channel->id);
    bot_.guild_get_member(
        dpp::snowflake{guild_id.value()}, bot_.me.id,
        [this, callbacks = callbacks_, guild_id, user_id, expected_channel,
         callback = std::move(callback)](
            const dpp::confirmation_callback_t &confirmation) mutable {
          fenced(callbacks, [this, guild_id, user_id, expected_channel,
                             callback = std::move(callback),
                             &confirmation]() mutable {
            if (confirmation.is_error() || shutdown_.load()) {
              callback({.status = VoiceResolveStatus::unavailable,
                        .channel_id = {},
                        .human_count = 0,
                        .failure_category = "bot_member_fetch_failed"});
              return;
            }
            try {
              const auto member = confirmation.get<dpp::guild_member>();
              const auto recheck = resolve_cache(guild_id, user_id, bot_.me.id);
              if (recheck.guild == nullptr || recheck.channel == nullptr ||
                  id(recheck.channel->id) != expected_channel) {
                callback({.status = VoiceResolveStatus::unavailable,
                          .channel_id = {},
                          .human_count = 0,
                          .failure_category = "member_voice_state_changed"});
                return;
              }
              {
                const std::scoped_lock lock{mutex_};
                fetched_bot_member_ = member;
                fetched_guild_id_ = guild_id;
              }
              callback(resolved_channel(*recheck.guild, *recheck.channel,
                                        member, bot_.me.id));
            } catch (...) {
              callback({.status = VoiceResolveStatus::unavailable,
                        .channel_id = {},
                        .human_count = 0,
                        .failure_category = "bot_member_fetch_invalid"});
            }
          });
        });
  }

  VoiceGatewaySubmit connect(const VoiceConnectRequest &request) {
    if (shutdown_.load() || request.session_id.empty() ||
        !request.guild_id.is_set() || !request.channel_id.is_set() ||
        (request.validate_member_channel && !request.member_user_id.is_set()) ||
        request.generation == 0)
      return VoiceGatewaySubmit::invalid;
    auto *guild = dpp::find_guild(dpp::snowflake{request.guild_id.value()});
    auto *channel =
        dpp::find_channel(dpp::snowflake{request.channel_id.value()});
    if (guild == nullptr || channel == nullptr ||
        channel->guild_id != guild->id)
      return VoiceGatewaySubmit::unavailable;
    if (request.validate_member_channel) {
      const auto voice = guild->voice_members.find(
          dpp::snowflake{request.member_user_id.value()});
      if (voice == guild->voice_members.end() ||
          id(voice->second.channel_id) != request.channel_id)
        return VoiceGatewaySubmit::unavailable;
    }
    const auto member = guild->members.find(bot_.me.id);
    std::optional<dpp::guild_member> fetched_member;
    if (member == guild->members.end()) {
      const std::scoped_lock lock{mutex_};
      if (fetched_bot_member_ && fetched_guild_id_ == request.guild_id)
        fetched_member = fetched_bot_member_;
    }
    const auto *bot_member = member != guild->members.end() ? &member->second
                             : fetched_member               ? &*fetched_member
                                                            : nullptr;
    if (bot_member == nullptr ||
        resolved_channel(*guild, *channel, *bot_member, bot_.me.id).status !=
            VoiceResolveStatus::ready)
      return VoiceGatewaySubmit::unavailable;
    auto *shard = bot_.get_shard(guild->shard_id);
    if (shard == nullptr)
      return VoiceGatewaySubmit::unavailable;
    {
      const std::scoped_lock lock{mutex_};
      const auto current_binding = snapshot_unlocked();
      if (!dpp_voice_gateway_detail::may_replace_binding(current_binding,
                                                         request.session_id))
        return VoiceGatewaySubmit::unavailable;
      if (binding_ && binding_->session_id != request.session_id)
        suppress_next_departure_ = true;
      binding_ = Binding{.session_id = request.session_id,
                         .guild_id = request.guild_id,
                         .channel_id = request.channel_id,
                         .observed_channel_id = request.channel_id,
                         .member_user_id = request.member_user_id,
                         .discord_session_id = {},
                         .generation = request.generation,
                         .shard_id = guild->shard_id,
                         .human_count = human_count(*channel, bot_.me.id),
                         .connected = false,
                         .ready = false,
                         .dave_active = false,
                         .marker_completed = false,
                         .bot_moved = false,
                         .expected_marker = {}};
    }
    try {
      shard->connect_voice(dpp::snowflake{request.guild_id.value()},
                           dpp::snowflake{request.channel_id.value()}, false,
                           true, true);
      return VoiceGatewaySubmit::accepted;
    } catch (...) {
      const std::scoped_lock lock{mutex_};
      binding_.reset();
      return VoiceGatewaySubmit::unavailable;
    }
  }

  VoiceGatewaySubmit disconnect(const std::string_view session_id) {
    const auto binding = get_binding(session_id);
    if (!binding)
      return VoiceGatewaySubmit::invalid;
    auto *shard = bot_.get_shard(binding->shard_id);
    if (shard == nullptr)
      return VoiceGatewaySubmit::unavailable;
    try {
      shard->disconnect_voice(dpp::snowflake{binding->guild_id.value()});
      return VoiceGatewaySubmit::accepted;
    } catch (...) {
      return VoiceGatewaySubmit::unavailable;
    }
  }

  void release_binding(const std::string_view session_id) noexcept {
    const std::scoped_lock lock{mutex_};
    if (!binding_ || binding_->session_id != session_id)
      return;
    suppress_next_departure_ = suppress_next_departure_ || binding_->connected;
    binding_.reset();
  }

  VoiceGatewaySubmit stop_audio(const std::string_view session_id) {
    return with_voice_client(session_id, [](dpp::discord_voice_client &voice) {
      voice.stop_audio();
    });
  }

  VoiceGatewaySubmit send_pcm(const std::string_view session_id,
                              const PcmAudio &audio,
                              const std::string_view marker) {
    constexpr auto expected_bytes = std::size_t{115'200};
    if (audio.sample_rate != 48'000 || audio.channels != 2 ||
        audio.bits_per_sample != 16 || marker.empty() ||
        audio.samples.size() * sizeof(std::int16_t) != expected_bytes)
      return VoiceGatewaySubmit::invalid;
    {
      const std::scoped_lock lock{mutex_};
      if (!binding_ || binding_->session_id != session_id || !binding_->ready ||
          !binding_->dave_active)
        return VoiceGatewaySubmit::unavailable;
      binding_->expected_marker = std::string{marker};
      binding_->marker_completed = false;
    }
    return with_voice_client(
        session_id, [&audio, marker](dpp::discord_voice_client &voice) {
          constexpr auto chunk_bytes = dpp::send_audio_raw_max_length;
          constexpr auto samples_per_chunk = chunk_bytes / sizeof(std::int16_t);
          for (std::size_t offset = 0; offset < audio.samples.size();
               offset += samples_per_chunk) {
            auto *samples = reinterpret_cast<std::uint16_t *>(
                const_cast<std::int16_t *>(audio.samples.data() + offset));
            voice.send_audio_raw(samples, chunk_bytes);
          }
          voice.insert_marker(std::string{marker});
        });
  }

  VoiceGatewaySnapshot
  snapshot(const std::string_view session_id) const noexcept {
    const std::scoped_lock lock{mutex_};
    if (!binding_ || binding_->session_id != session_id)
      return {};
    return snapshot_unlocked();
  }

  [[nodiscard]] VoiceGatewaySnapshot snapshot_unlocked() const noexcept {
    if (!binding_)
      return {};
    return {.bound = true,
            .connected = binding_->connected,
            .ready = binding_->ready,
            .dave_active = binding_->dave_active,
            .marker_completed = binding_->marker_completed,
            .bot_moved = binding_->bot_moved,
            .session_id = binding_->session_id,
            .guild_id = binding_->guild_id,
            .channel_id = binding_->channel_id,
            .observed_channel_id = binding_->observed_channel_id,
            .generation = binding_->generation,
            .human_count = binding_->human_count};
  }

  void shutdown() noexcept {
    if (shutdown_.exchange(true))
      return;
    try {
      if (voice_ready_handle_ != 0) {
        static_cast<void>(bot_.on_voice_ready.detach(voice_ready_handle_));
        voice_ready_handle_ = 0;
      }
      if (voice_state_handle_ != 0) {
        static_cast<void>(
            bot_.on_voice_state_update.detach(voice_state_handle_));
        voice_state_handle_ = 0;
      }
      if (marker_handle_ != 0) {
        static_cast<void>(bot_.on_voice_track_marker.detach(marker_handle_));
        marker_handle_ = 0;
      }
    } catch (...) {
    }
    callbacks_->close_and_wait();
    const std::scoped_lock lock{mutex_};
    event_callback_ = {};
    binding_.reset();
    fetched_bot_member_.reset();
    fetched_guild_id_ = {};
    suppress_next_departure_ = false;
  }

private:
  template <typename Callback>
  VoiceGatewaySubmit with_voice_client(const std::string_view session_id,
                                       Callback callback) {
    const auto binding = get_binding(session_id);
    if (!binding)
      return VoiceGatewaySubmit::invalid;
    auto *shard = bot_.get_shard(binding->shard_id);
    auto *connection =
        shard == nullptr
            ? nullptr
            : shard->get_voice(dpp::snowflake{binding->guild_id.value()});
    if (connection == nullptr || !connection->voiceclient)
      return VoiceGatewaySubmit::unavailable;
    try {
      callback(*connection->voiceclient);
      return VoiceGatewaySubmit::accepted;
    } catch (...) {
      return VoiceGatewaySubmit::unavailable;
    }
  }

  [[nodiscard]] std::optional<Binding>
  get_binding(const std::string_view session_id) const {
    const std::scoped_lock lock{mutex_};
    if (!binding_ || binding_->session_id != session_id)
      return std::nullopt;
    return binding_;
  }

  void emit(VoiceEvent event) noexcept {
    EventCallback callback;
    {
      const std::scoped_lock lock{mutex_};
      callback = event_callback_;
    }
    try {
      if (callback)
        callback(std::move(event));
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "vox.callback",
                         "A redacted Vox callback failed.", std::nullopt});
    }
  }

  void on_ready(const dpp::voice_ready_t &event) {
    if (event.voice_client == nullptr)
      return;
    VoiceEvent translated;
    {
      const std::scoped_lock lock{mutex_};
      if (!binding_ || id(event.voice_client->server_id) != binding_->guild_id)
        return;
      auto *shard = bot_.get_shard(binding_->shard_id);
      auto *current =
          shard == nullptr
              ? nullptr
              : shard->get_voice(dpp::snowflake{binding_->guild_id.value()});
      if (current == nullptr ||
          !dpp_voice_gateway_detail::matches_voice_client(
              current->voiceclient.get(), event.voice_client) ||
          event.voice_client->sessionid.empty())
        return;
      if (!binding_->discord_session_id.empty() &&
          !dpp_voice_gateway_detail::matches_voice_session(
              binding_->discord_session_id, event.voice_client->sessionid))
        return;
      binding_->discord_session_id = event.voice_client->sessionid;
      if (binding_->bot_moved) {
        translated = event_from(*binding_, VoiceEventKind::bot_moved);
        translated.channel_id = binding_->observed_channel_id;
      } else {
        const auto ready = event.voice_client->is_ready();
        const auto dave =
            ready && event.voice_client->is_end_to_end_encrypted();
        translated = dpp_voice_gateway_detail::translate_ready(
            snapshot_unlocked(), id(event.voice_channel_id), ready, dave);
        if (translated.kind == VoiceEventKind::ready) {
          suppress_next_departure_ = false;
          binding_->observed_channel_id = binding_->channel_id;
          binding_->connected = true;
          binding_->ready = true;
          binding_->dave_active = dave;
        } else if (translated.kind == VoiceEventKind::error) {
          binding_->observed_channel_id = binding_->channel_id;
          binding_->connected = true;
          binding_->ready = false;
          binding_->dave_active = false;
        } else if (translated.kind == VoiceEventKind::bot_moved) {
          binding_->observed_channel_id = id(event.voice_channel_id);
          binding_->connected = true;
          binding_->ready = false;
          binding_->dave_active = false;
          binding_->bot_moved = true;
        }
      }
    }
    emit(std::move(translated));
  }

  void on_voice_state(const dpp::voice_state_update_t &event) {
    VoiceEvent translated;
    bool should_emit = false;
    {
      const std::scoped_lock lock{mutex_};
      if (event.state.user_id == bot_.me.id && event.state.channel_id == 0 &&
          suppress_next_departure_) {
        suppress_next_departure_ = false;
        return;
      }
      if (!binding_ || id(event.state.guild_id) != binding_->guild_id)
        return;
      if (event.state.user_id == bot_.me.id) {
        if (event.state.channel_id != 0 && !event.state.session_id.empty())
          binding_->discord_session_id = event.state.session_id;
        if (event.state.channel_id == 0) {
          binding_->connected = false;
          binding_->ready = false;
          binding_->dave_active = false;
          if (!binding_->bot_moved)
            binding_->observed_channel_id = {};
          translated = event_from(*binding_, VoiceEventKind::disconnected);
          should_emit = true;
        } else if (id(event.state.channel_id) != binding_->channel_id) {
          binding_->observed_channel_id = id(event.state.channel_id);
          binding_->connected = true;
          binding_->ready = false;
          binding_->dave_active = false;
          binding_->bot_moved = true;
          translated = event_from(*binding_, VoiceEventKind::bot_moved);
          translated.channel_id = id(event.state.channel_id);
          should_emit = true;
        } else {
          suppress_next_departure_ = false;
          if (!binding_->bot_moved)
            binding_->observed_channel_id = binding_->channel_id;
          binding_->connected = true;
        }
      } else if (auto *channel = dpp::find_channel(
                     dpp::snowflake{binding_->channel_id.value()})) {
        const auto count = human_count(*channel, bot_.me.id);
        if (count != binding_->human_count) {
          binding_->human_count = count;
          translated = event_from(*binding_, VoiceEventKind::occupancy_changed);
          translated.human_count = count;
          should_emit = true;
        }
      }
    }
    if (should_emit)
      emit(std::move(translated));
  }

  void on_marker(const dpp::voice_track_marker_t &event) {
    if (event.voice_client == nullptr)
      return;
    VoiceEvent translated;
    {
      const std::scoped_lock lock{mutex_};
      if (!binding_ ||
          id(event.voice_client->server_id) != binding_->guild_id ||
          event.track_meta != binding_->expected_marker)
        return;
      binding_->marker_completed = true;
      translated = event_from(*binding_, VoiceEventKind::track_marker);
      translated.marker = event.track_meta;
    }
    emit(std::move(translated));
  }

  [[nodiscard]] static VoiceEvent event_from(const Binding &binding,
                                             const VoiceEventKind kind) {
    return {.kind = kind,
            .session_id = binding.session_id,
            .guild_id = binding.guild_id,
            .channel_id = binding.channel_id,
            .generation = binding.generation,
            .human_count = binding.human_count,
            .dave_active = binding.dave_active,
            .marker = {},
            .failure_category = {}};
  }

  std::shared_ptr<DppClusterHost> cluster_host_;
  dpp::cluster &bot_;
  Diagnostics &diagnostics_;
  std::shared_ptr<CallbackFence> callbacks_;
  mutable std::mutex mutex_;
  std::optional<Binding> binding_;
  std::optional<dpp::guild_member> fetched_bot_member_;
  DiscordSnowflake fetched_guild_id_;
  bool suppress_next_departure_{};
  EventCallback event_callback_;
  std::atomic<bool> started_{false};
  std::atomic<bool> shutdown_{false};
  dpp::event_handle voice_ready_handle_{};
  dpp::event_handle voice_state_handle_{};
  dpp::event_handle marker_handle_{};
};

DppVoiceGateway::DppVoiceGateway(std::shared_ptr<DppClusterHost> cluster_host,
                                 Diagnostics &diagnostics)
    : impl_{std::make_unique<Impl>(std::move(cluster_host), diagnostics)} {}

DppVoiceGateway::~DppVoiceGateway() { shutdown(); }

void DppVoiceGateway::start(EventCallback callback) {
  impl_->start(std::move(callback));
}

void DppVoiceGateway::resolve_member_channel(const DiscordSnowflake guild_id,
                                             const DiscordSnowflake user_id,
                                             ResolveCallback callback) {
  impl_->resolve_member_channel(guild_id, user_id, std::move(callback));
}

VoiceGatewaySubmit
DppVoiceGateway::connect(const VoiceConnectRequest &request) {
  return impl_->connect(request);
}

VoiceGatewaySubmit
DppVoiceGateway::disconnect(const std::string_view session_id) {
  return impl_->disconnect(session_id);
}

void DppVoiceGateway::release_binding(
    const std::string_view session_id) noexcept {
  impl_->release_binding(session_id);
}

VoiceGatewaySubmit
DppVoiceGateway::stop_audio(const std::string_view session_id) {
  return impl_->stop_audio(session_id);
}

VoiceGatewaySubmit DppVoiceGateway::send_pcm(const std::string_view session_id,
                                             const PcmAudio &audio,
                                             const std::string_view marker) {
  return impl_->send_pcm(session_id, audio, marker);
}

VoiceGatewaySnapshot
DppVoiceGateway::snapshot(const std::string_view session_id) const noexcept {
  return impl_->snapshot(session_id);
}

void DppVoiceGateway::shutdown() noexcept { impl_->shutdown(); }

} // namespace sanguinius
