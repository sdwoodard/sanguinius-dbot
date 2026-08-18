#include "sanguinius/server_scope_policy.hpp"

#include <stdexcept>

namespace sanguinius {

ServerScopePolicy::ServerScopePolicy(ServerScopeConfiguration configuration)
    : configuration_{configuration} {
  if (!configuration_.guild_id.is_set() ||
      !configuration_.primary_channel_id.is_set() ||
      !configuration_.owner_user_id.is_set()) {
    throw std::invalid_argument{"Server scope IDs must all be configured."};
  }
}

ScopeDecision
ServerScopePolicy::authorize(const ServerRequestContext &context,
                             const RequiredRole role) const noexcept {
  if (context.guild_id != configuration_.guild_id) {
    return {ScopeRejection::wrong_guild};
  }
  if (context.channel_id != configuration_.primary_channel_id) {
    return {ScopeRejection::wrong_channel};
  }
  if (role == RequiredRole::owner &&
      context.user_id != configuration_.owner_user_id) {
    return {ScopeRejection::owner_required};
  }
  return {};
}

const char *scope_rejection_name(const ScopeRejection rejection) noexcept {
  switch (rejection) {
  case ScopeRejection::none:
    return "none";
  case ScopeRejection::wrong_guild:
    return "wrong_guild";
  case ScopeRejection::wrong_channel:
    return "wrong_channel";
  case ScopeRejection::owner_required:
    return "owner_required";
  }
  return "unknown";
}

} // namespace sanguinius
