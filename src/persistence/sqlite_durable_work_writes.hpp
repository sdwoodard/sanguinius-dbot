#pragma once

#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/sqlite.hpp"

#include <string>

namespace sanguinius::persistence::detail {

// These helpers deliberately do not acquire a repository mutex or start a
// transaction. Persistence repositories use them only while they already own
// the shared SQLite context lock and transaction, so domain state and durable
// effects can commit atomically without exposing SQLite in application APIs.
[[nodiscard]] bool insert_event_uncommitted(SqliteConnection &connection,
                                            const EventJournalEntry &event);
[[nodiscard]] bool insert_job_uncommitted(SqliteConnection &connection,
                                          const ScheduledJobEnqueue &job,
                                          const std::string &payload_json);
[[nodiscard]] bool insert_outbox_uncommitted(SqliteConnection &connection,
                                             const OutboxEnqueue &outbox,
                                             const std::string &payload_json);
[[nodiscard]] std::string
encode_public_payload(const PublicOutboxPayload &payload,
                      std::string_view correlation_id,
                      const std::optional<std::string> &causation_event_id);
[[nodiscard]] std::string
encode_memory_expiry_payload(const MemoryExpiryJobPayload &payload,
                             std::string_view correlation_id,
                             const std::optional<std::string> &causation_event_id);

} // namespace sanguinius::persistence::detail
