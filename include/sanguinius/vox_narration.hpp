#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/callback_fence.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_interfaces.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/work_queue.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace sanguinius {

class AiWorkService;

enum class VoxNarrationFeature { chronicle, tarot, appearance, session };
enum class VoxNarrationModelStatus {
  not_requested,
  generated,
  fallback,
  refused,
  failed,
  saturated,
  duplicate,
};

struct VoxNarrationPolicy {
  VoxNarrationFeature feature{VoxNarrationFeature::chronicle};
  std::uint8_t rank{};
  std::int64_t ttl_ms{};
  bool counterpart_required{true};
  bool deterministic_only{};
  std::optional<std::string> fallback_line;
};

struct VoxNarrationCandidate {
  std::string intent_id;
  std::size_t revision{1};
  std::string source_event_id;
  std::string event_type;
  VoxNarrationFeature feature{VoxNarrationFeature::chronicle};
  std::string guild_id;
  std::string channel_id;
  std::string safe_input;
  std::optional<std::string> fallback_line;
  std::uint8_t rank{};
  std::int64_t created_at_ms{};
  std::int64_t expires_at_ms{};
  std::string session_id;
  std::size_t mute_epoch{};
  std::optional<std::string> counterpart_outbox_id;
  bool counterpart_required{true};
  bool is_test{};
};

struct VoxNarrationObserveRequest {
  std::int64_t now_ms{};
  bool enabled{};
  bool test_mode{};
  std::size_t limit{32};
  std::function<std::string()> next_id;
};

enum class VoxNarrationEnqueueStatus { accepted, replay, rejected };

struct VoxNarrationEnqueueRequest {
  std::string source_event_id;
  std::int64_t now_ms{};
  bool enabled{};
  bool test_mode{};
  std::function<std::string()> next_id;
};

struct VoxNarrationEnqueueResult {
  VoxNarrationEnqueueStatus status{VoxNarrationEnqueueStatus::rejected};
  std::string reason;
};

struct VoxNarrationClaimRequest {
  std::int64_t now_ms{};
  std::string instance_id;
  std::string lease_token;
  std::string transition_id;
  std::int64_t lease_until_ms{};
  bool test_mode{};
};

struct VoxNarrationGenerationStartRequest {
  std::string intent_id;
  std::size_t expected_revision{};
  std::size_t expected_mute_epoch{};
  std::string instance_id;
  std::string expected_lease_token;
  std::string lease_token;
  std::string transition_id;
  std::int64_t now_ms{};
  std::int64_t lease_until_ms{};
  bool test_mode{};
};

struct VoxNarrationCompletion {
  std::string intent_id;
  std::size_t expected_revision{};
  std::size_t expected_mute_epoch{};
  std::optional<std::string> line;
  VoxNarrationModelStatus model_status{VoxNarrationModelStatus::failed};
  std::string content_hash;
  std::string speech_id;
  std::string transition_id;
  std::int64_t now_ms{};
};

struct VoxNarrationRecent {
  std::string intent_id;
  std::string event_type;
  std::string feature;
  std::string state;
  std::optional<std::string> reason;
  std::int64_t created_at_ms{};
};

struct VoxNarrationControlContext {
  std::string idempotency_key;
  std::string operation;
  std::string actor_user_id;
  std::string guild_id;
  std::string channel_id;
  std::string request_fingerprint;
  std::int64_t now_ms{};
};

struct VoxNarrationHealth {
  std::size_t pending{};
  std::size_t generating{};
  std::size_t queued{};
  std::size_t session_feature_count{};
  std::int64_t cursor_rowid{};
  std::int64_t journal_head_rowid{};
};

[[nodiscard]] std::string
vox_narration_enqueue_response(const VoxNarrationEnqueueResult &result);

