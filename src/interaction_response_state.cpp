#include "sanguinius/interaction_response_state.hpp"

namespace sanguinius {

bool InteractionResponseState::acknowledge_reply(
    const ResponseVisibility visibility) noexcept {
  return acknowledge(Phase::replied, visibility);
}

bool InteractionResponseState::acknowledge_defer(
    const ResponseVisibility visibility) noexcept {
  return acknowledge(Phase::deferred, visibility);
}

bool InteractionResponseState::acknowledge_modal() noexcept {
  if (phase_ != Phase::unacknowledged) {
    return false;
  }
  phase_ = Phase::modal;
  return true;
}

bool InteractionResponseState::may_edit() const noexcept {
  return phase_ == Phase::deferred || phase_ == Phase::replied;
}

bool InteractionResponseState::may_follow_up(
    const ResponseVisibility visibility) const noexcept {
  return may_edit() && visibility == visibility_;
}

bool InteractionResponseState::acknowledge(
    const Phase phase, const ResponseVisibility visibility) noexcept {
  if (phase_ != Phase::unacknowledged) {
    return false;
  }
  phase_ = phase;
  visibility_ = visibility;
  return true;
}

} // namespace sanguinius
