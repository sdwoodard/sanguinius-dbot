#include "sanguinius/composition_root.hpp"

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"
#include "sanguinius/id_generator.hpp"
#include "sanguinius/message_logger.hpp"
#include "sanguinius/openai_client.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_session_repository.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/random.hpp"

#include <array>
#include <chrono>
#include <limits.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string local_hostname() {
  std::array<char, HOST_NAME_MAX + 1> hostname{};
  if (::gethostname(hostname.data(), HOST_NAME_MAX) != 0) {
    throw std::runtime_error{"Unable to determine application hostname."};
  }
  hostname.back() = '\0';
  const std::string result{hostname.data()};
  if (result.empty()) {
    throw std::runtime_error{"Unable to determine application hostname."};
  }
  return result;
}

} // namespace

std::unique_ptr<Application> make_application(const Config &config) {
  auto clock = std::make_unique<SystemClock>();
  auto database =
      persistence::Database::open_runtime(config.paths.database_file);
  const persistence::Migrator migrator{persistence::production_migrations(),
                                       current_build_info(), *clock};
  migrator.require_current(database.connection());
  const auto migration = migrator.inspect(database.connection());
  auto repository_context =
      std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(database));
  auto identities = std::make_unique<persistence::SqliteCoreIdentityRepository>(
      repository_context);
  identities->initialize_or_validate_scope(config.discord.server_scope,
                                           unix_milliseconds(*clock));
  auto application_instances =
      std::make_unique<persistence::SqliteApplicationInstanceRepository>(
          repository_context);
  auto persistent_ids = std::make_unique<UuidV4Generator>();
  const auto instance_id = persistent_ids->next_id();
  auto pending_notices =
      std::make_unique<persistence::SqlitePendingNoticeRepository>(
          repository_context);
  auto durable_work =
      std::make_unique<persistence::SqliteDurableWorkRepository>(
          repository_context);
  auto chronicle =
      std::make_unique<persistence::SqliteChronicleRepository>(
          repository_context);
  auto chronicle_sessions =
      std::make_unique<persistence::SqliteChronicleSessionRepository>(
          repository_context);
  auto relationships =
      std::make_unique<persistence::SqliteRelationshipRepository>(
          repository_context);
  auto appearances =
      std::make_unique<persistence::SqliteAppearanceRepository>(repository_context);
  auto tarot =
      std::make_unique<persistence::SqliteTarotRepository>(repository_context);
  auto random = std::make_unique<SystemRandom>();

  auto id_generator = std::make_unique<ProcessIdGenerator>();
  auto diagnostics = std::make_unique<ConsoleDiagnostics>();
  auto message_log =
      std::make_unique<MessageLogger>(config.paths.message_log, *clock);
  auto ai_client =
      std::make_unique<OpenAiClient>(config.ai.api_key, config.ai.model);
  auto discord = std::make_unique<DppDiscordAdapter>(
      config.discord.token, config.discord.request_timeout,
      config.discord.server_scope.guild_id, *diagnostics);

  return std::make_unique<Application>(
      ApplicationOptions{
          .persona = config.ai.persona,
          .command_prefix = config.command_prefix,
          .server_scope = config.discord.server_scope,
          .controls = config.controls,
          .features = config.features,
          .tarot_policy = config.tarot_policy,
          .build = current_build_info(),
          .persistence =
              PersistenceHealth{
                  .ready = true,
                  .schema_version = migration.current_version,
                  .target_schema_version = migration.target_version,
                  .sqlite_version = persistence::sqlite_runtime_version(),
                  .instance_id = instance_id,
              },
          .instance_id = instance_id,
          .timezone = config.timezone,
          .hostname = local_hostname(),
          .process_id = static_cast<std::int64_t>(::getpid()),
          .message_queue_capacity = 64,
          .ai_queue_capacity = 64,
          .ai_worker_count = 2,
          .interaction_queue_capacity = 64,
      },
      ApplicationDependencies{
          .clock = std::move(clock),
          .id_generator = std::move(id_generator),
          .persistent_id_generator = std::move(persistent_ids),
          .diagnostics = std::move(diagnostics),
          .message_log = std::move(message_log),
          .application_instances = std::move(application_instances),
          .identities = std::move(identities),
          .pending_notices = std::move(pending_notices),
          .durable_work = std::move(durable_work),
          .chronicle = std::move(chronicle),
          .chronicle_sessions = std::move(chronicle_sessions),
          .relationships = std::move(relationships),
          .appearances = std::move(appearances),
          .tarot = std::move(tarot),
          .random = std::move(random),
          .appearance_policy = config.appearance_policy,
          .ai_client = std::move(ai_client),
          .discord = std::move(discord),
      });
}

} // namespace sanguinius
