#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/feature_config.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace sanguinius {

inline constexpr std::size_t maximum_chronicle_title_size = 100;
inline constexpr std::size_t maximum_chronicle_body_size = 1'000;
inline constexpr std::size_t maximum_chronicle_source_size = 2'000;
inline constexpr std::size_t maximum_memory_text_size = 500;
inline constexpr std::size_t maximum_chronicle_tags = 5;
inline constexpr std::size_t maximum_chronicle_attachments = 10;
inline constexpr std::size_t maximum_chronicle_mentions = 10;
inline constexpr std::string_view chronicle_component_prefix{"sgc:1:"};
inline constexpr std::string_view chronicle_modal_prefix{"sgm:1:"};
inline constexpr std::string_view memory_draft_component_prefix{"sgd:1:"};
inline constexpr std::string_view memory_expiry_job_type{
    "chronicle.memory-expire.v1"};

enum class ChronicleEntryType {
  quote,
  deed,
  prediction,
  incident,
  custom,
};

enum class ChronicleVisibility {
  shared,
  participant_only,
};

enum class ChronicleEntryStatus {
  proposed,
  canon,
  retracted,
};

enum class MemoryVisibility {
  shared,
  self_only,
};

enum class MemorySensitivity {
  ordinary,
  personal,
  sensitive,
};

enum class MemoryStatus {
  confirmed,
  retracted,
  expired,
};

enum class ChronicleEntryAction {
  edit,
  submit,
  approval_completed,
  decline,
  retract,
};

enum class MemoryAction {
  retract,
  expire,
};

struct ChronicleEntryTransition {
  ChronicleEntryStatus status{ChronicleEntryStatus::proposed};
  bool submitted{};
  bool changed{};
};

[[nodiscard]] std::optional<ChronicleEntryTransition>
transition_chronicle_entry(ChronicleEntryStatus status, bool submitted,
                           ChronicleEntryAction action,
                           bool approvals_remaining = false) noexcept;
[[nodiscard]] std::optional<MemoryStatus>
transition_memory(MemoryStatus status, MemoryAction action) noexcept;

enum class ChronicleResultCode {
  created,
  existing,
  updated,
  unchanged,
  not_found,
  unauthorized,
  opted_out,
  invalid_state,
  stale_revision,
  expired,
  invalid_token,
};

struct ChronicleAttachment {
  DiscordSnowflake attachment_id;
  std::string filename;
  std::optional<std::string> content_type{};
  std::uint64_t byte_size{};
  std::optional<std::uint32_t> width{};
  std::optional<std::uint32_t> height{};
  bool ephemeral{};
  bool spoiler{};
};

struct ChronicleEntry {
  std::string entry_id;
  ChronicleEntryType type{ChronicleEntryType::quote};
  std::string title;
  std::string body;
  ChronicleVisibility visibility{ChronicleVisibility::shared};
  ChronicleEntryStatus status{ChronicleEntryStatus::proposed};
  DiscordSnowflake created_by_user_id;
  DiscordSnowflake source_author_user_id;
  DiscordSnowflake source_guild_id;
  DiscordSnowflake source_channel_id;
  DiscordSnowflake source_message_id;
  std::string source_text;
  bool source_text_truncated{};
  std::int64_t occurred_at_ms{};
  std::int64_t created_at_ms{};
  std::optional<std::int64_t> submitted_at_ms{};
  std::optional<std::int64_t> approved_at_ms{};
  std::optional<std::int64_t> retracted_at_ms{};
  std::size_t revision{1};
  std::vector<DiscordSnowflake> participants{};
  std::vector<std::string> tags{};
  std::vector<ChronicleAttachment> attachments{};
};

