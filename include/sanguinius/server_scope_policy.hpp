#pragma once

#include "sanguinius/snowflake.hpp"

namespace sanguinius {

struct ServerScopeConfiguration {
  DiscordSnowflake guild_id;
  DiscordSnowflake primary_channel_id;
  DiscordSnowflake owner_user_id;
};

struct ServerRequestContext {
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake user_id;
};

enum class RequiredRole {
  member,
  owner,
};

enum class ScopeRejection {
  none,
  wrong_guild,
  wrong_channel,
  owner_required,
};

struct ScopeDecision {
  ScopeRejection rejection{ScopeRejection::none};

  [[nodiscard]] bool allowed() const noexcept {
    return rejection == ScopeRejection::none;
  }
};

class ServerScopePolicy {
public:
  explicit ServerScopePolicy(ServerScopeConfiguration configuration);

  [[nodiscard]] ScopeDecision authorize(const ServerRequestContext &context,
                                        RequiredRole role) const noexcept;

private:
  ServerScopeConfiguration configuration_;
};

[[nodiscard]] const char *
scope_rejection_name(ScopeRejection rejection) noexcept;

} // namespace sanguinius
