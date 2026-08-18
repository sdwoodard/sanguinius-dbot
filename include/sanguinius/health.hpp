#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sanguinius {

inline constexpr std::size_t maximum_health_message_size = 1'900;

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
  ControlConfiguration controls;
  FeatureConfiguration features;
  PersistenceHealth persistence;
  bool scope_matched{};
};

class HealthService {
public:
  HealthService(BuildInfo build, ControlConfiguration controls,
                FeatureConfiguration features, PersistenceHealth persistence);

  [[nodiscard]] HealthSnapshot snapshot(QueueSnapshot message_queue,
                                        QueueSnapshot ai_queue,
                                        bool scope_matched) const;

private:
  BuildInfo build_;
  ControlConfiguration controls_;
  FeatureConfiguration features_;
  PersistenceHealth persistence_;
};

[[nodiscard]] std::string render_health(const HealthSnapshot &snapshot);

} // namespace sanguinius