struct ExplicitMemory {
  std::string memory_id;
  std::string text;
  MemoryVisibility visibility{MemoryVisibility::shared};
  MemorySensitivity sensitivity{MemorySensitivity::ordinary};
  MemoryStatus status{MemoryStatus::confirmed};
  DiscordSnowflake subject_user_id;
  DiscordSnowflake created_by_user_id;
  std::int64_t created_at_ms{};
  std::optional<std::int64_t> expires_at_ms{};
  std::size_t revision{1};
};

struct ProposalActionIds {
  std::string edit_token_id;
  std::string submit_token_id;
  std::string retract_token_id;
};

enum class ProposalControlMode {
  edit_submit_retract,
  owner_stale_resolution,
  awaiting_confirmations,
  confirmations_reissued,
};

struct ApprovalRenewalDispatch {
  std::string notice_id;
  std::string notice_open_token_id;
  std::string approve_token_id;
  std::string decline_token_id;
  std::string notice_event_id;
  std::string notice_outbox_id;
};

struct CreateProposalRequest {
  std::string entry_id;
  std::string event_id;
  ProposalActionIds actions;
  ContextMessageSnapshot source;
  DiscordSnowflake proposer_user_id;
  DiscordSnowflake owner_user_id{};
  std::string title;
  std::string body;
  ChronicleEntryType type{ChronicleEntryType::quote};
  ChronicleVisibility visibility{ChronicleVisibility::shared};
  bool owner_test{};
  std::string correlation_id;
  std::string idempotency_key;
  std::int64_t now_ms{};
  std::int64_t action_expires_at_ms{};
  std::int64_t notice_expires_at_ms{};
  std::vector<ApprovalRenewalDispatch> renewal_dispatches{};
};

struct ProposalResult {
  ChronicleResultCode code{ChronicleResultCode::invalid_state};
  std::optional<ChronicleEntry> entry{};
  std::optional<ProposalActionIds> actions{};
  ProposalControlMode control_mode{ProposalControlMode::edit_submit_retract};
  bool wake_outbox{};
};

struct EditProposalRequest {
  std::string token_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  std::string title;
  std::string body;
  ChronicleEntryType type{ChronicleEntryType::quote};
  ChronicleVisibility visibility{ChronicleVisibility::shared};
  std::vector<std::string> tags{};
  std::string event_id;
  std::string correlation_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

struct SubmitProposalRequest {
  struct ReviewerDispatch {
    std::string approval_id;
    std::string notice_id;
    std::string notice_open_token_id;
    std::string approve_token_id;
    std::string decline_token_id;
    std::string notice_event_id;
    std::string notice_outbox_id;
  };

  std::string token_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string proposer_approval_id;
  std::string submit_event_id;
  std::string immediate_canon_event_id;
  std::vector<ReviewerDispatch> reviewer_dispatches{};
  std::string correlation_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
  std::int64_t notice_expires_at_ms{};
};

struct ApplyApprovalRequest {
  std::string token_id;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string action_event_id;
  std::string canon_event_id;
  std::string public_outbox_id;
  std::string correlation_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

struct ChronicleMutationResult {
  ChronicleResultCode code{ChronicleResultCode::invalid_state};
  std::optional<ChronicleEntry> entry{};
  bool became_canon{};
  bool wake_outbox{};
  bool wake_scheduler{};
  bool draft_cancelled{};
};

struct MemoryDraft {
  std::string text;
  std::vector<std::string> tags{};
  MemoryVisibility visibility{MemoryVisibility::shared};
  MemorySensitivity sensitivity{MemorySensitivity::ordinary};
  std::optional<std::int64_t> expires_at_ms{};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake user_id;
};

struct ConfirmMemoryRequest {
  std::string memory_id;
  std::string event_id;
  std::optional<std::string> expiry_job_id{};
  MemoryDraft draft;
  std::string correlation_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

struct RetractItemRequest {
  std::string entity_id;
  std::size_t expected_revision{};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake actor_user_id;
  DiscordSnowflake owner_user_id;
  std::string event_id;
  std::string public_outbox_id;
  std::string correlation_id;
  std::string interaction_idempotency_key;
  std::int64_t now_ms{};
};

enum class ManageableKind {
  memory,
  entry,
};

struct ManageableChronicleItem {
  ManageableKind kind{ManageableKind::memory};
  std::string entity_id;
  std::size_t revision{};
  std::string summary;
};

struct RecallResults {
  std::vector<ChronicleEntry> entries{};
  std::vector<ExplicitMemory> memories{};
  std::vector<std::variant<ChronicleEntry, ExplicitMemory>> ordered_items{};
};

class ChronicleRepository {
public:
  virtual ~ChronicleRepository() = default;

