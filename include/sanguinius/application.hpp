#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/appearance_policy.hpp"
#include "sanguinius/appearances.hpp"
#include "sanguinius/build_info.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/chronicle_sessions.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/id_generator.hpp"
#include "sanguinius/message_log.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/repositories.hpp"
#include "sanguinius/relationships.hpp"
#include "sanguinius/random.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/tarot_house.hpp"
#include "sanguinius/tarot_integration.hpp"
#include "sanguinius/wagers.hpp"
#include "sanguinius/vox.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sanguinius {

struct ApplicationOptions {
  std::string persona;
  std::string command_prefix;
  ServerScopeConfiguration server_scope;
  ControlConfiguration controls;
  FeatureConfiguration features;
  TarotPolicy tarot_policy;
  WagerPolicy wager_policy;
  TarotHousePolicy tarot_house_policy{.house_enabled = false,
                                      .integration_enabled = false};
  std::optional<TarotDeckCatalog> tarot_deck_catalog;
  std::optional<TarotHouseCatalog> tarot_house_catalog;
  BuildInfo build;
  PersistenceHealth persistence;
  std::string instance_id;
  std::string timezone{"America/New_York"};
  std::string hostname;
  std::int64_t process_id{};
  std::size_t message_queue_capacity{64};
  std::size_t ai_queue_capacity{64};
  std::size_t ai_worker_count{2};
  std::size_t interaction_queue_capacity{64};
  std::chrono::milliseconds durable_delivery_receipt_wait{
      std::chrono::seconds{90}};
};

struct ApplicationDependencies {
  std::unique_ptr<Clock> clock;
  std::unique_ptr<IdGenerator> id_generator;
  std::unique_ptr<PersistentIdGenerator> persistent_id_generator;
  std::unique_ptr<Diagnostics> diagnostics;
  std::unique_ptr<MessageLog> message_log;
  std::unique_ptr<ApplicationInstanceRepository> application_instances;
  std::unique_ptr<CoreIdentityRepository> identities;
  std::unique_ptr<PendingNoticeRepository> pending_notices;
  std::unique_ptr<DurableWorkRepository> durable_work;
  std::unique_ptr<ChronicleRepository> chronicle;
  std::unique_ptr<ChronicleSessionRepository> chronicle_sessions;
  std::unique_ptr<RelationshipRepository> relationships;
  std::unique_ptr<AppearanceRepository> appearances;
  std::unique_ptr<TarotRepository> tarot;
  std::unique_ptr<TarotWagerRepository> wagers;
  std::unique_ptr<TarotCatalogRepository> tarot_catalogs;
  std::unique_ptr<TarotDrawRepository> tarot_draws;
  std::unique_ptr<TarotHouseRepository> tarot_house;
  std::unique_ptr<TarotIntegrationRepository> tarot_integration;
  std::unique_ptr<VoxRepository> vox;
  std::unique_ptr<Random> random;
  std::optional<AppearancePolicy> appearance_policy;
  std::unique_ptr<AiClient> ai_client;
  std::unique_ptr<DiscordRuntime> discord;
  std::unique_ptr<VoiceGateway> voice_gateway;
};

class Application {
public:
  Application(ApplicationOptions options, ApplicationDependencies dependencies);
  ~Application();

  Application(const Application &) = delete;
  Application &operator=(const Application &) = delete;
  Application(Application &&) = delete;
  Application &operator=(Application &&) = delete;

  void start();
  void stop() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sanguinius
