#pragma once

#include "sanguinius/build_info.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <string>

namespace sanguinius {

inline constexpr std::size_t maximum_health_message_size = 1'900;

struct HealthSnapshot {
  BuildInfo build;
  QueueSnapshot message_queue;
  QueueSnapshot ai_queue;
  ControlConfiguration controls;
  FeatureConfiguration features;
  bool scope_matched{};
};

class HealthService {
public:
  HealthService(BuildInfo build, ControlConfiguration controls,
                FeatureConfiguration features);

  [[nodiscard]] HealthSnapshot snapshot(QueueSnapshot message_queue,
                                        QueueSnapshot ai_queue,
                                        bool scope_matched) const;

private:
  BuildInfo build_;
  ControlConfiguration controls_;
  FeatureConfiguration features_;
};

[[nodiscard]] std::string render_health(const HealthSnapshot &snapshot);

} // namespace sanguinius
