#include "sanguinius/owner_admin.hpp"

#include <algorithm>
#include <cctype>

namespace sanguinius {
namespace {

[[nodiscard]] std::string lowercase(const std::string_view value) {
  std::string result{value};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

void skip_space(const std::string_view value, std::size_t &position) {
  while (position < value.size() &&
         std::isspace(static_cast<unsigned char>(value[position])) != 0) {
    ++position;
  }
}

[[nodiscard]] std::string_view token(const std::string_view value,
                                     std::size_t &position) {
  const auto start = position;
  while (position < value.size() &&
         std::isspace(static_cast<unsigned char>(value[position])) == 0) {
    ++position;
  }
  return value.substr(start, position - start);
}

} // namespace

std::optional<AdminOperation>
parse_admin_operation(const std::string_view content,
                      const std::string_view prefix) {
  if (prefix.empty() || !content.starts_with(prefix)) {
    return std::nullopt;
  }

  std::size_t position = prefix.size();
  const auto family = lowercase(token(content, position));
  skip_space(content, position);
  const auto operation = lowercase(token(content, position));
  skip_space(content, position);
  if (position != content.size() || family != "sang-admin" ||
      operation != "health") {
    return std::nullopt;
  }
  return AdminOperation::health;
}

OwnerAdminService::OwnerAdminService(ControlConfiguration controls,
                                     const ServerScopePolicy &scope_policy,
                                     const HealthService &health_service)
    : controls_{controls}, scope_policy_{scope_policy},
      health_service_{health_service} {}

AdminAuthorization OwnerAdminService::authorize(
    const ServerRequestContext &context) const noexcept {
  if (!controls_.admin_commands_enabled) {
    return {AdminStatus::disabled, ScopeRejection::none};
  }
  const auto decision = scope_policy_.authorize(context, RequiredRole::owner);
  if (!decision.allowed()) {
    return {AdminStatus::rejected, decision.rejection};
  }
  return {AdminStatus::handled, ScopeRejection::none};
}

AdminResult OwnerAdminService::handle(const AdminRequest &request,
                                      const QueueSnapshot message_queue,
                                      const QueueSnapshot ai_queue) const {
  const auto authorization = authorize(request.context);
  if (!authorization.allowed()) {
    return {.authorization = authorization, .health = std::nullopt};
  }

  switch (request.operation) {
  case AdminOperation::health:
    return {
        .authorization = authorization,
        .health = health_service_.snapshot(message_queue, ai_queue, true),
    };
  }
  return {.authorization = authorization, .health = std::nullopt};
}

const char *admin_status_name(const AdminStatus status) noexcept {
  switch (status) {
  case AdminStatus::handled:
    return "handled";
  case AdminStatus::disabled:
    return "disabled";
  case AdminStatus::rejected:
    return "rejected";
  }
  return "unknown";
}

} // namespace sanguinius
