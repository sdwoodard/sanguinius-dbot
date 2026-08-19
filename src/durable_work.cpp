#include "sanguinius/durable_work.hpp"

#include "sanguinius/persistent_id.hpp"

#include <stdexcept>

namespace sanguinius {

const char *scheduled_job_state_name(const ScheduledJobState state) noexcept {
  switch (state) {
  case ScheduledJobState::pending:
    return "pending";
  case ScheduledJobState::claimed:
    return "claimed";
  case ScheduledJobState::completed:
    return "completed";
  case ScheduledJobState::cancelled:
    return "cancelled";
  case ScheduledJobState::dead:
    return "dead";
  }
  return "dead";
}

const char *outbox_state_name(const OutboxState state) noexcept {
  switch (state) {
  case OutboxState::pending:
    return "pending";
  case OutboxState::claimed:
    return "claimed";
  case OutboxState::delivered:
    return "delivered";
  case OutboxState::failed:
    return "failed";
  case OutboxState::dead:
    return "dead";
  case OutboxState::cancelled:
    return "cancelled";
  }
  return "dead";
}

std::string discord_nonce_from_uuid(const std::string_view uuid) {
  if (!valid_uuid_v4(std::string{uuid})) {
    throw std::invalid_argument{"Discord nonce source must be a UUIDv4."};
  }
  std::string nonce;
  std::string compact;
  compact.reserve(32);
  for (const char character : uuid) {
    if (character != '-') {
      compact.push_back(character);
    }
  }
  nonce.reserve(25);
  // Preserve 100 random UUID bits while sampling both ends. Taking only a
  // prefix would make sequential test UUIDs—and some structured generators—
  // collide despite distinct durable row IDs.
  nonce.append(compact, 0, 12);
  nonce.append(compact, compact.size() - 13, 13);
  return nonce;
}

std::string shortened_persistent_id(const std::string_view value) {
  constexpr std::size_t prefix = 8;
  constexpr std::size_t suffix = 4;
  if (value.size() <= prefix + suffix) {
    return std::string{value};
  }
  return std::string{value.substr(0, prefix)} + "..." +
         std::string{value.substr(value.size() - suffix)};
}

} // namespace sanguinius
