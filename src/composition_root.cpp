#include "sanguinius/composition_root.hpp"

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"
#include "sanguinius/id_generator.hpp"
#include "sanguinius/message_logger.hpp"
#include "sanguinius/openai_client.hpp"

#include <utility>

namespace sanguinius {

std::unique_ptr<Application> make_application(const Config &config) {
  auto clock = std::make_unique<SystemClock>();
  auto id_generator = std::make_unique<ProcessIdGenerator>();
  auto diagnostics = std::make_unique<ConsoleDiagnostics>();
  auto message_log =
      std::make_unique<MessageLogger>(config.paths.message_log, *clock);
  auto ai_client =
      std::make_unique<OpenAiClient>(config.ai.api_key, config.ai.model);
  auto discord = std::make_unique<DppDiscordAdapter>(
      config.discord.token, config.discord.request_timeout, *diagnostics);

  return std::make_unique<Application>(
      ApplicationOptions{
          .persona = config.ai.persona,
          .command_prefix = config.command_prefix,
          .server_scope = config.discord.server_scope,
          .controls = config.controls,
          .features = config.features,
          .build = current_build_info(),
          .message_queue_capacity = 64,
          .ai_queue_capacity = 64,
          .ai_worker_count = 2,
      },
      ApplicationDependencies{
          .clock = std::move(clock),
          .id_generator = std::move(id_generator),
          .diagnostics = std::move(diagnostics),
          .message_log = std::move(message_log),
          .ai_client = std::move(ai_client),
          .discord = std::move(discord),
      });
}

} // namespace sanguinius
