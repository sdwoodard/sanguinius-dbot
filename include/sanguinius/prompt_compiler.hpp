#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/relationships.hpp"

#include <optional>
#include <string>
#include <vector>

namespace sanguinius {

inline constexpr std::size_t maximum_persona_size = 16'384;
inline constexpr std::size_t maximum_compiled_context_size = 16'384;

struct PromptCompilerInput {
  IncomingMessage message;
  std::string current_request;
  std::vector<ConversationEntry> recent;
  std::optional<ConversationEntry> replied;
  PreparedPromptContext social;
  FeatureConfiguration features;
};

class PromptCompiler {
public:
  explicit PromptCompiler(std::string persona);

  [[nodiscard]] AiRequest compile(const PromptCompilerInput &input) const;

private:
  std::string persona_;
};

} // namespace sanguinius
