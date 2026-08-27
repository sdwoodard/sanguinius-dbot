#include "sanguinius/ai_generation.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::size_t request_bytes(const AiRequest &request) {
  std::size_t total = request.instructions.size();
  for (const auto &message : request.conversation) {
    if (message.role.size() > std::numeric_limits<std::size_t>::max() - total)
      throw std::invalid_argument{"AI request is too large."};
    total += message.role.size();
    if (message.content.size() >
        std::numeric_limits<std::size_t>::max() - total)
      throw std::invalid_argument{"AI request is too large."};
    total += message.content.size();
  }
  if (request.json_schema) {
    if (request.json_schema->schema.size() >
        std::numeric_limits<std::size_t>::max() - total)
      throw std::invalid_argument{"AI request is too large."};
    total += request.json_schema->schema.size();
  }
  return total;
}

[[nodiscard]] const char *failure_code(const AiProviderErrorCategory category) {
  switch (category) {
  case AiProviderErrorCategory::timeout:
    return "timeout";
  case AiProviderErrorCategory::rate_limited:
    return "rate_limited";
  case AiProviderErrorCategory::authentication:
    return "authentication";
  case AiProviderErrorCategory::server:
    return "server";
  case AiProviderErrorCategory::invalid_request:
    return "invalid_request";
  case AiProviderErrorCategory::invalid_response:
    return "invalid_response";
  case AiProviderErrorCategory::transport:
    return "transport";
  }
  return "provider_failure";
}

} // namespace

