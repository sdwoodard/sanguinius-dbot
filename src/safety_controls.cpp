#include "sanguinius/safety_controls.hpp"

#include "sanguinius/ai_generation.hpp"
#include "sanguinius/appearances.hpp"
#include "sanguinius/presentation.hpp"
#include "sanguinius/speech_service.hpp"
#include "sanguinius/voice_input.hpp"
#include "sanguinius/vox.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] bool disabled(const std::vector<RuntimeFeatureControl> &controls,
                            const std::string_view feature) {
  const auto found =
      std::ranges::find(controls, feature, &RuntimeFeatureControl::feature);
  return found == controls.end() || found->disabled;
}

} // namespace

SafetyTarget parse_safety_target(const std::string_view value) {
  if (value == "appearances")
    return SafetyTarget::appearances;
  if (value == "text-ai")
    return SafetyTarget::text_ai;
  if (value == "tts")
    return SafetyTarget::tts;
  if (value == "vox-output")
    return SafetyTarget::vox_output;
  if (value == "voice-input")
    return SafetyTarget::voice_input;
  throw std::invalid_argument{"Unknown safety target."};
}

std::string_view safety_target_name(const SafetyTarget target) noexcept {
  switch (target) {
  case SafetyTarget::appearances:
    return "appearances";
  case SafetyTarget::text_ai:
    return "text-ai";
  case SafetyTarget::tts:
    return "tts";
  case SafetyTarget::vox_output:
    return "vox-output";
  case SafetyTarget::voice_input:
    return "voice-input";
  }
  return "unknown";
}

SafetyControlService::SafetyControlService(
    std::unique_ptr<RuntimeFeatureControlRepository> runtime,
    const Clock &clock, PersistentIdGenerator &ids,
    const FeatureConfiguration features, AiGenerationService *text_ai,
    AppearanceService *appearances, SpeechService *speech, VoxService *vox,
    VoiceListeningService *voice_input)
    : runtime_{std::move(runtime)}, clock_{clock}, ids_{ids},
      features_{features}, text_ai_{text_ai}, appearances_{appearances},
      speech_{speech}, vox_{vox}, voice_input_{voice_input} {
  if (!runtime_)
    throw std::invalid_argument{"Runtime feature controls are required."};
  const auto controls = runtime_->snapshot();
  if (speech_)
    speech_->set_provider_operator_disabled(disabled(controls, "tts"));
  if (vox_)
    vox_->set_output_operator_disabled(disabled(controls, "vox-output"));
}

MemberRuntimeStatus SafetyControlService::member_status() const {
  const std::scoped_lock lock{mutex_};
  const auto controls = runtime_->snapshot();
  MemberRuntimeStatus result;
  if (text_ai_) {
    const auto health = text_ai_->health();
    if (disabled(controls, "text-ai") || health.operator_disabled)
      result.text_ai = "disabled";
    else if (health.circuit_state != "closed" ||
             health.generation_limit_exhausted ||
             health.rolling_day_budget_exhausted ||
             health.calendar_month_budget_exhausted)
      result.text_ai = "degraded";
    else
      result.text_ai = "ready";
  }
  if (features_.vox_enabled && speech_) {
    const auto health = speech_->health();
    if (disabled(controls, "tts"))
      result.tts = "disabled";
    else if (!health.provider_configured)
      result.tts = "unavailable";
    else if (!health.provider_enabled ||
             health.provider_circuit_state != "closed" ||
             health.repository.usage.rolling_day_attempts >=
                 health.usage_policy.rolling_day_attempts ||
             health.repository.usage.rolling_day_micro_usd >=
                 health.usage_policy.rolling_day_micro_usd ||
             health.repository.usage.calendar_month_micro_usd >=
                 health.usage_policy.calendar_month_micro_usd)
      result.tts = "degraded";
    else
      result.tts = "ready";
  }
  if (features_.vox_enabled && vox_) {
    if (disabled(controls, "vox-output"))
      result.vox_output = "disabled";
    else {
      const auto health = vox_->health();
      if (health.operator_disabled)
        result.vox_output = "disabled";
      else if (health.state == VoxState::muted)
        result.vox_output = "muted";
      else if (health.state == VoxState::failed ||
               health.state == VoxState::connecting ||
               health.state == VoxState::reconnecting ||
               health.state == VoxState::leaving ||
               health.last_failure_category)
        result.vox_output = "degraded";
      else
        result.vox_output = "ready";
    }
  }
  if (voice_input_) {
    const auto health = voice_input_->health();
    result.voice_consent_attested = health.consent_attested;
    if (health.repository.kill_switch)
      result.voice_input = "disabled";
    else if (!features_.voice_input_enabled ||
             health.capability == VoiceInputCapability::disabled)
      result.voice_input = "disabled";
    else if (health.capability != VoiceInputCapability::ready ||
             !health.provider_enabled)
      result.voice_input = "unavailable";
    else if (health.provider_circuit_state != "closed" ||
             health.last_failure_category)
      result.voice_input = "degraded";
    else
      result.voice_input = "ready";
  }
  return result;
}

