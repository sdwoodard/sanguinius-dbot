#include "sanguinius/dpp_cluster_host.hpp"

#include <dpp/dpp.h>

#include <atomic>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::uint32_t text_intents =
    dpp::i_guilds | dpp::i_guild_messages | dpp::i_message_content;
static_assert((text_intents & dpp::i_direct_messages) == 0U);

[[nodiscard]] constexpr std::uint32_t intents_for(const bool voice_enabled) {
  return text_intents |
         (voice_enabled ? static_cast<std::uint32_t>(dpp::i_guild_voice_states)
                        : 0U);
}

} // namespace

class DppClusterHost::Impl {
public:
  Impl(std::string token, const bool voice_enabled)
      : intents_{intents_for(voice_enabled)}, bot_{std::move(token), intents_} {
    // Voice input sends an explicit Gateway VOICE_STATE_UPDATE so a same-channel
    // self-deaf change cannot be swallowed by D++'s connect_voice no-op.
    bot_.set_websocket_protocol(dpp::ws_json);
  }

  const std::uint32_t intents_;
  dpp::cluster bot_;
  std::atomic<bool> started_{false};
  std::atomic<bool> shutdown_{false};
};

DppClusterHost::DppClusterHost(std::string token, const bool voice_enabled)
    : impl_{std::make_unique<Impl>(std::move(token), voice_enabled)} {}

DppClusterHost::~DppClusterHost() { shutdown(); }

dpp::cluster &DppClusterHost::native() noexcept { return impl_->bot_; }

std::uint32_t DppClusterHost::intents() const noexcept {
  return impl_->intents_;
}

void DppClusterHost::start() {
  if (!impl_->started_.exchange(true))
    impl_->bot_.start(dpp::st_return);
}

void DppClusterHost::shutdown() noexcept {
  if (impl_->shutdown_.exchange(true))
    return;
  try {
    impl_->bot_.shutdown();
  } catch (...) {
  }
}

} // namespace sanguinius
