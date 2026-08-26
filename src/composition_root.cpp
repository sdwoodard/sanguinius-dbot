#include "sanguinius/composition_root.hpp"

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/dpp_discord_adapter.hpp"
#include "sanguinius/dpp_cluster_host.hpp"
#include "sanguinius/dpp_voice_gateway.hpp"
#include "sanguinius/ffmpeg_audio_normalizer.hpp"
#include "sanguinius/id_generator.hpp"
#include "sanguinius/message_logger.hpp"
#include "sanguinius/openai_client.hpp"
#include "sanguinius/openai_tts_client.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_chronicle_session_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_speech_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"
#include "sanguinius/persistence/sqlite_vox_repository.hpp"
#include "sanguinius/persistence/sqlite_vox_narration_repository.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/random.hpp"
#include "sanguinius/static_speech_assets.hpp"
#include "sanguinius/tts_cache.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <limits.h>
#include <stdexcept>
#include <sys/stat.h>
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

void validate_runtime_configuration(const Config &config) {
  if (!config.features.vox_enabled)
    return;
  if (config.tts.model != "tts-1" || config.tts.voice != "onyx" ||
      !config.tts.cache_directory.is_absolute() ||
      !config.tts.fallback_directory.is_absolute())
    throw std::runtime_error{"Vox TTS fixed runtime configuration is invalid."};
  const auto resolved = [](const std::filesystem::path &path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
      throw std::runtime_error{"Unable to resolve a Vox runtime path."};
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    return (error ? absolute : canonical).lexically_normal();
  };
  const auto within = [](const std::filesystem::path &candidate,
                         const std::filesystem::path &directory) {
    auto candidate_part = candidate.begin();
    for (auto directory_part = directory.begin();
         directory_part != directory.end();
         ++directory_part, ++candidate_part) {
      if (candidate_part == candidate.end() ||
          *candidate_part != *directory_part)
        return false;
    }
    return true;
  };
  const auto cache = resolved(config.tts.cache_directory);
  const auto fallback = resolved(config.tts.fallback_directory);
  const auto database = resolved(config.paths.database_file);
  const auto message_log = resolved(config.paths.message_log);
  if (within(database, cache) || within(message_log, cache) ||
      within(fallback, cache) || within(cache, fallback))
    throw std::runtime_error{
        "The TTS cache path overlaps persistent or fallback data."};
  validate_ffmpeg_runtime(config.tts.ffprobe_path, config.tts.ffmpeg_path);

  const auto validate_directory = [](const std::filesystem::path &path,
                                     const std::string_view label,
                                     const bool required,
                                     const bool owner_only) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
      throw std::runtime_error{"Unable to inspect the configured " +
                               std::string{label} + " directory."};
    if (!error && std::filesystem::exists(status)) {
      if (std::filesystem::is_symlink(status) ||
          !std::filesystem::is_directory(status))
        throw std::runtime_error{"Configured " + std::string{label} +
                                 " path is not a safe directory."};
      struct stat native_status {};
      constexpr auto permission_bits =
          S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
      if (owner_only &&
          (::lstat(path.c_str(), &native_status) != 0 ||
           native_status.st_uid != ::geteuid() ||
           (native_status.st_mode & permission_bits) != S_IRWXU))
        throw std::runtime_error{"Configured " + std::string{label} +
                                 " directory is not owner-only."};
      if (::access(path.c_str(), R_OK | X_OK | (required ? 0 : W_OK)) != 0)
        throw std::runtime_error{"Configured " + std::string{label} +
                                 " directory is not accessible."};
      return;
    }
    if (required)
      throw std::runtime_error{"Configured " + std::string{label} +
                               " directory does not exist."};
    auto parent = path.parent_path();
    while (!parent.empty() && !std::filesystem::exists(parent, error))
      parent = parent.parent_path();
    if (parent.empty() || error || ::access(parent.c_str(), W_OK | X_OK) != 0)
      throw std::runtime_error{"Configured " + std::string{label} +
                               " directory cannot be created securely."};
  };
  validate_directory(config.tts.cache_directory, "TTS cache", false, true);
  validate_directory(config.tts.fallback_directory, "Vox fallback", true,
                     false);
  static_cast<void>(load_static_speech_assets(config.tts.fallback_directory));
}

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
  auto chronicle = std::make_unique<persistence::SqliteChronicleRepository>(
      repository_context);
  auto chronicle_sessions =
      std::make_unique<persistence::SqliteChronicleSessionRepository>(
          repository_context);
  auto relationships =
      std::make_unique<persistence::SqliteRelationshipRepository>(
          repository_context);
  auto appearances = std::make_unique<persistence::SqliteAppearanceRepository>(
      repository_context);
  auto tarot =
      std::make_unique<persistence::SqliteTarotRepository>(repository_context);
  auto wagers =
      std::make_unique<persistence::SqliteWagerRepository>(repository_context);
  auto tarot_catalogs =
      std::make_unique<persistence::SqliteTarotCatalogRepository>(
          repository_context);
  auto tarot_draws = std::make_unique<persistence::SqliteTarotDrawRepository>(
      repository_context);
  auto tarot_house = std::make_unique<persistence::SqliteTarotHouseRepository>(
      repository_context);
  auto tarot_integration =
      std::make_unique<persistence::SqliteTarotIntegrationRepository>(
          repository_context);
  auto vox = std::make_unique<persistence::SqliteVoxRepository>(
      repository_context);
  auto speech = std::make_unique<persistence::SqliteSpeechRepository>(
      repository_context);
  auto vox_narration =
      std::make_unique<persistence::SqliteVoxNarrationRepository>(
          repository_context, config.tts.usage_policy);
  std::optional<TarotDeckCatalog> deck_catalog;
  std::optional<TarotHouseCatalog> house_catalog;
  if (config.features.tarot_enabled) {
    if (!config.tarot_deck_catalog || !config.tarot_house_catalog)
      throw std::runtime_error{"Enabled Tarot catalogs were not validated."};
    deck_catalog = config.tarot_deck_catalog;
    house_catalog = config.tarot_house_catalog;
  }
  auto random = std::make_unique<SystemRandom>();

  auto id_generator = std::make_unique<ProcessIdGenerator>();
  auto diagnostics = std::make_unique<ConsoleDiagnostics>();
  auto message_log =
      std::make_unique<MessageLogger>(config.paths.message_log, *clock);
  auto ai_client =
      std::make_unique<OpenAiClient>(config.ai.api_key, config.ai.model);
  auto cluster_host = std::make_shared<DppClusterHost>(
      config.discord.token, config.features.vox_enabled);
  auto discord = std::make_unique<DppDiscordAdapter>(
      cluster_host, config.discord.request_timeout,
      config.discord.server_scope.guild_id, *diagnostics);
  std::unique_ptr<VoiceGateway> voice_gateway;
  std::unique_ptr<TextToSpeechClient> text_to_speech;
  std::unique_ptr<AudioNormalizer> audio_normalizer;
  std::unique_ptr<TtsCache> tts_cache;
  if (config.features.vox_enabled)
    voice_gateway =
        std::make_unique<DppVoiceGateway>(cluster_host, *diagnostics);
  if (config.features.vox_enabled) {
    if (config.tts.provider == TtsProvider::openai) {
      text_to_speech = std::make_unique<OpenAiTtsClient>(
          config.ai.api_key, nullptr,
          OpenAiTtsClientConfiguration{
              .connect_timeout = config.tts.connect_timeout,
              .total_timeout = config.tts.request_timeout,
              .maximum_body_bytes = maximum_tts_encoded_bytes,
          });
    }
    audio_normalizer = std::make_unique<FfmpegAudioNormalizer>(
        config.tts.ffprobe_path, config.tts.ffmpeg_path);
    tts_cache = std::make_unique<FilesystemTtsCache>(
        config.tts.cache_directory, config.tts.cache_policy);
  }

  auto static_speech_assets =
      config.features.vox_enabled
          ? load_static_speech_assets(config.tts.fallback_directory)
          : StaticSpeechAssets{.entrance = make_vox_proof_chime(),
                               .error = make_vox_proof_chime(),
                               .farewell = make_vox_proof_chime()};

  return std::make_unique<Application>(
      ApplicationOptions{
          .persona = config.ai.persona,
          .command_prefix = config.command_prefix,
          .server_scope = config.discord.server_scope,
          .controls = config.controls,
          .features = config.features,
          .tarot_policy = config.tarot_policy,
          .wager_policy = config.wager_policy,
          .tarot_house_policy = config.tarot_house_policy,
          .tarot_deck_catalog = std::move(deck_catalog),
          .tarot_house_catalog = std::move(house_catalog),
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
          .speech =
              SpeechServiceConfiguration{
                  .provider_enabled =
                      config.tts.provider == TtsProvider::openai,
                  .usage_policy = config.tts.usage_policy,
                  .normalization_limits = config.tts.normalization_limits,
                  .request_deadline = config.tts.request_timeout,
                  .maximum_attempts = config.tts.maximum_attempts,
                  .maximum_text_scalars = config.tts.maximum_text_scalars,
                  .queue_capacity = 64,
              },
          .static_speech_assets = std::move(static_speech_assets),
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
          .wagers = std::move(wagers),
          .tarot_catalogs = std::move(tarot_catalogs),
          .tarot_draws = std::move(tarot_draws),
          .tarot_house = std::move(tarot_house),
          .tarot_integration = std::move(tarot_integration),
          .vox = std::move(vox),
          .speech = std::move(speech),
          .vox_narration = std::move(vox_narration),
          .random = std::move(random),
          .appearance_policy = config.appearance_policy,
          .ai_client = std::move(ai_client),
          .discord = std::move(discord),
          .voice_gateway = std::move(voice_gateway),
          .text_to_speech = std::move(text_to_speech),
          .audio_normalizer = std::move(audio_normalizer),
          .tts_cache = std::move(tts_cache),
      });
}

} // namespace sanguinius
