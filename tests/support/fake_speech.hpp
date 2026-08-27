#pragma once

#include "sanguinius/speech.hpp"
#include "sanguinius/tts_cache.hpp"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeSpeechRepository final : public SpeechRepository {
public:
  SpeechEnqueueResult enqueue(const SpeechEnqueueRequest &request) override {
    const std::scoped_lock lock{mutex_};
    for (const auto &[id, existing] : items_) {
      static_cast<void>(id);
      if (existing.deduplication_key == request.deduplication_key)
        return {.status = SpeechEnqueueStatus::replay,
                .item = existing,
                .evicted_speech_id = std::nullopt};
    }
    SpeechItem item{
        .speech_id = request.speech_id,
        .voice_session_id = request.voice_session_id,
        .source_event_id = request.source_event_id,
        .source_kind = request.source_kind,
        .text = request.text.text,
        .text_hash = request.text_hash,
        .scalar_count = request.text.scalar_count,
        .provider = request.provider,
        .model = request.model,
        .voice = request.voice,
        .priority = request.priority,
        .narration_rank = request.narration_rank,
        .state = SpeechState::pending,
        .revision = 1,
        .earliest_at_ms = request.earliest_at_ms,
        .expires_at_ms = request.expires_at_ms,
        .interruptible = request.interruptible,
        .deduplication_key = request.deduplication_key,
        .provider_request_id = std::nullopt,
        .cache_key = std::nullopt,
        .cache_checksum = std::nullopt,
        .marker = std::nullopt,
        .duration_ms = std::nullopt,
        .attempt_count = 0,
        .created_at_ms = request.created_at_ms,
        .terminal_at_ms = std::nullopt,
        .last_error_code = std::nullopt,
    };
    items_[item.speech_id] = item;
    return {.status = SpeechEnqueueStatus::accepted,
            .item = item,
            .evicted_speech_id = std::nullopt};
  }

  std::optional<SpeechItem> claim_next(std::string_view voice_session_id,
                                       std::int64_t now_ms, std::string,
                                       std::string) override {
    const std::scoped_lock lock{mutex_};
    SpeechItem *selected{};
    for (auto &[id, item] : items_) {
      static_cast<void>(id);
      if (item.voice_session_id != voice_session_id ||
          item.state != SpeechState::pending || item.earliest_at_ms > now_ms ||
          (item.expires_at_ms && *item.expires_at_ms <= now_ms))
        continue;
      const auto preferred = !selected || item.priority > selected->priority ||
                             (item.priority == selected->priority &&
                              item.narration_rank > selected->narration_rank) ||
                             (item.priority == selected->priority &&
                              item.narration_rank == selected->narration_rank &&
                              item.earliest_at_ms < selected->earliest_at_ms) ||
                             (item.priority == selected->priority &&
                              item.narration_rank == selected->narration_rank &&
                              item.earliest_at_ms == selected->earliest_at_ms &&
                              item.created_at_ms < selected->created_at_ms);
      if (preferred)
        selected = &item;
    }
    if (!selected)
      return std::nullopt;
    selected->state = SpeechState::synthesizing;
    ++selected->revision;
    return *selected;
  }

  SpeechMutationStatus
  transition(const SpeechTransitionRequest &request) override {
    const std::scoped_lock lock{mutex_};
    ++transition_attempts_;
    if (transition_failures_ > 0) {
      --transition_failures_;
      throw std::runtime_error{"injected speech transition failure"};
    }
    const auto found = items_.find(request.speech_id);
    if (found == items_.end())
      return SpeechMutationStatus::not_found;
    auto &item = found->second;
    if (item.revision != request.expected_revision)
      return SpeechMutationStatus::stale;
    if (!speech_transition_allowed(item.state, request.target))
      return SpeechMutationStatus::invalid_state;
    item.state = request.target;
    ++item.revision;
    if (terminal(request.target) ||
        (request.target == SpeechState::ready &&
         item.priority != SpeechPriority::event_narration))
      item.text.reset();
    if (request.provider_request_id)
      item.provider_request_id = request.provider_request_id;
    if (request.cache_key)
      item.cache_key = request.cache_key;
    if (request.cache_checksum)
      item.cache_checksum = request.cache_checksum;
    if (request.marker)
      item.marker = request.marker;
    if (request.duration_ms)
      item.duration_ms = request.duration_ms;
    if (terminal(request.target)) {
      item.terminal_at_ms = request.occurred_at_ms;
      item.last_error_code = request.error_code;
    }
    return SpeechMutationStatus::applied;
  }

  std::optional<SpeechItem> find(std::string_view speech_id) override {
    const std::scoped_lock lock{mutex_};
    const auto found = items_.find(speech_id);
    return found == items_.end() ? std::nullopt
                                 : std::optional<SpeechItem>{found->second};
  }

  std::size_t cancel_session(std::string_view voice_session_id,
                             std::int64_t now_ms, std::string_view reason,
                             bool include_interactive,
                             bool preserve_event_narration = false) override {
    const std::scoped_lock lock{mutex_};
    std::size_t count{};
    for (auto &[id, item] : items_) {
      static_cast<void>(id);
      if (item.voice_session_id != voice_session_id || terminal(item.state) ||
          (!include_interactive &&
           item.priority == SpeechPriority::interactive))
        continue;
      if (preserve_event_narration &&
          item.priority == SpeechPriority::event_narration &&
          item.state != SpeechState::playing) {
        if (item.state != SpeechState::pending) {
          item.state = SpeechState::pending;
          ++item.revision;
        }
        continue;
      }
      item.state = SpeechState::cancelled;
      ++item.revision;
      item.text.reset();
      item.terminal_at_ms = now_ms;
      item.last_error_code = std::string{reason};
      ++count;
    }
    return count;
  }

  std::size_t recover(std::int64_t now_ms, std::string_view reason) override {
    const std::scoped_lock lock{mutex_};
    std::size_t count{};
    for (auto &[id, item] : items_) {
      static_cast<void>(id);
      if (terminal(item.state))
        continue;
      item.state = SpeechState::cancelled;
      ++item.revision;
      item.text.reset();
      item.terminal_at_ms = now_ms;
      item.last_error_code = std::string{reason};
      ++count;
    }
    return count;
  }
  void ensure_purge_schedule(std::int64_t, std::string) override {}
  std::size_t purge_retained(std::int64_t) override { return 0; }

  TtsUsageReservationResult
  reserve_usage(const TtsUsageReservationRequest &) override {
    return {.accepted = true, .replay = false, .usage = {}};
  }

  SpeechMutationStatus complete_usage(const TtsUsageCompletion &) override {
    return SpeechMutationStatus::applied;
  }

  SpeechMutationStatus release_usage(std::string_view) override {
    return SpeechMutationStatus::applied;
  }

  std::optional<TtsCacheMetadata> cache_metadata(std::string_view,
                                                 std::int64_t) override {
    return std::nullopt;
  }

  void put_cache_metadata(const TtsCacheMetadata &) override {}
  void remove_cache_metadata(std::string_view) override {}
  std::vector<std::string> cache_keys() override { return {}; }

  std::string selected_voice(std::string_view) override { return "onyx"; }

  SpeechMutationStatus select_voice(std::string_view, std::string_view voice,
                                    std::string_view, std::int64_t) override {
    return voice == "onyx" ? SpeechMutationStatus::unchanged
                           : SpeechMutationStatus::invalid_state;
  }

  SpeechRepositoryHealth health(std::int64_t, std::int64_t) override {
    const std::scoped_lock lock{mutex_};
    SpeechRepositoryHealth result;
    for (const auto &[id, item] : items_) {
      static_cast<void>(id);
      if (item.state == SpeechState::pending)
        ++result.queued;
      else if (item.state == SpeechState::synthesizing)
        ++result.synthesizing;
      else if (item.state == SpeechState::ready)
        ++result.ready;
      else if (item.state == SpeechState::playing)
        ++result.playing;
    }
    return result;
  }

  void fail_next_transitions(const std::size_t count) {
    const std::scoped_lock lock{mutex_};
    transition_failures_ = count;
  }

  [[nodiscard]] std::size_t transition_attempts() const {
    const std::scoped_lock lock{mutex_};
    return transition_attempts_;
  }

  [[nodiscard]] std::vector<SpeechItem> items() const {
    const std::scoped_lock lock{mutex_};
    std::vector<SpeechItem> result;
    result.reserve(items_.size());
    for (const auto &[id, item] : items_) {
      static_cast<void>(id);
      result.push_back(item);
    }
    return result;
  }

