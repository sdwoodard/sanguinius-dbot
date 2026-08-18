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
      std::make_unique<MessageLogger>(config.message_log, *clock);
  auto ai_client = std::make_unique<OpenAiClient>(config.openai_api_key,
                                                  config.openai_model);
  auto discord =
      std::make_unique<DppDiscordAdapter>(config.token, *diagnostics);

  return std::make_unique<Application>(
      ApplicationOptions{
          .persona = config.persona,
          .command_prefix = config.command_prefix,
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
