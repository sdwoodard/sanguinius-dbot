#include "sanguinius/sanguinius_overview.hpp"

#include "sanguinius/presentation.hpp"

#include <chrono>
#include <sstream>
#include <string>

namespace sanguinius {
namespace {

[[nodiscard]] std::string enabled(const bool value) {
  return value ? "enabled" : "disabled";
}

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string normalized_state(const std::string_view state) {
  if (state == "ready" || state == "quiet" || state == "muted" ||
      state == "degraded" || state == "disabled" || state == "unavailable")
    return std::string{state};
  return "unavailable";
}

[[nodiscard]] std::string
consent_state(const std::optional<bool> consent_attested) {
  if (!consent_attested.has_value())
    return "unavailable";
  return *consent_attested ? "attested" : "not attested";
}

} // namespace

SanguiniusOverviewService::SanguiniusOverviewService(
    const FeatureConfiguration features, const Clock &clock)
    : features_{features}, clock_{clock} {}

InteractionMessage
SanguiniusOverviewService::help(const std::string_view topic) const {
  return presentation::help(topic, features_, now_ms(clock_));
}

InteractionMessage
SanguiniusOverviewService::status(const std::size_t pending_notices,
                                  const std::string_view appearance_status,
                                  const UserPreferences &preferences,
                                  const MemberRuntimeStatus &runtime) const {
  EmbedPayload embed{
      .color = presentation::neutral,
      .title = "Sanguinius status",
      .description = "Ready for deterministic commands.",
      .fields = {{.name = "Chronicle",
                  .value =
                      features_.chronicle_enabled ? "ready" : "unavailable",
                  .inline_field = true},
                 {.name = "Tarot",
                  .value = features_.tarot_enabled ? "ready" : "unavailable",
                  .inline_field = true},
                 {.name = "Vox",
                  .value = normalized_state(runtime.vox_output),
                  .inline_field = true},
                 {.name = "Text AI",
                  .value = normalized_state(runtime.text_ai),
                  .inline_field = true},
                 {.name = "TTS",
                  .value = normalized_state(runtime.tts),
                  .inline_field = true},
                 {.name = "Listening",
                  .value = normalized_state(runtime.voice_input),
                  .inline_field = true},
                 {.name = "Appearances",
                  .value = normalized_state(appearance_status)},
                 {.name = "Chronicle callbacks",
                  .value = enabled(preferences.memory_callback_opt_in),
                  .inline_field = true},
                 {.name = "Appearance callbacks",
                  .value = enabled(preferences.appearance_callback_opt_in),
                  .inline_field = true},
                 {.name = "Sealed notices",
                  .value = std::to_string(pending_notices),
                  .inline_field = true}},
      .footer = "Private status · use /sanguinius privacy for data controls",
      .timestamp_ms = now_ms(clock_),
  };
  InteractionMessage message{.content = {},
                             .embed = std::move(embed),
                             .buttons = {},
                             .allowed_user_mentions = {}};
  presentation::validate(message);
  return message;
}

InteractionMessage
SanguiniusOverviewService::privacy(const UserPreferences &preferences,
                                   const std::size_t pending_notices,
                                   const std::string_view appearance_status,
                                   const std::string_view tarot_status,
                                   const MemberRuntimeStatus &runtime) const {
  EmbedPayload embed{
      .color = presentation::neutral,
      .title = "Your privacy controls",
      .description = "Discord DMs are never used. Raw received voice audio is "
                     "never persisted.",
      .fields =
          {{.name = "Chronicle",
            .value =
                "Storage opt-in: " + enabled(preferences.chronicle_opt_in) +
                "\nCallbacks: " + enabled(preferences.memory_callback_opt_in) +
                "\nControl with `/chronicle profile`, `/chronicle callbacks`, "
                "and `/chronicle forget`."},
           {.name = "Relationships",
            .value = "Qualitative private summaries only; hidden scores are "
                     "never exposed."},
           {.name = "Appearances",
            .value = "Callbacks: " +
                     enabled(preferences.appearance_callback_opt_in) + "\n" +
                     std::string{appearance_status} +
                     "\nControl with `/sanguinius appearance-callbacks` and "
                     "`/sanguinius quiet for`, `/sanguinius quiet tonight`, "
                     "`/sanguinius quiet until`, or `/sanguinius quiet off`."},
           {.name = "Tarot & sealed notices",
            .value = std::string{tarot_status} +
                     "\nPending notices: " + std::to_string(pending_notices) +
                     "\nOpen private content with `/sanguinius inbox`."},
           {.name = "Voice",
            .value = "Output: " + normalized_state(runtime.vox_output) +
                     "\nInput: " + normalized_state(runtime.voice_input) +
                     "\nGuild consent: " +
                     consent_state(runtime.voice_consent_attested) +
                     "\nControl output with `/vox summon`, `/vox mute`, and "
                     "`/vox leave`. Control listening with `/vox "
                     "listen-start` and `/vox listen-stop`. Listening needs "
                     "prior guild consent and a public indicator. Raw audio "
                     "retention: zero."}},
      .footer =
          "Private overview · authoritative state changes remain deterministic",
      .timestamp_ms = now_ms(clock_),
  };
  InteractionMessage message{.content = {},
                             .embed = std::move(embed),
                             .buttons = {},
                             .allowed_user_mentions = {}};
  presentation::validate(message);
  return message;
}

std::string SanguiniusOverviewService::owner_health(
    std::string operational_health, const std::string_view appearance_status,
    const MemberRuntimeStatus &runtime,
    const std::string_view appearance_diagnostics) const {
  operational_health +=
      "\nMember overview: appearances=" + normalized_state(appearance_status) +
      ", text_ai=" + normalized_state(runtime.text_ai) +
      ", tts=" + normalized_state(runtime.tts) +
      ", vox_output=" + normalized_state(runtime.vox_output) +
      ", voice_input=" + normalized_state(runtime.voice_input);
  if (!appearance_diagnostics.empty())
    operational_health +=
        "\nAppearances: " + std::string{appearance_diagnostics};
  return operational_health;
}

} // namespace sanguinius