private:
  static bool terminal(const SpeechState state) noexcept {
    return state == SpeechState::played || state == SpeechState::failed ||
           state == SpeechState::expired || state == SpeechState::cancelled;
  }

  mutable std::mutex mutex_;
  std::map<std::string, SpeechItem, std::less<>> items_;
  std::size_t transition_failures_{};
  std::size_t transition_attempts_{};
};

class FakeAudioNormalizer final : public AudioNormalizer {
public:
  NormalizedAudio normalize(const SynthesizedAudio &,
                            const AudioNormalizationLimits &,
                            std::stop_token) const override {
    return {.pcm = PcmAudio{.samples = {0, 0}}, .duration_ms = 1};
  }
};

class FakeTtsCache final : public TtsCache {
public:
  std::optional<PcmAudio> read(std::string_view key,
                               std::string_view expected_checksum) override {
    const std::scoped_lock lock{mutex_};
    const auto found = entries_.find(std::string{key});
    if (found == entries_.end() || found->second.first != expected_checksum) {
      ++health_.misses;
      return std::nullopt;
    }
    ++health_.hits;
    return found->second.second;
  }
  TtsCacheMutationResult write(std::string_view key,
                               const PcmAudio &audio) override {
    const auto bytes = validated_pcm_bytes(audio);
    std::unique_lock lock{mutex_};
    if (block_writes_) {
      write_entered_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return !block_writes_; });
    }
    entries_[std::string{key}] = {sha256_hex(bytes), audio};
    health_.entries = entries_.size();
    health_.bytes = 0;
    for (const auto &[entry_key, entry] : entries_) {
      static_cast<void>(entry_key);
      health_.bytes += validated_pcm_bytes(entry.second).size();
    }
    return {};
  }
  void erase(std::string_view key) noexcept override {
    const std::scoped_lock lock{mutex_};
    entries_.erase(std::string{key});
    health_.entries = entries_.size();
  }
  void record_miss() noexcept override {
    const std::scoped_lock lock{mutex_};
    ++health_.misses;
  }
  TtsCacheMutationResult purge() override {
    const std::scoped_lock lock{mutex_};
    if (purge_fails_)
      throw std::runtime_error{"Injected TTS cache purge failure."};
    auto result = TtsCacheMutationResult{.removed_keys = purge_removed_keys_};
    for (const auto &key : purge_removed_keys_)
      entries_.erase(key);
    purge_removed_keys_.clear();
    health_.entries = entries_.size();
    return result;
  }
  std::vector<std::string> keys() const override {
    const std::scoped_lock lock{mutex_};
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto &[key, entry] : entries_) {
      static_cast<void>(entry);
      result.push_back(key);
    }
    return result;
  }
  void set_purge_removed_keys(std::vector<std::string> keys) {
    const std::scoped_lock lock{mutex_};
    purge_removed_keys_ = std::move(keys);
  }
  void fail_purge() {
    const std::scoped_lock lock{mutex_};
    purge_fails_ = true;
  }
  void block_writes() {
    const std::scoped_lock lock{mutex_};
    block_writes_ = true;
    write_entered_ = false;
  }
  [[nodiscard]] bool wait_for_write(std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this] { return write_entered_; });
  }
  void release_writes() {
    const std::scoped_lock lock{mutex_};
    block_writes_ = false;
    changed_.notify_all();
  }
  TtsCacheHealth health() const override {
    const std::scoped_lock lock{mutex_};
    return health_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::map<std::string, std::pair<std::string, PcmAudio>, std::less<>> entries_;
  std::vector<std::string> purge_removed_keys_;
  TtsCacheHealth health_;
  bool block_writes_{};
  bool write_entered_{};
  bool purge_fails_{};
};

} // namespace sanguinius::test
