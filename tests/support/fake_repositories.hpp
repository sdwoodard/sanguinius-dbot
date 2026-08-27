#pragma once

#include "sanguinius/repositories.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace sanguinius::test {

struct RecordedStop {
  std::string instance_id;
  std::int64_t stopped_at_ms{};
  ApplicationStopReason reason{ApplicationStopReason::clean_shutdown};
};

struct RecordedHeartbeat {
  std::string instance_id;
  std::int64_t heartbeat_at_ms{};
};

class FakeApplicationInstanceRepository final
    : public ApplicationInstanceRepository {
public:
  void record_start(const ApplicationInstanceRecord &record) override {
    std::scoped_lock lock{mutex_};
    if (fail_start_) {
      throw std::runtime_error{"scripted instance start failure"};
    }
    starts_.push_back(record);
  }

  void record_stop(const std::string &instance_id,
                   const std::int64_t stopped_at_ms,
                   const ApplicationStopReason reason) override {
    std::scoped_lock lock{mutex_};
    if (fail_stop_) {
      throw std::runtime_error{"scripted instance stop failure"};
    }
    stops_.push_back({instance_id, stopped_at_ms, reason});
  }

  void record_heartbeat(const std::string &instance_id,
                        const std::int64_t heartbeat_at_ms) override {
    std::scoped_lock lock{mutex_};
    heartbeats_.push_back({instance_id, heartbeat_at_ms});
  }

  [[nodiscard]] std::vector<ApplicationInstanceRecord> starts() const {
    std::scoped_lock lock{mutex_};
    return starts_;
  }

  [[nodiscard]] std::vector<RecordedStop> stops() const {
    std::scoped_lock lock{mutex_};
    return stops_;
  }

  [[nodiscard]] std::vector<RecordedHeartbeat> heartbeats() const {
    std::scoped_lock lock{mutex_};
    return heartbeats_;
  }

  void fail_start(bool value = true) { fail_start_ = value; }
  void fail_stop(bool value = true) { fail_stop_ = value; }

private:
  mutable std::mutex mutex_;
  std::vector<ApplicationInstanceRecord> starts_;
  std::vector<RecordedHeartbeat> heartbeats_;
  std::vector<RecordedStop> stops_;
  bool fail_start_{false};
  bool fail_stop_{false};
};

class FakeCoreIdentityRepository final : public CoreIdentityRepository {
public:
  void initialize_or_validate_scope(const ServerScopeConfiguration &scope,
                                    const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    scope_ = scope;
  }

