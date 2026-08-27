#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::int64_t relationship_direct_cooldown_ms =
    24LL * 60 * 60 * 1'000;
inline constexpr std::int64_t memory_callback_cooldown_ms =
    7LL * 24 * 60 * 60 * 1'000;
inline constexpr std::size_t maximum_prompt_memory_candidates = 50;
inline constexpr std::size_t maximum_prompt_memories = 3;
inline constexpr std::size_t maximum_prompt_memory_bytes = 1'500;

struct RelationshipDimensions {
  int familiarity{};
  int esteem{};
  int mirth{};
  int reliability{};
  int wariness{};

  [[nodiscard]] bool operator==(const RelationshipDimensions &) const = default;
};

struct RelationshipDelta {
  int familiarity{};
  int esteem{};
  int mirth{};
  int reliability{};
  int wariness{};

  [[nodiscard]] bool operator==(const RelationshipDelta &) const = default;
};

enum class RelationshipSourceKind {
  chronicle_canon,
  direct_ai,
  tarot_resolved,
  tarot_honored,
  appearance_positive_feedback,
  session_completed,
  title_awarded,
};

enum class QualitativeBand {
  dormant,
  emerging,
  established,
  strong,
  storied,
  legendary,
};

[[nodiscard]] RelationshipDelta
relationship_policy(RelationshipSourceKind kind,
                    bool direct_cooldown_active = false) noexcept;
[[nodiscard]] RelationshipDimensions
apply_relationship_delta(RelationshipDimensions current,
                         RelationshipDelta requested) noexcept;
[[nodiscard]] RelationshipDelta
applied_relationship_delta(RelationshipDimensions old_values,
                           RelationshipDimensions new_values) noexcept;
[[nodiscard]] QualitativeBand qualitative_band(int value) noexcept;
[[nodiscard]] std::string_view
qualitative_band_name(QualitativeBand band) noexcept;
[[nodiscard]] std::string
relationship_style_hint(const RelationshipDimensions &dimensions);

struct MemoryCandidate {
  std::string memory_id;
  std::string text;
  std::vector<std::string> tags;
  std::int64_t created_at_ms{};
  std::size_t revision{};
};

struct RankedMemory {
  MemoryCandidate memory;
  int score{};
  std::size_t tag_matches{};
};

[[nodiscard]] std::vector<std::string>
normalized_relevance_tokens(std::string_view text, std::size_t limit = 32);
[[nodiscard]] std::vector<RankedMemory>
rank_prompt_memories(std::vector<MemoryCandidate> candidates,
                     std::string_view current_request,
                     std::string_view replied_text, std::int64_t now_ms);

enum class PromptPreparationStatus {
  generic,
  prepared,
  duplicate,
};

struct PreparedPromptContext {
  PromptPreparationStatus status{PromptPreparationStatus::generic};
  std::optional<std::string> attempt_id;
  std::string relationship_style;
  std::vector<RankedMemory> memories;
  std::optional<std::string> featured_title;
  std::optional<std::string> latest_session_summary;
  bool session_open{};
};

struct PreparePromptContextRequest {
  std::string attempt_id;
  std::string application_instance_id;
  DiscordSnowflake requester_user_id;
  std::string requester_username;
  std::string requester_display_name;
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake source_message_id;
  std::string current_request;
  std::string replied_text;
  std::string correlation_id;
  std::int64_t now_ms{};
};

enum class PromptFinalizationStatus {
  applied,
  unchanged,
  invalidated,
  not_found,
};

struct CompletePromptAttemptRequest {
  std::string attempt_id;
  std::string source_event_id;
  std::string relationship_event_id;
  std::int64_t now_ms{};
};

struct FailPromptAttemptRequest {
  std::string attempt_id;
  std::string outcome;
  std::string error_code;
  std::int64_t now_ms{};
};

struct RelationshipProfile {
  bool found{};
  bool is_bot{};
  bool chronicle_opt_in{};
  bool memory_callbacks{};
  DiscordSnowflake user_id;
  std::string display_name;
  RelationshipDimensions dimensions;
  std::vector<std::string> recent_reasons;
  std::size_t shared_canon_count{};
  std::vector<std::string> visible_canon_titles;
  std::optional<std::string> featured_title;
  std::optional<std::string> latest_session_summary;
  bool session_open{};
};

struct SetMemoryCallbacksRequest {
  DiscordSnowflake guild_id;
  DiscordSnowflake channel_id;
  DiscordSnowflake user_id;
  bool enabled{};
  std::string event_id;
  std::string correlation_id;
  std::string idempotency_key;
  std::int64_t now_ms{};
};

enum class PreferenceChangeStatus {
  updated,
  unchanged,
  chronicle_opted_out,
};

struct ProjectionCheckResult {
  bool valid{};
  std::size_t event_count{};
  std::size_t projection_count{};
  std::size_t mismatch_count{};
};

class RelationshipRepository {
public:
  virtual ~RelationshipRepository() = default;

  [[nodiscard]] virtual PreparedPromptContext
  prepare_prompt_context(const PreparePromptContextRequest &request) = 0;
  [[nodiscard]] virtual PromptFinalizationStatus
  complete_prompt_attempt(const CompletePromptAttemptRequest &request) = 0;
  [[nodiscard]] virtual PromptFinalizationStatus
  fail_prompt_attempt(const FailPromptAttemptRequest &request) = 0;
  virtual std::size_t recover_prompt_attempts(std::string_view instance_id,
                                              std::int64_t now_ms,
                                              std::size_t limit = 50) = 0;
  virtual std::size_t synchronize_chronicle_sources(PersistentIdGenerator &ids,
                                                    std::int64_t now_ms,
                                                    std::size_t limit = 50) = 0;
  [[nodiscard]] virtual RelationshipProfile
  profile(const DiscordSnowflake &viewer, const DiscordSnowflake &target,
          bool public_view, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual PreferenceChangeStatus
  set_memory_callbacks(const SetMemoryCallbacksRequest &request) = 0;
  [[nodiscard]] virtual ProjectionCheckResult check_projection() = 0;
  [[nodiscard]] virtual ProjectionCheckResult rebuild_projection() = 0;
};

class RelationshipService {
public:
  RelationshipService(RelationshipRepository &repository, const Clock &clock,
                      PersistentIdGenerator &ids,
                      ServerScopeConfiguration scope,
                      std::string application_instance_id);

  [[nodiscard]] PreparedPromptContext
  prepare_prompt(const IncomingMessage &message, std::string current_request,
                 std::string replied_text);
  [[nodiscard]] PromptFinalizationStatus
  complete_prompt(std::string_view attempt_id);
  [[nodiscard]] PromptFinalizationStatus
  fail_prompt(std::string_view attempt_id, std::string_view outcome,
              std::string_view error_code);
  [[nodiscard]] InteractionMessage
  profile(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  set_memory_callbacks(const IncomingInteraction &interaction);
  std::size_t recover(std::size_t limit = 50);
  [[nodiscard]] ProjectionCheckResult check_projection();
  [[nodiscard]] bool
  in_feature_scope(const IncomingMessage &message) const noexcept;

private:
  RelationshipRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  ServerScopeConfiguration scope_;
  std::string application_instance_id_;
};

} // namespace sanguinius
