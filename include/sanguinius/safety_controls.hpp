#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

class AppearanceService;
class AiGenerationService;
class SpeechService;
class VoxService;
class VoiceListeningService;

enum class SafetyTarget {
  appearances,
  text_ai,
  tts,
  vox_output,
  voice_input,
};

struct RuntimeFeatureControl {
  std::string feature;
  bool disabled{};
  std::size_t revision{};
  std::int64_t changed_at_ms{};
};

enum class RuntimeControlMutation { applied, unchanged, duplicate };

struct MemberRuntimeStatus {
  std::string text_ai{"unavailable"};
  std::string tts{"unavailable"};
  std::string vox_output{"unavailable"};
  std::string voice_input{"unavailable"};
  std::optional<bool> voice_consent_attested;
};

class RuntimeFeatureControlRepository {
public:
  virtual ~RuntimeFeatureControlRepository() = default;
  [[nodiscard]] virtual std::vector<RuntimeFeatureControl> snapshot() = 0;
  [[nodiscard]] virtual RuntimeControlMutation
  set(std::string_view feature, bool disabled, DiscordSnowflake actor_user_id,
      std::string transition_id, std::string idempotency_key,
      std::int64_t now_ms) = 0;
};

class SafetyControlService {
public:
  SafetyControlService(std::unique_ptr<RuntimeFeatureControlRepository> runtime,
                       const Clock &clock, PersistentIdGenerator &ids,
                       FeatureConfiguration features,
                       AiGenerationService *text_ai,
                       AppearanceService *appearances, SpeechService *speech,
                       VoxService *vox, VoiceListeningService *voice_input);

  [[nodiscard]] InteractionMessage status() const;
  [[nodiscard]] MemberRuntimeStatus member_status() const;
  [[nodiscard]] InteractionMessage set(SafetyTarget target, bool disabled,
                                       DiscordSnowflake actor_user_id,
                                       std::string idempotency_key,
                                       std::string correlation_id);

private:
  std::unique_ptr<RuntimeFeatureControlRepository> runtime_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  FeatureConfiguration features_;
  AiGenerationService *text_ai_{};
  AppearanceService *appearances_{};
  SpeechService *speech_{};
  VoxService *vox_{};
  VoiceListeningService *voice_input_{};
  mutable std::mutex mutex_;
};

[[nodiscard]] SafetyTarget parse_safety_target(std::string_view value);
[[nodiscard]] std::string_view safety_target_name(SafetyTarget target) noexcept;

} // namespace sanguinius
