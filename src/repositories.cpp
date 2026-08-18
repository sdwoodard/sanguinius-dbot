#include "sanguinius/repositories.hpp"

namespace sanguinius {

const char *
application_stop_reason_name(const ApplicationStopReason reason) noexcept {
  switch (reason) {
  case ApplicationStopReason::clean_shutdown:
    return "clean_shutdown";
  case ApplicationStopReason::startup_failure:
    return "startup_failure";
  }
  return "startup_failure";
}

const char *pending_notice_state_name(const PendingNoticeState state) noexcept {
  switch (state) {
  case PendingNoticeState::pending:
    return "pending";
  case PendingNoticeState::opened:
    return "opened";
  case PendingNoticeState::consumed:
    return "consumed";
  case PendingNoticeState::expired:
    return "expired";
  case PendingNoticeState::cancelled:
    return "cancelled";
  }
  return "unknown";
}

const char *
interaction_token_kind_name(const InteractionTokenKind kind) noexcept {
  switch (kind) {
  case InteractionTokenKind::button:
    return "button";
  case InteractionTokenKind::select:
    return "select";
  case InteractionTokenKind::modal:
    return "modal";
  }
  return "unknown";
}

} // namespace sanguinius
