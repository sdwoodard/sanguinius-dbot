#pragma once

#include "sanguinius/application.hpp"

#include "support/fake_ai_client.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_discord.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_message_log.hpp"

#include <memory>
#include <utility>

namespace sanguinius::test {

class ApplicationFixture {
public:
  explicit ApplicationFixture(ApplicationOptions options = ApplicationOptions{
                                  .persona = "test persona",
                                  .command_prefix = "!",
                                  .message_queue_capacity = 64,
                                  .ai_queue_capacity = 64,
                                  .ai_worker_count = 2,
                              }) {
    auto owned_clock = std::make_unique<FakeClock>();
    auto owned_ids = std::make_unique<FakeIdGenerator>(std::vector<std::string>{
        "correlation-1", "correlation-2", "correlation-3", "correlation-4"});
    auto owned_diagnostics = std::make_unique<FakeDiagnostics>();
    auto owned_log = std::make_unique<FakeMessageLog>();
    auto owned_ai = std::make_unique<FakeAiClient>();
    auto owned_discord = std::make_unique<FakeDiscord>();

    clock = owned_clock.get();
    ids = owned_ids.get();
    diagnostics = owned_diagnostics.get();
    log = owned_log.get();
    ai = owned_ai.get();
    discord = owned_discord.get();

    application = std::make_unique<Application>(
        std::move(options), ApplicationDependencies{
                                .clock = std::move(owned_clock),
                                .id_generator = std::move(owned_ids),
                                .diagnostics = std::move(owned_diagnostics),
                                .message_log = std::move(owned_log),
                                .ai_client = std::move(owned_ai),
                                .discord = std::move(owned_discord),
                            });
  }

  ~ApplicationFixture() {
    // A failed assertion can unwind a test before it manually releases a
    // blocked log. Release it before Application destruction joins the message
    // worker so Catch2 can report the original failure instead of hanging.
    if (application) {
      log->release();
    }
  }

  FakeClock *clock{};
  FakeIdGenerator *ids{};
  FakeDiagnostics *diagnostics{};
  FakeMessageLog *log{};
  FakeAiClient *ai{};
  FakeDiscord *discord{};
  std::unique_ptr<Application> application;
};

} // namespace sanguinius::test
