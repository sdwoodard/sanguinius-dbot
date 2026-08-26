#pragma once

#include "sanguinius/application.hpp"

#include "support/fake_ai_client.hpp"
#include "support/fake_appearance_repository.hpp"
#include "support/fake_chronicle_repository.hpp"
#include "support/fake_chronicle_session_repository.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_discord.hpp"
#include "support/fake_durable_work_repository.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_message_log.hpp"
#include "support/fake_random.hpp"
#include "support/fake_relationship_repository.hpp"
#include "support/fake_speech.hpp"
#include "support/fake_repositories.hpp"
#include "support/fake_tarot_house_repository.hpp"
#include "support/fake_tarot_repository.hpp"
#include "support/fake_wager_repository.hpp"
#include "support/fake_vox.hpp"

#include <memory>
#include <utility>

namespace sanguinius::test {

class ApplicationFixture {
public:
  explicit ApplicationFixture(
      ApplicationOptions options = ApplicationOptions{
          .persona = "test persona",
          .command_prefix = "!",
          .server_scope = {10, 20, 30},
          .controls = {},
          .features = {},
          .tarot_policy = {},
          .wager_policy = {},
          .tarot_house_policy = {.house_enabled = false,
                                 .integration_enabled = false},
          .tarot_deck_catalog = std::nullopt,
          .tarot_house_catalog = std::nullopt,
          .build = {"test-version", "test-revision"},
          .persistence = {true, 1, 1, "3.53.4",
                          "00000000-0000-4000-8000-000000000001"},
          .instance_id = "00000000-0000-4000-8000-000000000001",
          .hostname = "test-host",
          .process_id = 123,
          .message_queue_capacity = 64,
          .ai_queue_capacity = 64,
          .ai_worker_count = 2,
          .interaction_queue_capacity = 64,
          .durable_delivery_receipt_wait = std::chrono::milliseconds{100},
          .speech = {},
          .static_speech_assets =
              {.entrance = make_vox_proof_chime(),
               .error = make_vox_proof_chime(),
               .farewell = make_vox_proof_chime()},
      }) {
    auto owned_clock = std::make_unique<FakeClock>();
    auto owned_ids = std::make_unique<FakeIdGenerator>(std::vector<std::string>{
        "correlation-1", "correlation-2", "correlation-3", "correlation-4"});
    auto owned_persistent_ids = std::make_unique<FakePersistentIdGenerator>(
        std::vector<std::string>{"00000000-0000-4000-8000-000000000101",
                                 "00000000-0000-4000-8000-000000000102",
                                 "00000000-0000-4000-8000-000000000103",
                                 "00000000-0000-4000-8000-000000000104"});
    auto owned_diagnostics = std::make_unique<FakeDiagnostics>();
    auto owned_log = std::make_unique<FakeMessageLog>();
    auto owned_instances =
        std::make_unique<FakeApplicationInstanceRepository>();
    auto owned_identities = std::make_unique<FakeCoreIdentityRepository>();
    auto owned_notices = std::make_unique<FakePendingNoticeRepository>();
    auto owned_durable_work =
        std::make_unique<FakeDurableWorkRepository>(*owned_notices);
    auto owned_chronicle = std::make_unique<FakeChronicleRepository>();
    auto owned_chronicle_sessions =
        std::make_unique<FakeChronicleSessionRepository>();
    auto owned_relationships = std::make_unique<FakeRelationshipRepository>();
    auto owned_appearances = std::make_unique<FakeAppearanceRepository>();
    auto owned_tarot = std::make_unique<FakeTarotRepository>();
    auto owned_wagers = std::make_unique<FakeWagerRepository>();
    auto owned_tarot_catalogs = std::make_unique<FakeTarotCatalogRepository>();
    auto owned_tarot_draws = std::make_unique<FakeTarotDrawRepository>();
    auto owned_tarot_house = std::make_unique<FakeTarotHouseRepository>();
    auto owned_tarot_integration =
        std::make_unique<FakeTarotIntegrationRepository>();
    auto owned_vox = std::make_unique<FakeVoxRepository>();
    auto owned_speech = std::make_unique<FakeSpeechRepository>();
    auto owned_random =
        std::make_unique<FakeRandom>(std::vector<std::uint64_t>(128, 0));
    auto owned_ai = std::make_unique<FakeAiClient>();
    auto owned_discord = std::make_unique<FakeDiscord>();
    auto owned_voice_gateway = std::make_unique<FakeVoiceGateway>();
    auto owned_audio_normalizer = std::make_unique<FakeAudioNormalizer>();
    auto owned_tts_cache = std::make_unique<FakeTtsCache>();

    clock = owned_clock.get();
    ids = owned_ids.get();
    diagnostics = owned_diagnostics.get();
    log = owned_log.get();
    instances = owned_instances.get();
    identities = owned_identities.get();
    notices = owned_notices.get();
    durable_work = owned_durable_work.get();
    chronicle = owned_chronicle.get();
    chronicle_sessions = owned_chronicle_sessions.get();
    relationships = owned_relationships.get();
    appearances = owned_appearances.get();
    tarot = owned_tarot.get();
    wagers = owned_wagers.get();
    tarot_catalogs = owned_tarot_catalogs.get();
    tarot_draws = owned_tarot_draws.get();
    tarot_house = owned_tarot_house.get();
    tarot_integration = owned_tarot_integration.get();
    vox = owned_vox.get();
    ai = owned_ai.get();
    discord = owned_discord.get();
    voice_gateway = owned_voice_gateway.get();

    application = std::make_unique<Application>(
        std::move(options),
        ApplicationDependencies{
            .clock = std::move(owned_clock),
            .id_generator = std::move(owned_ids),
            .persistent_id_generator = std::move(owned_persistent_ids),
            .diagnostics = std::move(owned_diagnostics),
            .message_log = std::move(owned_log),
            .application_instances = std::move(owned_instances),
            .identities = std::move(owned_identities),
            .pending_notices = std::move(owned_notices),
            .durable_work = std::move(owned_durable_work),
            .chronicle = std::move(owned_chronicle),
            .chronicle_sessions = std::move(owned_chronicle_sessions),
            .relationships = std::move(owned_relationships),
            .appearances = std::move(owned_appearances),
            .tarot = std::move(owned_tarot),
            .wagers = std::move(owned_wagers),
            .tarot_catalogs = std::move(owned_tarot_catalogs),
            .tarot_draws = std::move(owned_tarot_draws),
            .tarot_house = std::move(owned_tarot_house),
            .tarot_integration = std::move(owned_tarot_integration),
            .vox = std::move(owned_vox),
            .speech = std::move(owned_speech),
            .random = std::move(owned_random),
            .appearance_policy = test_appearance_policy(),
            .ai_client = std::move(owned_ai),
            .discord = std::move(owned_discord),
            .voice_gateway = std::move(owned_voice_gateway),
            .text_to_speech = nullptr,
            .audio_normalizer = std::move(owned_audio_normalizer),
            .tts_cache = std::move(owned_tts_cache),
        });
  }