  void ensure_user(const DiscordUserRecord &user) override {
    std::unique_lock lock{mutex_};
    entered_ = true;
    ++entered_count_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !blocked_; });
    users_.insert_or_assign(user.user_id, user);
    UserPreferences preferences;
    preferences.updated_at_ms = user.observed_at_ms;
    preferences_.try_emplace(user.user_id, preferences);
  }

  [[nodiscard]] std::optional<UserPreferences>
  load_preferences(const DiscordSnowflake &user_id) override {
    const std::scoped_lock lock{mutex_};
    const auto found = preferences_.find(user_id);
    if (found == preferences_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void set_tarot_standings_visibility(const DiscordSnowflake &user_id,
                                      const bool is_public) {
    const std::scoped_lock lock{mutex_};
    preferences_[user_id].public_tarot_results_opt_in = is_public;
  }

  [[nodiscard]] std::size_t user_count() const {
    const std::scoped_lock lock{mutex_};
    return users_.size();
  }

  void block() {
    const std::scoped_lock lock{mutex_};
    blocked_ = true;
    entered_ = false;
    entered_count_ = 0;
  }

  void release() {
    const std::scoped_lock lock{mutex_};
    blocked_ = false;
    changed_.notify_all();
  }

  [[nodiscard]] bool
  wait_until_entered(const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  [[nodiscard]] bool
  wait_until_entered_count(const std::size_t count,
                           const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return entered_count_ >= count; });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  ServerScopeConfiguration scope_;
  std::unordered_map<DiscordSnowflake, DiscordUserRecord> users_;
  std::unordered_map<DiscordSnowflake, UserPreferences> preferences_;
  bool blocked_{};
  bool entered_{};
  std::size_t entered_count_{};
};

class FakePendingNoticeRepository final : public PendingNoticeRepository {
public:
  [[nodiscard]] CreatePendingNoticeResult
  create_with_token(const CreatePendingNoticeRequest &request) override {
    const std::scoped_lock lock{mutex_};
    const auto existing = creation_keys_.find(request.notice_idempotency_key);
    if (existing != creation_keys_.end()) {
      const auto &notice = notices_.at(existing->second);
      return {notice, notice_tokens_.at(notice.notice_id), false};
    }
    PendingNoticeRecord notice{
        .notice_id = request.notice_id,
        .target_user_id = request.target_user_id,
        .notice_type = request.notice_type,
        .content = request.content,
        .state = PendingNoticeState::pending,
        .expires_at_ms = request.expires_at_ms,
        .opened_at_ms = std::nullopt,
        .consumed_at_ms = std::nullopt,
        .created_at_ms = request.created_at_ms,
    };
    notices_.emplace(notice.notice_id, notice);
    creation_keys_.emplace(request.notice_idempotency_key, notice.notice_id);
    notice_tokens_.emplace(notice.notice_id, request.token_id);
    tokens_.emplace(request.token_id,
                    Token{notice.notice_id, request.target_user_id,
                          request.guild_id, request.channel_id,
                          request.expires_at_ms, InteractionTokenKind::button});
    changed_.notify_all();
    return {notice, request.token_id, true};
  }

  [[nodiscard]] OpenPendingNoticeResult
  open_by_token(const OpenNoticeByTokenRequest &request) override {
    const std::scoped_lock lock{mutex_};
    const auto token = tokens_.find(request.token_id);
    if (token == tokens_.end()) {
      return {.status = OpenPendingNoticeStatus::invalid_token,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    if (token->second.kind != request.interaction_kind) {
      return {.status = OpenPendingNoticeStatus::wrong_kind,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    if (token->second.guild_id != request.guild_id ||
        token->second.channel_id != request.channel_id) {
      return {.status = OpenPendingNoticeStatus::wrong_scope,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    if (token->second.user_id != request.user_id) {
      return {.status = OpenPendingNoticeStatus::wrong_user,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    auto &notice = notices_.at(token->second.notice_id);
    expire(notice, request.now_ms);
    if (notice.state == PendingNoticeState::expired ||
        request.now_ms >= token->second.expires_at_ms) {
      return {.status = OpenPendingNoticeStatus::expired,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    if (notice.state == PendingNoticeState::consumed ||
        notice.state == PendingNoticeState::cancelled) {
      return {.status = OpenPendingNoticeStatus::unavailable,
              .notice = std::nullopt,
              .delivery_idempotency_key = std::nullopt};
    }
    const auto existing = attempts_.find(request.interaction_idempotency_key);
    if (existing != attempts_.end()) {
      if (existing->second.kind != "button" ||
          existing->second.user_id != request.user_id ||
          existing->second.notice_id !=
              std::optional<std::string>{notice.notice_id} ||
          existing->second.token_id !=
              std::optional<std::string>{request.token_id}) {
        throw std::runtime_error{"conflicting reveal attempt"};
      }
      if (existing->second.state == AttemptState::failed) {
        existing->second.state = AttemptState::prepared;
      }
    } else {
      attempts_.emplace(request.interaction_idempotency_key,
                        Attempt{"button", request.user_id, notice.notice_id,
                                request.token_id, AttemptState::prepared});
    }
    return {.status = OpenPendingNoticeStatus::opened,
            .notice = notice,
            .delivery_idempotency_key = request.interaction_idempotency_key};
  }

  [[nodiscard]] OpenPendingNoticeResult
  open_next(const OpenNextNoticeRequest &request) override {
    const std::scoped_lock lock{mutex_};
    const auto previous = attempts_.find(request.interaction_idempotency_key);
    if (previous != attempts_.end()) {
      if (previous->second.kind != "inbox" ||
          previous->second.user_id != request.user_id) {
        throw std::runtime_error{"conflicting reveal attempt"};
      }
      if (previous->second.state == AttemptState::failed) {
        previous->second.state = AttemptState::prepared;
      }
      if (!previous->second.notice_id.has_value()) {
        return {.status = OpenPendingNoticeStatus::no_pending_notice,
                .notice = std::nullopt,
                .delivery_idempotency_key =
                    request.interaction_idempotency_key};
      }
      auto &notice = notices_.at(*previous->second.notice_id);
      expire(notice, request.now_ms);
      if (notice.state == PendingNoticeState::expired) {
        return {.status = OpenPendingNoticeStatus::expired,
                .notice = std::nullopt,
                .delivery_idempotency_key = std::nullopt};
      }
      if (notice.state == PendingNoticeState::consumed ||
          notice.state == PendingNoticeState::cancelled) {
        return {.status = OpenPendingNoticeStatus::unavailable,
                .notice = std::nullopt,
                .delivery_idempotency_key = std::nullopt};
      }
      return {.status = OpenPendingNoticeStatus::opened,
              .notice = notice,
              .delivery_idempotency_key = request.interaction_idempotency_key};
    }
    PendingNoticeRecord *oldest{};
    for (auto &[id, notice] : notices_) {
      static_cast<void>(id);
      expire(notice, request.now_ms);
      if (notice.target_user_id == request.user_id &&
          notice.state == PendingNoticeState::pending &&
          !reserved(notice.notice_id) &&
          (oldest == nullptr || notice.created_at_ms < oldest->created_at_ms)) {
        oldest = &notice;
      }
    }
    if (oldest == nullptr) {
      attempts_.emplace(request.interaction_idempotency_key,
                        Attempt{"inbox", request.user_id, std::nullopt,
                                std::nullopt, AttemptState::prepared});
      return {.status = OpenPendingNoticeStatus::no_pending_notice,
              .notice = std::nullopt,
              .delivery_idempotency_key = request.interaction_idempotency_key};
    }
    attempts_.emplace(request.interaction_idempotency_key,
                      Attempt{"inbox", request.user_id, oldest->notice_id,
                              std::nullopt, AttemptState::prepared});
    return {.status = OpenPendingNoticeStatus::opened,
            .notice = *oldest,
            .delivery_idempotency_key = request.interaction_idempotency_key};
  }

  [[nodiscard]] PendingNoticeMutationStatus
  confirm_open_delivery(const std::string &interaction_idempotency_key,
                        const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    const auto found = attempts_.find(interaction_idempotency_key);
    if (found == attempts_.end()) {
      return PendingNoticeMutationStatus::not_found;
    }
    auto &attempt = found->second;
    if (attempt.state == AttemptState::delivered) {
      return PendingNoticeMutationStatus::unchanged;
    }
    if (attempt.state != AttemptState::prepared) {
      return PendingNoticeMutationStatus::invalid_state;
    }
    if (attempt.notice_id.has_value()) {
      auto &notice = notices_.at(*attempt.notice_id);
      expire(notice, now_ms);
      if (notice.state == PendingNoticeState::pending) {
        notice.state = PendingNoticeState::opened;
        notice.opened_at_ms = now_ms;
      }
    }
    attempt.state = AttemptState::delivered;
    return PendingNoticeMutationStatus::applied;
  }

  [[nodiscard]] PendingNoticeMutationStatus
  release_open_delivery(const std::string &interaction_idempotency_key,
                        const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    const auto found = attempts_.find(interaction_idempotency_key);
    if (found == attempts_.end()) {
      return PendingNoticeMutationStatus::not_found;
    }
    auto &attempt = found->second;
    if (attempt.state == AttemptState::failed) {
      return PendingNoticeMutationStatus::unchanged;
    }
    if (attempt.state != AttemptState::prepared) {
      return PendingNoticeMutationStatus::invalid_state;
    }
    attempt.state = AttemptState::failed;
    return PendingNoticeMutationStatus::applied;
  }

  [[nodiscard]] std::size_t
  recover_incomplete_open_deliveries(const std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    std::size_t changed{};
    for (auto &[key, attempt] : attempts_) {
      static_cast<void>(key);
      if (attempt.state == AttemptState::prepared) {
        attempt.state = AttemptState::failed;
        ++changed;
      }
    }
    return changed;
  }

  [[nodiscard]] PendingNoticeMutationStatus
  consume(const std::string &notice_id, const DiscordSnowflake &user_id,
          const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    const auto found = notices_.find(notice_id);
    if (found == notices_.end()) {
      return PendingNoticeMutationStatus::not_found;
    }
    auto &notice = found->second;
    if (notice.target_user_id != user_id) {
      return PendingNoticeMutationStatus::wrong_user;
    }
    expire(notice, now_ms);
    if (notice.state == PendingNoticeState::expired) {
      return PendingNoticeMutationStatus::expired;
    }
    if (notice.state == PendingNoticeState::consumed) {
      return PendingNoticeMutationStatus::unchanged;
    }
    if (notice.state != PendingNoticeState::opened) {
      return PendingNoticeMutationStatus::invalid_state;
    }
    notice.state = PendingNoticeState::consumed;
    notice.consumed_at_ms = now_ms;
    return PendingNoticeMutationStatus::applied;
  }

  [[nodiscard]] PendingNoticeMutationStatus
  cancel(const std::string &notice_id, const DiscordSnowflake &user_id,
         const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    const auto found = notices_.find(notice_id);
    if (found == notices_.end()) {
      return PendingNoticeMutationStatus::not_found;
    }
    auto &notice = found->second;
    if (notice.target_user_id != user_id) {
      return PendingNoticeMutationStatus::wrong_user;
    }
    expire(notice, now_ms);
    if (notice.state == PendingNoticeState::expired) {
      return PendingNoticeMutationStatus::expired;
    }
    if (notice.state == PendingNoticeState::cancelled) {
      return PendingNoticeMutationStatus::unchanged;
    }
    if (notice.state == PendingNoticeState::consumed) {
      return PendingNoticeMutationStatus::invalid_state;
    }
    notice.state = PendingNoticeState::cancelled;
    return PendingNoticeMutationStatus::applied;
  }

  [[nodiscard]] std::size_t expire_due(const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    std::size_t count{};
    for (auto &[id, notice] : notices_) {
      static_cast<void>(id);
      const auto prior = notice.state;
      expire(notice, now_ms);
      count += prior != notice.state ? 1U : 0U;
    }
    return count;
  }

  [[nodiscard]] std::size_t pending_count(const DiscordSnowflake &user_id,
                                          const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    std::size_t count{};
    for (auto &[id, notice] : notices_) {
      static_cast<void>(id);
      expire(notice, now_ms);
      if (notice.target_user_id == user_id &&
          notice.state == PendingNoticeState::pending) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t
  pending_count_all(const std::int64_t now_ms) override {
    const std::scoped_lock lock{mutex_};
    std::size_t count{};
    for (auto &[id, notice] : notices_) {
      static_cast<void>(id);
      expire(notice, now_ms);
      if (notice.state == PendingNoticeState::pending) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::size_t notice_count() const {
    const std::scoped_lock lock{mutex_};
    return notices_.size();
  }

  [[nodiscard]] bool
  wait_for_notice_count(const std::size_t count,
                        const std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, timeout, [this, count] { return notices_.size() >= count; });
  }

private:
  enum class AttemptState {
    prepared,
    delivered,
    failed,
  };

  struct Attempt {
    std::string kind;
    DiscordSnowflake user_id;
    std::optional<std::string> notice_id;
    std::optional<std::string> token_id;
    AttemptState state{AttemptState::prepared};
  };

  struct Token {
    std::string notice_id;
    DiscordSnowflake user_id;
    DiscordSnowflake guild_id;
    DiscordSnowflake channel_id;
    std::int64_t expires_at_ms{};
    InteractionTokenKind kind{InteractionTokenKind::button};
  };

  static void expire(PendingNoticeRecord &notice, const std::int64_t now_ms) {
    if (notice.expires_at_ms.has_value() && now_ms >= *notice.expires_at_ms &&
        (notice.state == PendingNoticeState::pending ||
         notice.state == PendingNoticeState::opened)) {
      notice.state = PendingNoticeState::expired;
    }
  }

  [[nodiscard]] bool reserved(const std::string &notice_id) const {
    return std::any_of(attempts_.begin(), attempts_.end(),
                       [&notice_id](const auto &entry) {
                         return entry.second.notice_id ==
                                    std::optional<std::string>{notice_id} &&
                                entry.second.state == AttemptState::prepared;
                       });
  }

  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::unordered_map<std::string, PendingNoticeRecord> notices_;
  std::unordered_map<std::string, Token> tokens_;
  std::unordered_map<std::string, std::string> creation_keys_;
  std::unordered_map<std::string, std::string> notice_tokens_;
  std::unordered_map<std::string, Attempt> attempts_;
};

} // namespace sanguinius::test
