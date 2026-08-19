#pragma once

#include "sanguinius/relationships.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeRelationshipRepository final : public RelationshipRepository {
public:
  PreparedPromptContext
  prepare_prompt_context(const PreparePromptContextRequest &request) override {
    const std::scoped_lock lock{mutex_};
    preparations_.push_back(request);
    auto result = prepared;
    if (result.status == PromptPreparationStatus::prepared &&
        !result.attempt_id) {
      result.attempt_id = request.attempt_id;
    }
    return result;
  }

  PromptFinalizationStatus complete_prompt_attempt(
      const CompletePromptAttemptRequest &request) override {
    const std::scoped_lock lock{mutex_};
    completions_.push_back(request);
    if (throw_on_completion) {
      throw std::runtime_error{"injected prompt completion failure"};
    }
    return completion_status;
  }

  PromptFinalizationStatus
  fail_prompt_attempt(const FailPromptAttemptRequest &request) override {
    const std::scoped_lock lock{mutex_};
    failures_.push_back(request);
    if (throw_on_failure) {
      throw std::runtime_error{"injected prompt failure transition failure"};
    }
    return PromptFinalizationStatus::applied;
  }

  std::size_t recover_prompt_attempts(std::string_view,
                                      std::int64_t) override {
    return recovered_attempts;
  }

  std::size_t synchronize_chronicle_sources(PersistentIdGenerator &,
                                             std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    if (throw_on_synchronization_) {
      throw std::runtime_error{"injected Chronicle relationship sync failure"};
    }
    return synchronized_sources;
  }

  RelationshipProfile profile(const DiscordSnowflake &viewer,
                              const DiscordSnowflake &target, bool,
                              std::int64_t) override {
    auto result = profile_result;
    if (!result.user_id.is_set()) result.user_id = target;
    if (viewer == target && !result.found) {
      result.found = true;
      result.chronicle_opt_in = true;
    }
    return result;
  }

  PreferenceChangeStatus
  set_memory_callbacks(const SetMemoryCallbacksRequest &request) override {
    const std::scoped_lock lock{mutex_};
    preference_changes_.push_back(request);
    return preference_status;
  }

  ProjectionCheckResult check_projection() override { return projection; }
  ProjectionCheckResult rebuild_projection() override { return projection; }

  [[nodiscard]] std::size_t preparation_count() const {
    const std::scoped_lock lock{mutex_};
    return preparations_.size();
  }
  [[nodiscard]] std::size_t completion_count() const {
    const std::scoped_lock lock{mutex_};
    return completions_.size();
  }
  [[nodiscard]] std::size_t failure_count() const {
    const std::scoped_lock lock{mutex_};
    return failures_.size();
  }
  [[nodiscard]] std::size_t preference_change_count() const {
    const std::scoped_lock lock{mutex_};
    return preference_changes_.size();
  }
  void fail_synchronization(const bool fail = true) {
    const std::scoped_lock lock{mutex_};
    throw_on_synchronization_ = fail;
  }

  PreparedPromptContext prepared{};
  PromptFinalizationStatus completion_status{PromptFinalizationStatus::applied};
  RelationshipProfile profile_result{};
  PreferenceChangeStatus preference_status{PreferenceChangeStatus::updated};
  ProjectionCheckResult projection{.valid = true};
  bool throw_on_completion{};
  bool throw_on_failure{};
  std::size_t recovered_attempts{};
  std::size_t synchronized_sources{};

private:
  mutable std::mutex mutex_;
  std::vector<PreparePromptContextRequest> preparations_;
  std::vector<CompletePromptAttemptRequest> completions_;
  std::vector<FailPromptAttemptRequest> failures_;
  std::vector<SetMemoryCallbacksRequest> preference_changes_;
  bool throw_on_synchronization_{};
};

} // namespace sanguinius::test
