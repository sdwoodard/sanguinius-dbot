#pragma once

#include "sanguinius/relationships.hpp"
#include "sanguinius/persistence/sqlite.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace sanguinius::persistence::detail {

// Appends an immutable relationship event and advances its projection in the
// caller's transaction. A source/subject replay is an unchanged success.
[[nodiscard]] bool insert_relationship_event_uncommitted(
    SqliteConnection &connection, const std::string &relationship_event_id,
    const std::string &source_event_id, std::string_view event_type,
    std::string_view reason_code, DiscordSnowflake subject,
    RelationshipDelta requested, std::int64_t occurred_at_ms,
    std::int64_t created_at_ms);

} // namespace sanguinius::persistence::detail
