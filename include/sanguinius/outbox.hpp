#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/work_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace sanguinius {

struct DiscordDeliveryFailureDisposition {
  std::string_view error_code;
  OutboxFailureMode mode{OutboxFailureMode::failed};
  bool retry{};
};

[[nodiscard]] DiscordDeliveryFailureDisposition
classify_discord_delivery_failure(DeliveryResult result,
                                  bool within_nonce_window) noexcept;

class OutboxHandlerRegistry {
public:
  using Handler =
      std::function<void(const ClaimedOutboxMessage &, std::stop_token)>;

  void add(std::string kind, Handler handler);
  void freeze();
  [[nodiscard]] bool dispatch(const ClaimedOutboxMessage &outbox,
                              std::stop_token stop_token) const;

private:
  std::unordered_map<std::string, Handler> handlers_;
  bool frozen_{};
};

class OutboxService {
public:
  OutboxService(
      DurableWorkRepository &repository, const Clock &clock,
      PersistentIdGenerator &ids, Diagnostics &diagnostics,
      DiscordPublicDelivery &delivery,
      const DiscordStatusProvider &discord_status,
      ServerScopeConfiguration scope, std::string instance_id,
      std::size_t queue_capacity = 32,
      std::chrono::milliseconds receipt_wait_timeout = std::chrono::seconds{90},
      std::function<void()> completion_observer = {});
  ~OutboxService();

  OutboxService(const OutboxService &) = delete;
  OutboxService &operator=(const OutboxService &) = delete;

  void start();
  void stop() noexcept;
  void wake() noexcept;
  void run_one_cycle();
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void poll(std::stop_token stop_token) noexcept;
  void process_one(std::stop_token stop_token) noexcept;
  void handle_notice(const ClaimedOutboxMessage &outbox,
                     std::stop_token stop_token);
  void handle_public(const ClaimedOutboxMessage &outbox,
                     std::stop_token stop_token);
  void handle_public_edit(const ClaimedOutboxMessage &outbox,
                          std::stop_token stop_token);
  void handle_receipt(ClaimedOutboxMessage outbox,
                      PublicDeliveryReceipt receipt) noexcept;

  DurableWorkRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  DiscordPublicDelivery &delivery_;
  const DiscordStatusProvider &discord_status_;
  ServerScopeConfiguration scope_;
  std::string instance_id_;
  OutboxHandlerRegistry handlers_;
  BoundedExecutor workers_;
  std::chrono::milliseconds receipt_wait_timeout_;
  std::function<void()> completion_observer_;
  mutable std::mutex poll_mutex_;
  std::condition_variable poll_wakeup_;
  std::jthread poller_;
  bool started_{};
};

} // namespace sanguinius
