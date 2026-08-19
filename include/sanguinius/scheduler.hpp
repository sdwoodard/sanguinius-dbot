#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/work_queue.hpp"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace sanguinius {

class JobHandlerRegistry {
public:
  using Handler = std::function<void(const ClaimedScheduledJob &)>;

  void add(std::string type, Handler handler);
  void freeze();
  [[nodiscard]] bool dispatch(const ClaimedScheduledJob &job) const;

private:
  std::unordered_map<std::string, Handler> handlers_;
  bool frozen_{};
};

class SchedulerService {
public:
  SchedulerService(DurableWorkRepository &repository, const Clock &clock,
                   PersistentIdGenerator &ids, Diagnostics &diagnostics,
                   std::string instance_id, std::function<void()> outbox_wakeup,
                   std::size_t queue_capacity = 32,
                   JobHandlerRegistry::Handler chronicle_expiry_handler = {});
  ~SchedulerService();

  SchedulerService(const SchedulerService &) = delete;
  SchedulerService &operator=(const SchedulerService &) = delete;

  void start();
  void stop() noexcept;
  void wake() noexcept;
  void run_one_cycle();
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void poll(std::stop_token stop_token) noexcept;
  void process_one(std::stop_token stop_token) noexcept;
  void handle_test_notice(const ClaimedScheduledJob &job);

  DurableWorkRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  std::string instance_id_;
  std::function<void()> outbox_wakeup_;
  JobHandlerRegistry handlers_;
  BoundedExecutor workers_;
  mutable std::mutex poll_mutex_;
  std::condition_variable poll_wakeup_;
  std::jthread poller_;
  bool started_{};
};

} // namespace sanguinius
