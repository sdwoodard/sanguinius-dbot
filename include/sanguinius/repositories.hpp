#pragma once

#include "sanguinius/server_scope_policy.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sanguinius {

enum class ApplicationStopReason {
  clean_shutdown,
  startup_failure,
};

struct ApplicationInstanceRecord {
  std::string instance_id;
  std::string application_version;
  std::string git_revision;
  std::string hostname;
  std::int64_t process_id{};
  std::int64_t started_at_ms{};
};

class ApplicationInstanceRepository {
public:
  virtual ~ApplicationInstanceRepository() = default;
  virtual void record_start(const ApplicationInstanceRecord &record) = 0;
  virtual void record_stop(const std::string &instance_id,
                           std::int64_t stopped_at_ms,
                           ApplicationStopReason reason) = 0;
};

struct DiscordUserRecord {
  DiscordSnowflake user_id;
  std::optional<std::string> display_name;
  std::optional<std::string> username;
  bool is_bot{};
  std::int64_t observed_at_ms{};
};

struct UserPreferences {
  bool chronicle_opt_in{};
  bool memory_callback_opt_in{};
  bool appearance_callback_opt_in{};
  bool voice_input_opt_in{};
  bool public_tarot_results_opt_in{true};
  std::optional<std::int64_t> quiet_until_ms;
  std::int64_t updated_at_ms{};
};

class CoreIdentityRepository {
public:
  virtual ~CoreIdentityRepository() = default;
  virtual void
  initialize_or_validate_scope(const ServerScopeConfiguration &scope,
                               std::int64_t now_ms) = 0;
  virtual void ensure_user(const DiscordUserRecord &user) = 0;
  [[nodiscard]] virtual std::optional<UserPreferences>
  load_preferences(const DiscordSnowflake &user_id) = 0;
};

[[nodiscard]] const char *
application_stop_reason_name(ApplicationStopReason reason) noexcept;

} // namespace sanguinius
