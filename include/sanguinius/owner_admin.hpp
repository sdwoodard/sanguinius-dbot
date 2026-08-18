#pragma once

#include "sanguinius/feature_config.hpp"
#include "sanguinius/health.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <optional>
#include <string_view>

namespace sanguinius {

enum class AdminOperation {
  health,
};

[[nodiscard]] std::optional<AdminOperation>
parse_admin_operation(std::string_view content, std::string_view prefix);

struct AdminRequest {
  ServerRequestContext context;
  AdminOperation operation{AdminOperation::health};
};

enum class AdminStatus {
  handled,
  disabled,
  rejected,
};

struct AdminAuthorization {
  AdminStatus status{AdminStatus::disabled};
  ScopeRejection rejection{ScopeRejection::none};

  [[nodiscard]] bool allowed() const noexcept {
    return status == AdminStatus::handled;
  }
};

struct AdminResult {
  AdminAuthorization authorization;
  std::optional<HealthSnapshot> health;
};

class OwnerAdminService {
public:
  OwnerAdminService(ControlConfiguration controls,
                    const ServerScopePolicy &scope_policy,
                    const HealthService &health_service);

  [[nodiscard]] AdminAuthorization
  authorize(const ServerRequestContext &context) const noexcept;
  [[nodiscard]] AdminResult handle(const AdminRequest &request,
                                   QueueSnapshot message_queue,
                                   QueueSnapshot ai_queue) const;

private:
  ControlConfiguration controls_;
  const ServerScopePolicy &scope_policy_;
  const HealthService &health_service_;
};

[[nodiscard]] const char *admin_status_name(AdminStatus status) noexcept;

} // namespace sanguinius