class VoxNarrationRepository {
public:
  virtual ~VoxNarrationRepository() = default;
  virtual std::size_t observe_batch(const VoxNarrationObserveRequest &) = 0;
  [[nodiscard]] virtual VoxNarrationEnqueueResult
  enqueue_reference(const VoxNarrationEnqueueRequest &) {
    return {.status = VoxNarrationEnqueueStatus::rejected,
            .reason = "unsupported"};
  }
  [[nodiscard]] virtual std::string
  enqueue_reference_with_receipt(const VoxNarrationEnqueueRequest &request,
                                 const VoxNarrationControlContext &context) {
    if (const auto receipt = control_receipt(context))
      return *receipt;
    return record_control_receipt(
        context, vox_narration_enqueue_response(enqueue_reference(request)));
  }
  [[nodiscard]] virtual std::optional<VoxNarrationCandidate>
  claim_next(const VoxNarrationClaimRequest &) = 0;
  [[nodiscard]] virtual std::optional<VoxNarrationCandidate>
  begin_generation(const VoxNarrationGenerationStartRequest &) {
    return std::nullopt;
  }
  virtual void complete_generation(const VoxNarrationCompletion &) = 0;
  virtual std::size_t reconcile(
      std::int64_t now_ms, const std::function<std::string()> &next_id,
      const std::function<bool(std::string_view)> &generation_is_live = {}) = 0;
  [[nodiscard]] virtual std::optional<VoxNarrationCandidate>
  preview(std::string_view source_event_id, std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::vector<VoxNarrationRecent>
  recent(std::size_t limit) = 0;
  [[nodiscard]] virtual VoxNarrationHealth health() = 0;
  [[nodiscard]] virtual std::optional<std::string>
  control_receipt(const VoxNarrationControlContext &) {
    return std::nullopt;
  }
  [[nodiscard]] virtual std::string
  record_control_receipt(const VoxNarrationControlContext &,
                         std::string message) {
    return message;
  }
  [[nodiscard]] virtual bool automatic_speech_suppressed(std::int64_t now_ms) {
    static_cast<void>(now_ms);
    return false;
  }
  [[nodiscard]] virtual bool
  automatic_speech_admission_suppressed(std::int64_t now_ms) {
    return automatic_speech_suppressed(now_ms);
  }
  [[nodiscard]] virtual std::optional<std::string>
  session_flavor_context(std::string_view session_id, std::string_view guild_id,
                         std::string_view summoner_user_id) {
    static_cast<void>(session_id);
    static_cast<void>(guild_id);
    static_cast<void>(summoner_user_id);
    return std::nullopt;
  }
};

[[nodiscard]] std::optional<VoxNarrationPolicy>
vox_narration_policy(std::string_view event_type);
[[nodiscard]] std::string_view
vox_narration_feature_name(VoxNarrationFeature feature) noexcept;
[[nodiscard]] std::string_view
vox_narration_model_status_name(VoxNarrationModelStatus status) noexcept;
[[nodiscard]] AiRequest
vox_narration_request(const VoxNarrationCandidate &candidate);
[[nodiscard]] std::optional<std::string>
parse_vox_narration_line(std::string_view response,
                         const VoxNarrationCandidate &candidate);
[[nodiscard]] bool
appearance_narration_too_similar(std::string_view public_text,
                                 std::string_view narration);

class VoxNarrationService {
public:
  using SessionFlavorReady =
      std::function<void(std::string, std::string, std::string, std::string)>;

  VoxNarrationService(VoxNarrationRepository &repository, const Clock &clock,
                      PersistentIdGenerator &ids, Diagnostics &diagnostics,
                      const AiClient &ai, AiWorkService &ai_work,
                      std::string instance_id, bool enabled, bool test_mode,
                      std::function<void()> speech_wakeup = {},
                      SessionFlavorReady session_flavor_ready = {});
  ~VoxNarrationService();

  VoxNarrationService(const VoxNarrationService &) = delete;
  VoxNarrationService &operator=(const VoxNarrationService &) = delete;

  void start();
  void stop() noexcept;
  void wake() noexcept;
  void run_one_cycle();
  [[nodiscard]] std::optional<VoxNarrationCandidate>
  preview(std::string_view source_event_id);
  [[nodiscard]] VoxNarrationEnqueueResult
  enqueue(std::string_view source_event_id);
  [[nodiscard]] std::string
  enqueue_with_receipt(std::string_view source_event_id,
                       const VoxNarrationControlContext &context);
  [[nodiscard]] std::vector<VoxNarrationRecent> recent(std::size_t limit = 10);
  [[nodiscard]] VoxNarrationHealth health();
  [[nodiscard]] std::optional<std::string>
  control_receipt(const VoxNarrationControlContext &context);
  [[nodiscard]] std::string
  record_control_receipt(const VoxNarrationControlContext &context,
                         std::string message);
  void prepare_session_flavor(std::string session_id, std::string guild_id,
                              std::string summoner_user_id);

private:
  void poll(std::stop_token stop_token) noexcept;
  void generate_dispatched(VoxNarrationCandidate candidate,
                           std::string claim_lease_token,
                           std::stop_token stop_token) noexcept;
  void generate(VoxNarrationCandidate candidate,
                std::stop_token stop_token) noexcept;
  void complete_fallback(VoxNarrationCandidate candidate,
                         VoxNarrationModelStatus status) noexcept;
  [[nodiscard]] bool generation_is_live(std::string_view intent_id) const;
  void release_generation(std::string_view intent_id) noexcept;

  VoxNarrationRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  const AiClient &ai_;
  AiWorkService &ai_work_;
  std::string instance_id_;
  bool enabled_{};
  bool test_mode_{};
  std::function<void()> speech_wakeup_;
  SessionFlavorReady session_flavor_ready_;
  std::shared_ptr<CallbackFence> callbacks_{std::make_shared<CallbackFence>()};
  mutable std::mutex generation_mutex_;
  std::unordered_set<std::string> live_generations_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  std::stop_source work_stop_;
  std::jthread poller_;
  bool started_{};
  bool callbacks_closed_{};
};

} // namespace sanguinius
