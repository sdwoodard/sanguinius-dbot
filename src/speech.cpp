#include "sanguinius/speech.hpp"

namespace sanguinius {

const char *speech_priority_name(const SpeechPriority priority) noexcept {
  switch (priority) {
  case SpeechPriority::flavor:
    return "flavor";
  case SpeechPriority::event_narration:
    return "event_narration";
  case SpeechPriority::interactive:
    return "interactive";
  case SpeechPriority::critical_control:
    return "critical_control";
  }
  return "flavor";
}

const char *speech_state_name(const SpeechState state) noexcept {
  switch (state) {
  case SpeechState::pending:
    return "pending";
  case SpeechState::synthesizing:
    return "synthesizing";
  case SpeechState::ready:
    return "ready";
  case SpeechState::playing:
    return "playing";
  case SpeechState::played:
    return "played";
  case SpeechState::failed:
    return "failed";
  case SpeechState::expired:
    return "expired";
  case SpeechState::cancelled:
    return "cancelled";
  }
  return "failed";
}

bool speech_transition_allowed(const SpeechState from,
                               const SpeechState to) noexcept {
  switch (from) {
  case SpeechState::pending:
    return to == SpeechState::synthesizing || to == SpeechState::cancelled ||
           to == SpeechState::expired;
  case SpeechState::synthesizing:
    return to == SpeechState::ready || to == SpeechState::failed ||
           to == SpeechState::cancelled || to == SpeechState::expired;
  case SpeechState::ready:
    return to == SpeechState::playing || to == SpeechState::cancelled ||
           to == SpeechState::expired || to == SpeechState::failed;
  case SpeechState::playing:
    return to == SpeechState::played || to == SpeechState::failed ||
           to == SpeechState::cancelled;
  case SpeechState::played:
  case SpeechState::failed:
  case SpeechState::expired:
  case SpeechState::cancelled:
    return false;
  }
  return false;
}

} // namespace sanguinius