std::int64_t estimated_ai_cost_micro_usd(const std::size_t input_tokens,
                                         const std::size_t output_tokens,
                                         const std::int64_t input_rate,
                                         const std::int64_t output_rate) {
  if (input_rate <= 0 || output_rate <= 0 || input_tokens > 16'000 ||
      output_tokens > 500)
    throw std::invalid_argument{"AI cost input is invalid."};
  const auto charge = [](const std::size_t tokens, const std::int64_t rate) {
    const auto amount = static_cast<std::int64_t>(tokens);
    if (amount > (std::numeric_limits<std::int64_t>::max() - 999'999) / rate)
      throw std::overflow_error{"AI cost calculation overflowed."};
    return (amount * rate + 999'999) / 1'000'000;
  };
  return charge(input_tokens, input_rate) + charge(output_tokens, output_rate);
}

AiGenerationAdmissionError::AiGenerationAdmissionError(
    const AiGenerationAdmission status)
    : std::runtime_error{status == AiGenerationAdmission::disabled
                             ? "Text generation is disabled."
                         : status == AiGenerationAdmission::provider_unavailable
                             ? "Text generation provider is degraded."
                         : status == AiGenerationAdmission::direct_rate
                             ? "Direct generation rate limit reached."
                             : "Text generation budget is exhausted."},
      status_{status} {}

AiGenerationAdmission AiGenerationAdmissionError::status() const noexcept {
  return status_;
}

AiGenerationService::AiGenerationService(
    std::unique_ptr<AiClient> provider,
    std::unique_ptr<AiGenerationRepository> repository, const Clock &clock,
    PersistentIdGenerator &ids, ServerScopeConfiguration scope,
    AiGenerationPolicy policy)
    : provider_{std::move(provider)}, repository_{std::move(repository)},
      clock_{clock}, ids_{ids}, scope_{scope}, policy_{std::move(policy)} {
  if (!provider_ || !repository_ || !scope_.guild_id.is_set() ||
      policy_.model.empty() ||
      policy_.input_rate_micro_usd_per_million_tokens <= 0 ||
      policy_.output_rate_micro_usd_per_million_tokens <= 0 ||
      policy_.maximum_input_bytes == 0 ||
      policy_.maximum_input_bytes > 16'000 ||
      policy_.maximum_output_tokens == 0 || policy_.maximum_output_tokens > 500)
    throw std::invalid_argument{"AI generation policy is incomplete."};
  const auto current = now_ms(clock_);
  static_cast<void>(repository_->recover_reserved(current));
  static_cast<void>(repository_->recover_submitted(current));
  repository_->restart_provider(current, ids_.next_id());
}

AiResult AiGenerationService::generate(
    const AiRequest &request, const std::stop_token stop_token,
    const std::function<void()> &transmission_started) const {
  if (stop_token.stop_requested())
    throw OperationCancelled{};
  const auto bytes = request_bytes(request);
  if (bytes == 0 || bytes > policy_.maximum_input_bytes ||
      request.max_output_tokens == 0 ||
      request.max_output_tokens > policy_.maximum_output_tokens)
    throw std::invalid_argument{"AI request exceeds configured token limits."};
  const auto attempt_id = ids_.next_id();
  const auto current = now_ms(clock_);
  // The provider's reported input usage includes request framing that is not
  // represented by the caller-owned UTF-8 payload. Reserve the configured
  // ceiling so admission remains conservative, then finalize downward from
  // reported usage after a successful request.
  const auto reserved_input = policy_.maximum_input_bytes;
  const auto cost = estimated_ai_cost_micro_usd(
      reserved_input, request.max_output_tokens,
      policy_.input_rate_micro_usd_per_million_tokens,
      policy_.output_rate_micro_usd_per_million_tokens);
  const auto idempotency = request.idempotency_key.empty()
                               ? "ai:attempt:" + attempt_id
                               : request.idempotency_key;
  const auto admitted = repository_->reserve(
      scope_.guild_id,
      AiGenerationReservation{
          .attempt_id = attempt_id,
          .idempotency_key = idempotency,
          .requester_user_id = request.requester_user_id,
          .purpose = request.purpose,
          .priority = request.priority,
          .model = policy_.model,
          .input_rate = policy_.input_rate_micro_usd_per_million_tokens,
          .output_rate = policy_.output_rate_micro_usd_per_million_tokens,
          .reserved_input_tokens = reserved_input,
          .reserved_output_tokens = request.max_output_tokens,
          .reserved_micro_usd = cost,
          .now_ms = current,
      },
      policy_);
  if (admitted.status != AiGenerationAdmission::accepted)
    throw AiGenerationAdmissionError{admitted.status};
  const auto active_attempt =
      admitted.attempt_id.empty() ? attempt_id : admitted.attempt_id;
  if (repository_->admit_provider(current, ids_.next_id()) !=
      ProviderCircuitAdmission::allowed) {
    repository_->cancel(active_attempt, "circuit_open", current);
    throw AiGenerationAdmissionError{
        AiGenerationAdmission::provider_unavailable};
  }
  bool submitted{};
  const auto release_unsent = [&](const std::string_view reason) {
    try {
      repository_->cancel(active_attempt, reason, now_ms(clock_));
    } catch (...) {
    }
    try {
      repository_->release_provider_probe(now_ms(clock_), ids_.next_id());
    } catch (...) {
    }
  };
  try {
    auto result = provider_->generate(request, stop_token, [&] {
      repository_->mark_submitted(active_attempt, now_ms(clock_));
      submitted = true;
      if (transmission_started)
        transmission_started();
    });
    result.provider_request_id =
        sanitize_ai_provider_request_id(result.provider_request_id);
    if (!submitted)
      throw AiProviderError{AiProviderErrorCategory::invalid_response,
                            result.provider_request_id};
    if (result.input_tokens > policy_.maximum_input_bytes ||
        result.output_tokens > request.max_output_tokens) {
      throw AiProviderError{AiProviderErrorCategory::invalid_response,
                            result.provider_request_id};
    }
    const auto actual_input =
        result.input_tokens == 0 ? reserved_input : result.input_tokens;
    const auto actual_output = result.output_tokens == 0
                                   ? request.max_output_tokens
                                   : result.output_tokens;
    const auto actual_cost = estimated_ai_cost_micro_usd(
        actual_input, actual_output,
        policy_.input_rate_micro_usd_per_million_tokens,
        policy_.output_rate_micro_usd_per_million_tokens);
    if (actual_cost > cost) {
      throw AiProviderError{AiProviderErrorCategory::invalid_response,
                            result.provider_request_id};
    }
    repository_->complete(
        active_attempt,
        AiGenerationUsage{.input_tokens = actual_input,
                          .output_tokens = actual_output,
                          .micro_usd = actual_cost,
                          .provider_request_id = result.provider_request_id,
                          .completed_at_ms = now_ms(clock_)});
    try {
      repository_->provider_succeeded(now_ms(clock_), ids_.next_id());
    } catch (...) {
      // Usage is already finalized; circuit bookkeeping is recovered from
      // subsequent calls and must not reclassify a successful provider call.
    }
    return result;
  } catch (const AiProviderError &error) {
    if (!submitted) {
      release_unsent("provider_not_sent");
      throw;
    }
    repository_->fail(active_attempt, failure_code(error.category()),
                      error.provider_request_id(), now_ms(clock_));
    try {
      repository_->provider_failed(error.category(), now_ms(clock_),
                                   ids_.next_id());
      if (error.category() == AiProviderErrorCategory::invalid_request)
        repository_->provider_succeeded(now_ms(clock_), ids_.next_id());
    } catch (...) {
    }
    throw;
  } catch (const OperationCancelled &) {
    if (submitted) {
      repository_->fail(active_attempt, "cancelled_after_send", {},
                        now_ms(clock_));
      try {
        repository_->release_provider_probe(now_ms(clock_), ids_.next_id());
      } catch (...) {
      }
    } else {
      release_unsent("cancelled_before_send");
    }
    throw;
  } catch (const AiRefusal &error) {
    if (submitted) {
      repository_->fail(active_attempt, "refusal", error.provider_request_id(),
                        now_ms(clock_));
      try {
        repository_->provider_succeeded(now_ms(clock_), ids_.next_id());
      } catch (...) {
      }
    } else {
      release_unsent("refusal_before_send");
    }
    throw;
  } catch (const AiIncompleteResponse &error) {
    if (submitted) {
      repository_->fail(active_attempt, "incomplete",
                        error.provider_request_id(), now_ms(clock_));
      try {
        repository_->release_provider_probe(now_ms(clock_), ids_.next_id());
      } catch (...) {
      }
    } else {
      release_unsent("incomplete_before_send");
    }
    throw;
  } catch (...) {
    if (submitted) {
      repository_->fail(active_attempt, "provider_failure", {}, now_ms(clock_));
      try {
        repository_->release_provider_probe(now_ms(clock_), ids_.next_id());
      } catch (...) {
      }
    } else {
      release_unsent("submission_not_started");
    }
    throw;
  }
}

AiGenerationHealth AiGenerationService::health() const {
  return repository_->health(scope_.guild_id, now_ms(clock_), policy_);
}

} // namespace sanguinius