InteractionMessage SafetyControlService::status() const {
  const std::scoped_lock lock{mutex_};
  const auto controls = runtime_->snapshot();
  const auto tts_configured =
      features_.vox_enabled && speech_ && speech_->health().provider_configured;
  const auto state = [&controls](const std::string_view feature,
                                 const bool configured) {
    auto result = disabled(controls, feature) ? std::string{"disabled"}
                                              : std::string{"enabled"};
    if (!configured)
      result += " (configuration unavailable)";
    return result;
  };
  auto appearance_state = appearances_ ? appearances_->operator_disabled()
                                             ? std::string{"disabled"}
                                             : std::string{"enabled"}
                                       : std::string{"unavailable"};
  if (appearances_ && features_.appearances_mode == AppearanceMode::off)
    appearance_state += " (configuration unavailable)";
  auto voice_input_state = voice_input_
                               ? voice_input_->health().repository.kill_switch
                                     ? std::string{"disabled"}
                                     : std::string{"enabled"}
                               : std::string{"unavailable"};
  if (voice_input_ && !features_.voice_input_enabled)
    voice_input_state += " (configuration unavailable)";
  InteractionMessage message{
      .content = {},
      .embed =
          EmbedPayload{
              .color = presentation::neutral,
              .title = "Sanguinius safety controls",
              .description = "Operator kills never override disabled "
                             "configuration, budgets, or provider health.",
              .fields = {{.name = "Appearances",
                          .value = appearance_state,
                          .inline_field = true},
                         {.name = "Text AI",
                          .value = state("text-ai", text_ai_ != nullptr),
                          .inline_field = true},
                         {.name = "TTS",
                          .value = state("tts", tts_configured),
                          .inline_field = true},
                         {.name = "Vox output",
                          .value = state("vox-output", features_.vox_enabled &&
                                                           vox_ != nullptr),
                          .inline_field = true},
                         {.name = "Voice input",
                          .value = voice_input_state,
                          .inline_field = true}},
              .footer =
                  "Use /sang-admin safety set to change one operator kill.",
              .timestamp_ms = now_ms(clock_),
          },
      .buttons = {},
      .allowed_user_mentions = {},
  };
  presentation::validate(message);
  return message;
}

InteractionMessage
SafetyControlService::set(const SafetyTarget target, const bool disabled_value,
                          const DiscordSnowflake actor_user_id,
                          std::string idempotency_key,
                          std::string correlation_id) {
  const std::scoped_lock lock{mutex_};
  if (target == SafetyTarget::voice_input)
    throw std::invalid_argument{
        "Voice input must use the privacy-preemptive control path."};
  if (target == SafetyTarget::appearances) {
    if (!appearances_)
      return presentation::error(presentation::ErrorKind::feature_disabled);
    return text_message(appearances_->set_global_disabled(
        actor_user_id, disabled_value, std::move(idempotency_key),
        std::move(correlation_id)));
  }
  const auto feature = std::string{safety_target_name(target)};
  const auto mutation =
      runtime_->set(feature, disabled_value, actor_user_id, ids_.next_id(),
                    std::move(idempotency_key), now_ms(clock_));
  auto effective_disabled = disabled_value;
  if (mutation == RuntimeControlMutation::duplicate)
    effective_disabled = disabled(runtime_->snapshot(), feature);
  if (target == SafetyTarget::tts && speech_)
    speech_->set_provider_operator_disabled(effective_disabled);
  if (target == SafetyTarget::vox_output && vox_)
    vox_->set_output_operator_disabled(effective_disabled);
  const auto verb = effective_disabled ? "disabled" : "enabled";
  const auto suffix = mutation == RuntimeControlMutation::applied ? "."
                      : mutation == RuntimeControlMutation::duplicate &&
                              effective_disabled != disabled_value
                          ? " (replayed; the newer state was retained)."
                          : " (already in that state).";
  return text_message("Safety target `" + feature + "` is " + verb + suffix);
}

} // namespace sanguinius
