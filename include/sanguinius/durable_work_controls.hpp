#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sanguinius {

class DurableWorkControlService {
public:
  DurableWorkControlService(DurableWorkRepository &repository,
                            const Clock &clock, PersistentIdGenerator &ids,
                            ServerScopeConfiguration scope,
                            std::function<void()> scheduler_wakeup,
                            std::function<void()> outbox_wakeup);

  [[nodiscard]] bool queue_test_notice(const IncomingInteraction &interaction);
  [[nodiscard]] bool
  schedule_test_notice(const IncomingInteraction &interaction);
  [[nodiscard]] bool
  queue_test_public_retry(const IncomingInteraction &interaction);
  [[nodiscard]] bool
  record_reliability_test(const IncomingInteraction &interaction,
                          std::string_view scenario);

  [[nodiscard]] std::vector<WorkInspectionEntry> recent() const;
  [[nodiscard]] std::vector<WorkInspectionEntry> dead() const;

private:
  [[nodiscard]] NoticeOutboxPayload
  test_notice_payload(const IncomingInteraction &interaction,
                      std::int64_t created_at_ms);
  [[nodiscard]] EventJournalEntry
  control_event(const IncomingInteraction &interaction, std::string event_type,
                std::string idempotency_suffix, std::int64_t at_ms);

  DurableWorkRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  ServerScopeConfiguration scope_;
  std::function<void()> scheduler_wakeup_;
  std::function<void()> outbox_wakeup_;
};

[[nodiscard]] std::string
render_work_inspection(const std::vector<WorkInspectionEntry> &entries,
                       std::string_view heading);

} // namespace sanguinius
