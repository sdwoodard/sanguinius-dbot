#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/work_queue.hpp"
#include "sanguinius/tarot.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sanguinius {

inline constexpr std::size_t maximum_health_message_size = 1'900;

[[nodiscard]] std::string bounded_health_message(std::string message);

struct PersistenceHealth {
  bool ready{};
  std::int64_t schema_version{};
  std::int64_t target_schema_version{};
  std::string sqlite_version;
  std::string instance_id;
};

struct HealthSnapshot {
  BuildInfo build;
  QueueSnapshot message_queue;
  QueueSnapshot ai_queue;
  QueueSnapshot interaction_queue;
  QueueSnapshot scheduler_queue;
  QueueSnapshot outbox_queue;
  ControlConfiguration controls;
  FeatureConfiguration features;
  PersistenceHealth persistence;
  DiscordRuntimeStatus discord;
  std::size_t pending_notice_count{};
  DurableWorkHealth durable_work;
  std::optional<TarotInvariantReport> tarot;
  bool scope_matched{};
};

struct HealthRuntimeProviders {
  const DiscordStatusProvider *discord_status{};
  std::function<QueueSnapshot()> interaction_queue;
  std::function<QueueSnapshot()> scheduler_queue;
  std::function<QueueSnapshot()> outbox_queue;
  std::function<std::size_t()> pending_notice_count;
  std::function<DurableWorkHealth()> durable_work;
  std::function<std::optional<TarotInvariantReport>()> tarot;
};

class HealthService {
public:
  HealthService(BuildInfo build, ControlConfiguration controls,
                FeatureConfiguration features, PersistenceHealth persistence,
                HealthRuntimeProviders runtime);

  [[nodiscard]] HealthSnapshot snapshot(QueueSnapshot message_queue,
                                        QueueSnapshot ai_queue,
                                        bool scope_matched) const;

private:
  BuildInfo build_;
  ControlConfiguration controls_;
  FeatureConfiguration features_;
  PersistenceHealth persistence_;
  HealthRuntimeProviders runtime_;
};

[[nodiscard]] std::string render_health(const HealthSnapshot &snapshot);
[[nodiscard]] const char *
command_registration_state_name(CommandRegistrationState state) noexcept;

} // namespace sanguinius
