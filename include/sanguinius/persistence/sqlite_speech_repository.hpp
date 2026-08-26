#pragma once

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/speech.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteSpeechRepository final : public SpeechRepository {
public:
  explicit SqliteSpeechRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] SpeechEnqueueResult
  enqueue(const SpeechEnqueueRequest &request) override;
  [[nodiscard]] std::optional<SpeechItem>
  claim_next(std::string_view voice_session_id, std::int64_t now_ms,
             std::string transition_id,
             std::string idempotency_key) override;
  [[nodiscard]] SpeechMutationStatus
  transition(const SpeechTransitionRequest &request) override;
  [[nodiscard]] std::optional<SpeechItem>
  find(std::string_view speech_id) override;
  [[nodiscard]] std::size_t
  cancel_session(std::string_view voice_session_id, std::int64_t now_ms,
                 std::string_view reason, bool include_interactive) override;
  [[nodiscard]] std::size_t recover(std::int64_t now_ms,
                                    std::string_view reason) override;
  void ensure_purge_schedule(std::int64_t now_ms,
                             std::string job_id) override;
  [[nodiscard]] std::size_t purge_retained(std::int64_t now_ms) override;
  [[nodiscard]] TtsUsageReservationResult
  reserve_usage(const TtsUsageReservationRequest &request) override;
  [[nodiscard]] SpeechMutationStatus
  complete_usage(const TtsUsageCompletion &completion) override;
  [[nodiscard]] std::optional<TtsCacheMetadata>
  cache_metadata(std::string_view cache_key,
                 std::int64_t accessed_at_ms) override;
  void put_cache_metadata(const TtsCacheMetadata &metadata) override;
  void remove_cache_metadata(std::string_view cache_key) override;
  [[nodiscard]] std::vector<std::string> cache_keys() override;
  [[nodiscard]] std::string
  selected_voice(std::string_view guild_id) override;
  [[nodiscard]] SpeechMutationStatus
  select_voice(std::string_view guild_id, std::string_view voice,
               std::string_view actor_user_id,
               std::int64_t now_ms) override;
  [[nodiscard]] SpeechRepositoryHealth
  health(std::int64_t now_ms,
         std::int64_t calendar_month_start_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
