#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/build_info.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/id_generator.hpp"
#include "sanguinius/message_log.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace sanguinius {

struct ApplicationOptions {
  std::string persona;
  std::string command_prefix;
  ServerScopeConfiguration server_scope;
  ControlConfiguration controls;
  FeatureConfiguration features;
  BuildInfo build;
  std::size_t message_queue_capacity{64};
  std::size_t ai_queue_capacity{64};
  std::size_t ai_worker_count{2};
};

struct ApplicationDependencies {
  std::unique_ptr<Clock> clock;
  std::unique_ptr<IdGenerator> id_generator;
  std::unique_ptr<Diagnostics> diagnostics;
  std::unique_ptr<MessageLog> message_log;
  std::unique_ptr<AiClient> ai_client;
  std::unique_ptr<DiscordRuntime> discord;
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