  [[nodiscard]] virtual ProposalResult
  create_or_get_proposal(const CreateProposalRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  edit_proposal(const EditProposalRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  submit_proposal(const SubmitProposalRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  apply_approval(const ApplyApprovalRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  confirm_memory(const ConfirmMemoryRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  retract_memory(const RetractItemRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  retract_entry(const RetractItemRequest &request) = 0;
  [[nodiscard]] virtual ChronicleMutationResult
  expire_memory(const ClaimedScheduledJob &job, std::string event_id,
                std::int64_t now_ms) = 0;
  [[nodiscard]] virtual RecallResults recall(const DiscordSnowflake &viewer,
                                             std::string_view query,
                                             std::int64_t now_ms,
                                             std::size_t limit) = 0;
  [[nodiscard]] virtual std::vector<ChronicleEntry>
  timeline(std::optional<std::int64_t> since_ms, std::int64_t now_ms,
           std::size_t limit) = 0;
  [[nodiscard]] virtual std::vector<ManageableChronicleItem>
  manageable(const DiscordSnowflake &viewer, const DiscordSnowflake &owner,
             std::string_view reference, std::int64_t now_ms,
             std::size_t limit) = 0;
};

enum class VolatileActionKind {
  confirm_memory,
  cancel_memory,
  retract_memory,
  retract_entry,
};

struct VolatileAction {
  VolatileActionKind kind{VolatileActionKind::confirm_memory};
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake expected_user_id;
  std::int64_t expires_at_ms{};
  std::optional<MemoryDraft> memory{};
  std::optional<ManageableChronicleItem> item{};
  std::string group_id;
};

enum class VolatileClaimStatus {
  claimed,
  completed,
  busy,
  expired,
  unavailable,
};

struct VolatileActionClaim {
  VolatileClaimStatus status{VolatileClaimStatus::unavailable};
  std::optional<VolatileAction> action{};
  std::optional<ChronicleMutationResult> result{};
};

class VolatileChronicleActions {
public:
  using NowProvider = std::function<std::int64_t()>;

  explicit VolatileChronicleActions(
      std::size_t capacity = 64, NowProvider now_provider = {},
      std::chrono::milliseconds cleanup_interval = std::chrono::seconds{1});
  ~VolatileChronicleActions();

  void put(std::string token_id, VolatileAction action);
  void put_group(std::vector<std::pair<std::string, VolatileAction>> actions);
  [[nodiscard]] VolatileActionClaim
  claim(std::string_view token_id, const IncomingInteraction &interaction,
        std::int64_t now_ms);
  void finish(std::string_view token_id, ChronicleMutationResult result);
  void release(std::string_view token_id);
  void cancel_group(std::string_view group_id);
  void purge_expired(std::int64_t now_ms);
  [[nodiscard]] std::size_t size() const;

private:
  enum class RecordState {
    active,
    in_progress,
    completed,
  };

  struct Record {
    VolatileAction action;
    RecordState state{RecordState::active};
    std::optional<ChronicleMutationResult> result{};
  };

  void purge_expired_locked(std::int64_t now_ms);
  void cleanup_loop(std::stop_token stop_token) noexcept;

  std::size_t capacity_;
  NowProvider now_provider_;
  std::chrono::milliseconds cleanup_interval_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Record> actions_;
  std::mutex cleanup_mutex_;
  std::condition_variable cleanup_condition_;
  std::jthread cleanup_thread_;
};

class ChronicleService {
public:
  ChronicleService(ChronicleRepository &repository, const Clock &clock,
                   PersistentIdGenerator &ids, ServerScopeConfiguration scope,
                   ControlConfiguration controls,
                   std::function<void()> outbox_wakeup,
                   std::function<void()> scheduler_wakeup,
                   std::size_t draft_capacity = 64,
                   std::function<void()> canon_observer = {});

  [[nodiscard]] ProposalResult
  canonize_message(const IncomingInteraction &interaction);
  [[nodiscard]] ChronicleMutationResult
  edit_proposal(const IncomingInteraction &interaction);
  [[nodiscard]] ChronicleMutationResult
  submit_proposal(const IncomingInteraction &interaction);
  [[nodiscard]] ChronicleMutationResult
  apply_component(const IncomingInteraction &interaction);
  [[nodiscard]] ChronicleMutationResult
  complete_expiry(const ClaimedScheduledJob &job);
  [[nodiscard]] InteractionMessage
  begin_memory_preview(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  recall(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  timeline(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  forget(const IncomingInteraction &interaction);

  [[nodiscard]] static ModalPayload remember_modal();
  [[nodiscard]] static ModalPayload edit_entry_modal(std::string token_id);

private:
  ChronicleRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  ServerScopeConfiguration scope_;
  ControlConfiguration controls_;
  std::function<void()> outbox_wakeup_;
  std::function<void()> scheduler_wakeup_;
  std::function<void()> canon_observer_;
  VolatileChronicleActions volatile_actions_;
};

[[nodiscard]] const char *
chronicle_entry_type_name(ChronicleEntryType type) noexcept;
[[nodiscard]] const char *
chronicle_visibility_name(ChronicleVisibility value) noexcept;
[[nodiscard]] const char *
chronicle_entry_status_name(ChronicleEntryStatus value) noexcept;
[[nodiscard]] const char *
memory_visibility_name(MemoryVisibility value) noexcept;
[[nodiscard]] const char *
memory_sensitivity_name(MemorySensitivity value) noexcept;
[[nodiscard]] const char *memory_status_name(MemoryStatus value) noexcept;
[[nodiscard]] std::optional<ChronicleEntryType>
parse_chronicle_entry_type(std::string_view value) noexcept;
[[nodiscard]] std::optional<ChronicleVisibility>
parse_chronicle_visibility(std::string_view value) noexcept;
[[nodiscard]] std::optional<MemoryVisibility>
parse_memory_visibility(std::string_view value) noexcept;
[[nodiscard]] std::optional<MemorySensitivity>
parse_memory_sensitivity(std::string_view value) noexcept;
[[nodiscard]] bool valid_chronicle_text(std::string_view value,
                                        std::size_t maximum) noexcept;
[[nodiscard]] bool valid_chronicle_snapshot_text(std::string_view value,
                                                 std::size_t maximum) noexcept;
[[nodiscard]] std::vector<std::string>
parse_chronicle_tags(std::string_view value);
[[nodiscard]] std::optional<std::string>
parse_chronicle_component(std::string_view custom_id, std::string_view prefix);
[[nodiscard]] std::string make_chronicle_component(std::string_view prefix,
                                                   std::string_view token_id);
[[nodiscard]] InteractionMessage
render_chronicle_proposal(const ProposalResult &result);
[[nodiscard]] InteractionMessage
render_chronicle_mutation(const ChronicleMutationResult &result);
[[nodiscard]] std::string
render_chronicle_provenance(const ChronicleEntry &entry,
                            std::size_t maximum_size);
[[nodiscard]] bool is_owner_test_entry(const ChronicleEntry &entry) noexcept;

} // namespace sanguinius
