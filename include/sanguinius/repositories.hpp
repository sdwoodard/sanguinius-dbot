#pragma once

#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
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

enum class PendingNoticeState {
  pending,
  opened,
  consumed,
  expired,
  cancelled,
};

enum class InteractionTokenKind {
  button,
  select,
  modal,
};

struct PendingNoticeContent {
  std::string title;
  std::string body;

  [[nodiscard]] bool operator==(const PendingNoticeContent &) const = default;
};

struct PendingNoticeRecord {
  std::string notice_id;
  DiscordSnowflake target_user_id;
  std::string notice_type;
  PendingNoticeContent content;
  PendingNoticeState state{PendingNoticeState::pending};
  std::optional<std::int64_t> expires_at_ms;
  std::optional<std::int64_t> opened_at_ms;
  std::optional<std::int64_t> consumed_at_ms;
  std::int64_t created_at_ms{};
};

struct CreatePendingNoticeRequest {
  std::string notice_id;
  std::string token_id;
  DiscordSnowflake target_user_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  std::string notice_type;
  PendingNoticeContent content;
  std::optional<std::string> source_aggregate_type;
  std::optional<std::string> source_aggregate_id;
  std::int64_t expires_at_ms{};
  std::string notice_idempotency_key;
  std::string token_idempotency_key;
  std::int64_t created_at_ms{};
};

struct CreatePendingNoticeResult {
  PendingNoticeRecord notice;
  std::string token_id;
  bool created{};
};

enum class OpenPendingNoticeStatus {
  opened,
  no_pending_notice,
  invalid_token,
  wrong_kind,
  wrong_scope,
  wrong_user,
  expired,
  unavailable,
};

struct OpenPendingNoticeResult {
  OpenPendingNoticeStatus status{OpenPendingNoticeStatus::unavailable};
  std::optional<PendingNoticeRecord> notice;
  std::optional<std::string> delivery_idempotency_key;
};

struct OpenNoticeByTokenRequest {
  std::string token_id;
  InteractionTokenKind interaction_kind{InteractionTokenKind::button};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake user_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

struct OpenNextNoticeRequest {
  DiscordSnowflake user_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

enum class PendingNoticeMutationStatus {
  applied,
  unchanged,
  not_found,
  wrong_user,
  invalid_state,
  expired,
};

class PendingNoticeRepository {
public:
  virtual ~PendingNoticeRepository() = default;

  [[nodiscard]] virtual CreatePendingNoticeResult
  create_with_token(const CreatePendingNoticeRequest &request) = 0;
  [[nodiscard]] virtual OpenPendingNoticeResult
  open_by_token(const OpenNoticeByTokenRequest &request) = 0;
  [[nodiscard]] virtual OpenPendingNoticeResult
  open_next(const OpenNextNoticeRequest &request) = 0;
  [[nodiscard]] virtual PendingNoticeMutationStatus
  confirm_open_delivery(const std::string &interaction_idempotency_key,
                        std::int64_t now_ms) = 0;
  [[nodiscard]] virtual PendingNoticeMutationStatus
  release_open_delivery(const std::string &interaction_idempotency_key,
                        std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t
  recover_incomplete_open_deliveries(std::int64_t now_ms) = 0;
  [[nodiscard]] virtual PendingNoticeMutationStatus
  consume(const std::string &notice_id, const DiscordSnowflake &user_id,
          std::int64_t now_ms) = 0;
  [[nodiscard]] virtual PendingNoticeMutationStatus
  cancel(const std::string &notice_id, const DiscordSnowflake &user_id,
         std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t expire_due(std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t
  pending_count(const DiscordSnowflake &user_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t pending_count_all(std::int64_t now_ms) = 0;
};

[[nodiscard]] const char *
application_stop_reason_name(ApplicationStopReason reason) noexcept;
[[nodiscard]] const char *
pending_notice_state_name(PendingNoticeState state) noexcept;
[[nodiscard]] const char *
interaction_token_kind_name(InteractionTokenKind kind) noexcept;

} // namespace sanguinius
