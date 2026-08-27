#pragma once

#include "sanguinius/safety_controls.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeRuntimeFeatureControlRepository final
    : public RuntimeFeatureControlRepository {
public:
  std::vector<RuntimeFeatureControl> snapshot() override { return controls; }

  RuntimeControlMutation set(std::string_view feature, bool disabled,
                             DiscordSnowflake, std::string,
                             std::string idempotency_key,
                             std::int64_t now_ms) override {
    if (!keys.insert(std::move(idempotency_key)).second)
      return RuntimeControlMutation::duplicate;
    const auto found =
        std::ranges::find(controls, feature, &RuntimeFeatureControl::feature);
    if (found == controls.end())
      throw std::invalid_argument{"Unknown fake safety target."};
    if (found->disabled == disabled)
      return RuntimeControlMutation::unchanged;
    found->disabled = disabled;
    ++found->revision;
    found->changed_at_ms = now_ms;
    return RuntimeControlMutation::applied;
  }

  std::vector<RuntimeFeatureControl> controls{
      {.feature = "text-ai",
       .disabled = false,
       .revision = 1,
       .changed_at_ms = 0},
      {.feature = "tts", .disabled = false, .revision = 1, .changed_at_ms = 0},
      {.feature = "vox-output",
       .disabled = false,
       .revision = 1,
       .changed_at_ms = 0}};
  std::unordered_set<std::string> keys;
};

} // namespace sanguinius::test
