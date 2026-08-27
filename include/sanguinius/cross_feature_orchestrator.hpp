#pragma once

#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sanguinius {

struct CrossFeatureConsumerHealth {
  std::string name;
  bool degraded{};
  bool backlog{};
  std::uint64_t failures{};
};

struct CrossFeatureHealth {
  bool running{};
  std::size_t consumer_count{};
  std::uint64_t completed_passes{};
  std::uint64_t failed_consumers{};
  std::vector<CrossFeatureConsumerHealth> consumers;
};

class CrossFeatureOrchestrator {
public:
  using Consumer = std::function<bool()>;

  explicit CrossFeatureOrchestrator(
      Diagnostics &diagnostics,
      std::chrono::milliseconds recovery_interval = std::chrono::minutes{1});
  ~CrossFeatureOrchestrator();

  CrossFeatureOrchestrator(const CrossFeatureOrchestrator &) = delete;
  CrossFeatureOrchestrator &
  operator=(const CrossFeatureOrchestrator &) = delete;

  void add_consumer(std::string name, Consumer consumer);
  void start();
  void stop() noexcept;
  void wake() noexcept;
  [[nodiscard]] CrossFeatureHealth health() const;

private:
  struct RegisteredConsumer {
    std::string name;
    Consumer run;
    bool degraded{};
    bool backlog{};
    std::uint64_t failures{};
  };

  void loop();
  [[nodiscard]] bool run_pass() noexcept;

  Diagnostics &diagnostics_;
  std::chrono::milliseconds recovery_interval_;
  std::vector<RegisteredConsumer> consumers_;
  mutable std::mutex mutex_;
  std::condition_variable wakeup_;
  std::thread worker_;
  bool running_{};
  bool stopping_{};
  bool wake_requested_{};
  std::uint64_t completed_passes_{};
  std::uint64_t failed_consumers_{};
};

class MessageObservationPipeline {
public:
  using Observer = std::function<void(const IncomingMessage &)>;

  explicit MessageObservationPipeline(Diagnostics &diagnostics);
  void add_observer(std::string name, Observer observer);
  void observe(const IncomingMessage &message) const noexcept;

private:
  struct RegisteredObserver {
    std::string name;
    Observer observe;
  };

  Diagnostics &diagnostics_;
  std::vector<RegisteredObserver> observers_;
};

} // namespace sanguinius
