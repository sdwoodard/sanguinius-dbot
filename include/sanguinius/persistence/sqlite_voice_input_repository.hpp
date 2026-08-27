#pragma once

#include "sanguinius/voice_input.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext;

class SqliteVoiceListeningRepository final : public VoiceListeningRepository {
public:
  explicit SqliteVoiceListeningRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  void record_consent_attestation(bool attested,
                                  DiscordSnowflake owner_user_id,
                                  std::string attestation_id,
                                  std::int64_t now_ms) override;
  [[nodiscard]] VoiceWindowBeginResult
  begin(const VoiceWindowBeginRequest &request,
        const TranscriptionUsagePolicy &policy) override;
  [[nodiscard]] std::optional<VoiceListeningWindow> active() override;
  [[nodiscard]] std::optional<VoiceListeningWindow>
  transition(const VoiceWindowTransitionRequest &request) override;
  [[nodiscard]] std::optional<VoiceListeningWindow>
  complete_transcription(const VoiceWindowTransitionRequest &request,
                         const VoiceTranscriptionUsage &usage) override;
  void record_public_message(std::string_view window_id,
                             DiscordSnowflake message_id,
                             std::int64_t now_ms) override;
  void record_usage(const VoiceTranscriptionUsage &usage) override;
  void record_provider_attempt(std::string_view window_id,
                               std::int64_t now_ms) override;
  void release_reservation(std::string_view window_id,
                           std::int64_t now_ms) override;
  [[nodiscard]] bool kill_switch_enabled() override;
  void set_kill_switch(bool enabled, DiscordSnowflake actor_user_id,
                       std::string change_id,
                       std::int64_t now_ms) override;
  [[nodiscard]] std::size_t abandon_nonterminal(
      std::int64_t now_ms, std::string_view reason,
      std::string_view transition_prefix) override;
  [[nodiscard]] VoiceListeningRepositoryHealth
  health(std::int64_t now_ms) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
