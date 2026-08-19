#pragma once

#include "sanguinius/callback_fence.hpp"
#include "sanguinius/chronicle.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/durable_work_controls.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/pending_notice.hpp"
#include "sanguinius/repositories.hpp"
#include "sanguinius/relationships.hpp"
#include "sanguinius/work_queue.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace sanguinius {

enum class InteractionOperation {
  status,
  inbox,
  privacy,
  admin_health,
  work_recent,
  work_dead,
  test_notice,
  test_schedule_notice,
  test_public_retry,
  open_component,
  chronicle_canonize,
  chronicle_memory_preview,
  chronicle_recall,
  chronicle_timeline,
  chronicle_forget,
  chronicle_profile,
  chronicle_callbacks,
  chronicle_edit,
  chronicle_component,
};

struct RoutedInteraction {
  IncomingInteraction interaction;
  InteractionOperation operation{InteractionOperation::status};
};

class InteractionHandler {
public:
  InteractionHandler(CoreIdentityRepository &identities,
                     PendingNoticeService &notices, const Clock &clock,
                     DurableWorkControlService &durable_controls,
                     ChronicleService *chronicle,
                     HealthService &health_service, Diagnostics &diagnostics,
                     FeatureConfiguration features,
                     std::function<QueueSnapshot()> message_queue,
                     std::function<QueueSnapshot()> ai_queue,
                     std::size_t queue_capacity = 64,
                     RelationshipService *relationships = nullptr);
  ~InteractionHandler();

  InteractionHandler(const InteractionHandler &) = delete;
  InteractionHandler &operator=(const InteractionHandler &) = delete;

  void start();
  void stop() noexcept;
  [[nodiscard]] SubmitResult enqueue(RoutedInteraction interaction);
  [[nodiscard]] QueueSnapshot queue_snapshot() const;

private:
  void process(const RoutedInteraction &request);
  void ensure_user(const IncomingInteraction &interaction);
  void edit(const IncomingInteraction &interaction, InteractionMessage message,
            std::string_view diagnostic_category) const noexcept;
  void edit_reveal(const IncomingInteraction &interaction,
                   OpenPendingNoticeResult reveal,
                   std::string_view diagnostic_category) const noexcept;

  CoreIdentityRepository &identities_;
  PendingNoticeService &notices_;
  const Clock &clock_;
  DurableWorkControlService &durable_controls_;
  ChronicleService *chronicle_{};
  RelationshipService *relationships_{};
  HealthService &health_service_;
  Diagnostics &diagnostics_;
  FeatureConfiguration features_;
  std::function<QueueSnapshot()> message_queue_;
  std::function<QueueSnapshot()> ai_queue_;
  std::shared_ptr<CallbackFence> callbacks_;
  BoundedExecutor worker_;
};

} // namespace sanguinius
