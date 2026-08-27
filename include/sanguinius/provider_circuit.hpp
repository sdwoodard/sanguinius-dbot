#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/transcription.hpp"
#include "sanguinius/tts.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace sanguinius {

enum class ProviderCircuitFailure { ignored, retryable, authentication };

class ProviderCircuitRepository {
public:
  virtual ~ProviderCircuitRepository() = default;
  virtual void restart(std::string_view provider, std::int64_t now_ms,
                       std::string transition_id) = 0;
  [[nodiscard]] virtual bool admit(std::string_view provider,
                                   std::int64_t now_ms,
                                   std::string transition_id) = 0;
  [[nodiscard]] virtual std::string state(std::string_view provider) const = 0;
  virtual void succeeded(std::string_view provider, std::int64_t now_ms,
                         std::string transition_id) = 0;
  virtual void failed(std::string_view provider, ProviderCircuitFailure failure,
                      std::string_view reason_code, std::int64_t now_ms,
                      std::string transition_id) = 0;
};

class CircuitBreakingTextToSpeechClient final : public TextToSpeechClient {
public:
  CircuitBreakingTextToSpeechClient(
      std::unique_ptr<TextToSpeechClient> provider,
      std::unique_ptr<ProviderCircuitRepository> circuit, const Clock &clock,
      PersistentIdGenerator &ids);

  [[nodiscard]] SynthesizedAudio
  synthesize(const TtsRequest &request,
             std::stop_token stop_token) const override;
  void provider_response_validated() const override;
  void provider_response_rejected(TtsFailureCategory category) const override;
  [[nodiscard]] std::string provider_circuit_state() const override;

private:
  std::unique_ptr<TextToSpeechClient> provider_;
  std::unique_ptr<ProviderCircuitRepository> circuit_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
};

class CircuitBreakingTranscriptionClient final : public TranscriptionClient {
public:
  CircuitBreakingTranscriptionClient(
      std::unique_ptr<TranscriptionClient> provider,
      std::unique_ptr<ProviderCircuitRepository> circuit, const Clock &clock,
      PersistentIdGenerator &ids);

  [[nodiscard]] Transcript transcribe(
      const TranscriptionRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started = {}) const override;
  [[nodiscard]] std::string provider_circuit_state() const override;

private:
  std::unique_ptr<TranscriptionClient> provider_;
  std::unique_ptr<ProviderCircuitRepository> circuit_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
};

} // namespace sanguinius
