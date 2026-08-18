#pragma once

#include "sanguinius/discord_types.hpp"

namespace sanguinius {

class InteractionResponseState {
public:
  [[nodiscard]] bool acknowledge_reply(ResponseVisibility visibility) noexcept;
  [[nodiscard]] bool acknowledge_defer(ResponseVisibility visibility) noexcept;
  [[nodiscard]] bool acknowledge_modal() noexcept;
  [[nodiscard]] bool may_edit() const noexcept;
  [[nodiscard]] bool
  may_follow_up(ResponseVisibility visibility) const noexcept;

private:
  enum class Phase {
    unacknowledged,
    deferred,
    replied,
    modal,
  };

  [[nodiscard]] bool acknowledge(Phase phase,
                                 ResponseVisibility visibility) noexcept;

  Phase phase_{Phase::unacknowledged};
  ResponseVisibility visibility_{ResponseVisibility::public_message};
};

} // namespace sanguinius