  ~ApplicationFixture() {
    // A failed assertion can unwind a test before it manually releases a
    // blocked log. Release it before Application destruction joins the message
    // worker so Catch2 can report the original failure instead of hanging.
    if (application) {
      log->release();
      identities->release();
    }
  }

  FakeClock *clock{};
  FakeIdGenerator *ids{};
  FakeDiagnostics *diagnostics{};
  FakeMessageLog *log{};
  FakeApplicationInstanceRepository *instances{};
  FakeCoreIdentityRepository *identities{};
  FakePendingNoticeRepository *notices{};
  FakeDurableWorkRepository *durable_work{};
  FakeChronicleRepository *chronicle{};
  FakeChronicleSessionRepository *chronicle_sessions{};
  FakeRelationshipRepository *relationships{};
  FakeAppearanceRepository *appearances{};
  FakeTarotRepository *tarot{};
  FakeWagerRepository *wagers{};
  FakeTarotCatalogRepository *tarot_catalogs{};
  FakeTarotDrawRepository *tarot_draws{};
  FakeTarotHouseRepository *tarot_house{};
  FakeTarotIntegrationRepository *tarot_integration{};
  FakeVoxRepository *vox{};
  FakeAiClient *ai{};
  FakeDiscord *discord{};
  FakeVoiceGateway *voice_gateway{};
  std::unique_ptr<Application> application;
};

} // namespace sanguinius::test
