#pragma once

#include "sanguinius/vox_narration.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeVoxNarrationRepository final : public VoxNarrationRepository {
public:
  std::size_t observe_batch(const VoxNarrationObserveRequest &) override {
    return 0;
  }

  [[nodiscard]] VoxNarrationEnqueueResult
  enqueue_reference(const VoxNarrationEnqueueRequest &request) override {
    const std::scoped_lock lock{mutex_};
    ++enqueue_count_;
    last_enqueue_reference_ = request.source_event_id;
    return enqueue_result_;
  }

  [[nodiscard]] std::optional<VoxNarrationCandidate>
  claim_next(const VoxNarrationClaimRequest &) override {
    return std::nullopt;
  }

  void complete_generation(const VoxNarrationCompletion &) override {}

  std::size_t reconcile(std::int64_t, const std::function<std::string()> &,
                        const std::function<bool(std::string_view)> & = {},
                        std::size_t = 50) override {
    return 0;
  }

  [[nodiscard]] std::optional<VoxNarrationCandidate>
  preview(const std::string_view source_event_id, std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    if (!candidate_ || candidate_->source_event_id != source_event_id)
      return std::nullopt;
    return candidate_;
  }

  [[nodiscard]] std::vector<VoxNarrationRecent>
  recent(const std::size_t limit) override {
    const std::scoped_lock lock{mutex_};
    auto result = recent_;
    result.resize(std::min(limit, result.size()));
    return result;
  }

  [[nodiscard]] VoxNarrationHealth health() override { return {}; }

  [[nodiscard]] std::optional<std::string>
  control_receipt(const VoxNarrationControlContext &context) override {
    const std::scoped_lock lock{mutex_};
    const auto found = receipts_.find(context.idempotency_key);
    if (found == receipts_.end())
      return std::nullopt;
    validate_replay(found->second.context, context);
    return found->second.message;
  }

  [[nodiscard]] std::string
  record_control_receipt(const VoxNarrationControlContext &context,
                         std::string message) override {
    const std::scoped_lock lock{mutex_};
    const auto [found, inserted] = receipts_.try_emplace(
        context.idempotency_key, Receipt{context, std::move(message)});
    if (!inserted)
      validate_replay(found->second.context, context);
    return found->second.message;
  }

  void set_candidate(VoxNarrationCandidate candidate) {
    const std::scoped_lock lock{mutex_};
    candidate_ = std::move(candidate);
  }

  void set_recent(std::vector<VoxNarrationRecent> recent) {
    const std::scoped_lock lock{mutex_};
    recent_ = std::move(recent);
  }

  void set_enqueue_result(VoxNarrationEnqueueResult result) {
    const std::scoped_lock lock{mutex_};
    enqueue_result_ = std::move(result);
  }

  [[nodiscard]] std::size_t enqueue_count() const {
    const std::scoped_lock lock{mutex_};
    return enqueue_count_;
  }

  [[nodiscard]] std::optional<std::string> last_enqueue_reference() const {
    const std::scoped_lock lock{mutex_};
    return last_enqueue_reference_;
  }

  [[nodiscard]] std::size_t receipt_count() const {
    const std::scoped_lock lock{mutex_};
    return receipts_.size();
  }

private:
  struct Receipt {
    VoxNarrationControlContext context;
    std::string message;
  };

  static void validate_replay(const VoxNarrationControlContext &stored,
                              const VoxNarrationControlContext &request) {
    if (stored.operation != request.operation ||
        stored.actor_user_id != request.actor_user_id ||
        stored.guild_id != request.guild_id ||
        stored.channel_id != request.channel_id ||
        stored.request_fingerprint != request.request_fingerprint)
      throw std::invalid_argument{
          "Narration interaction idempotency key was reused."};
  }

  mutable std::mutex mutex_;
  std::optional<VoxNarrationCandidate> candidate_;
  VoxNarrationEnqueueResult enqueue_result_{
      .status = VoxNarrationEnqueueStatus::accepted, .reason = "eligible"};
  std::optional<std::string> last_enqueue_reference_;
  std::vector<VoxNarrationRecent> recent_;
  std::map<std::string, Receipt> receipts_;
  std::size_t enqueue_count_{};
};

} // namespace sanguinius::test
