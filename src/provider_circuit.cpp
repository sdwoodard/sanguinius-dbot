#include "sanguinius/provider_circuit.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] ProviderCircuitFailure tts_failure(const TtsError &error) {
  if (error.category() == TtsFailureCategory::authentication)
    return ProviderCircuitFailure::authentication;
  switch (error.category()) {
  case TtsFailureCategory::timeout:
  case TtsFailureCategory::transport:
  case TtsFailureCategory::rate_limited:
  case TtsFailureCategory::provider_unavailable:
  case TtsFailureCategory::invalid_media:
  case TtsFailureCategory::oversized_media:
  case TtsFailureCategory::decoder_failed:
    return ProviderCircuitFailure::retryable;
  case TtsFailureCategory::authentication:
    return ProviderCircuitFailure::authentication;
  case TtsFailureCategory::cancelled:
  case TtsFailureCategory::invalid_request:
  case TtsFailureCategory::provider_rejected:
  case TtsFailureCategory::budget_exhausted:
  case TtsFailureCategory::circuit_open:
  case TtsFailureCategory::cache_failed:
  case TtsFailureCategory::unavailable:
    break;
  }
  if (error.retryable())
    return ProviderCircuitFailure::retryable;
  return ProviderCircuitFailure::ignored;
}

[[nodiscard]] ProviderCircuitFailure
transcription_failure(const TranscriptionError &error) {
  switch (error.category()) {
  case TranscriptionFailureCategory::authentication:
    return ProviderCircuitFailure::authentication;
  case TranscriptionFailureCategory::timeout:
  case TranscriptionFailureCategory::rate_limited:
  case TranscriptionFailureCategory::provider_unavailable:
  case TranscriptionFailureCategory::invalid_response:
  case TranscriptionFailureCategory::oversized_response:
  case TranscriptionFailureCategory::transport:
    return ProviderCircuitFailure::retryable;
  case TranscriptionFailureCategory::cancelled:
  case TranscriptionFailureCategory::invalid_request:
  case TranscriptionFailureCategory::provider_rejected:
  case TranscriptionFailureCategory::unavailable:
    return ProviderCircuitFailure::ignored;
  }
  return ProviderCircuitFailure::ignored;
}

} // namespace

CircuitBreakingTextToSpeechClient::CircuitBreakingTextToSpeechClient(
    std::unique_ptr<TextToSpeechClient> provider,
    std::unique_ptr<ProviderCircuitRepository> circuit, const Clock &clock,
    PersistentIdGenerator &ids)
    : provider_{std::move(provider)}, circuit_{std::move(circuit)},
      clock_{clock}, ids_{ids} {
  if (!provider_ || !circuit_)
    throw std::invalid_argument{"TTS provider circuit is incomplete."};
  circuit_->restart("openai_tts", now_ms(clock_), ids_.next_id());
}

SynthesizedAudio CircuitBreakingTextToSpeechClient::synthesize(
    const TtsRequest &request, const std::stop_token stop_token) const {
  const auto current = now_ms(clock_);
  if (!circuit_->admit("openai_tts", current, ids_.next_id()))
    throw TtsError{TtsFailureCategory::circuit_open,
                   "TTS provider circuit is open."};
  try {
    return provider_->synthesize(request, stop_token);
  } catch (const TtsError &error) {
    circuit_->failed("openai_tts", tts_failure(error),
                     tts_failure_category_name(error.category()),
                     now_ms(clock_), ids_.next_id());
    throw;
  } catch (...) {
    circuit_->failed("openai_tts", ProviderCircuitFailure::retryable,
                     "unexpected_failure", now_ms(clock_), ids_.next_id());
    throw;
  }
}

void CircuitBreakingTextToSpeechClient::provider_response_validated() const {
  circuit_->succeeded("openai_tts", now_ms(clock_), ids_.next_id());
}

void CircuitBreakingTextToSpeechClient::provider_response_rejected(
    const TtsFailureCategory category) const {
  const TtsError error{category, "TTS response failed local validation."};
  circuit_->failed("openai_tts", tts_failure(error),
                   tts_failure_category_name(category), now_ms(clock_),
                   ids_.next_id());
}

std::string CircuitBreakingTextToSpeechClient::provider_circuit_state() const {
  return circuit_->state("openai_tts");
}

CircuitBreakingTranscriptionClient::CircuitBreakingTranscriptionClient(
    std::unique_ptr<TranscriptionClient> provider,
    std::unique_ptr<ProviderCircuitRepository> circuit, const Clock &clock,
    PersistentIdGenerator &ids)
    : provider_{std::move(provider)}, circuit_{std::move(circuit)},
      clock_{clock}, ids_{ids} {
  if (!provider_ || !circuit_)
    throw std::invalid_argument{
        "Transcription provider circuit is incomplete."};
  circuit_->restart("openai_transcription", now_ms(clock_), ids_.next_id());
}

Transcript CircuitBreakingTranscriptionClient::transcribe(
    const TranscriptionRequest &request, const std::stop_token stop_token,
    const std::function<void()> &transmission_started) const {
  const auto current = now_ms(clock_);
  if (!circuit_->admit("openai_transcription", current, ids_.next_id()))
    throw TranscriptionError{TranscriptionFailureCategory::provider_unavailable,
                             "Transcription provider circuit is open."};
  try {
    auto result =
        provider_->transcribe(request, stop_token, transmission_started);
    circuit_->succeeded("openai_transcription", now_ms(clock_), ids_.next_id());
    return result;
  } catch (const TranscriptionError &error) {
    circuit_->failed("openai_transcription", transcription_failure(error),
                     transcription_failure_category_name(error.category()),
                     now_ms(clock_), ids_.next_id());
    throw;
  } catch (...) {
    circuit_->failed("openai_transcription", ProviderCircuitFailure::retryable,
                     "unexpected_failure", now_ms(clock_), ids_.next_id());
    throw;
  }
}

std::string CircuitBreakingTranscriptionClient::provider_circuit_state() const {
  return circuit_->state("openai_transcription");
}

} // namespace sanguinius
