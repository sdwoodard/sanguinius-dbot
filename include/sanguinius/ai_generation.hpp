#pragma once

#include "sanguinius/ai_client.hpp"
#include "sanguinius/clock.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/server_scope_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace sanguinius {

struct AiGenerationPolicy {
  std::int64_t rolling_day_micro_usd{1'250'000};
  std::int64_t calendar_month_micro_usd{25'000'000};
  std::size_t rolling_day_generations{300};
  std::size_t direct_user_ten_minute_generations{30};
  std::size_t maximum_input_bytes{16'000};
  std::size_t maximum_output_tokens{500};
  std::int64_t input_rate_micro_usd_per_million_tokens{};
  std::int64_t output_rate_micro_usd_per_million_tokens{};
  std::string model;
};

enum class AiGenerationAdmission {
  accepted,
  duplicate,
  disabled,
  daily_count,
  direct_rate,
  daily_cost,
  monthly_cost,
  provider_unavailable,
};

enum class ProviderCircuitAdmission { allowed, open };

enum class AiFailureAccounting { cancel_reservation, fail_attempt };

struct AiProviderFailureAccounting {
  AiFailureAccounting action{AiFailureAccounting::cancel_reservation};
  std::string_view result_code;
};

[[nodiscard]] AiProviderFailureAccounting
classify_ai_provider_failure(AiProviderErrorCategory category,
                             bool transmission_started) noexcept;

struct AiGenerationReservation {
  std::string attempt_id;
  std::string idempotency_key;
  std::optional<std::string> requester_user_id;
  AiPurpose purpose{AiPurpose::direct};
  AiPriority priority{AiPriority::direct};
  std::string model;
  std::int64_t input_rate{};
  std::int64_t output_rate{};
  std::size_t reserved_input_tokens{};
  std::size_t reserved_output_tokens{};
  std::int64_t reserved_micro_usd{};
  std::int64_t now_ms{};
};

struct AiGenerationAdmissionResult {
  AiGenerationAdmission status{AiGenerationAdmission::disabled};
  std::string attempt_id;
};

struct AiGenerationUsage {
  std::size_t input_tokens{};
  std::size_t output_tokens{};
  std::int64_t micro_usd{};
  std::string provider_request_id;
  std::int64_t completed_at_ms{};
};

struct AiGenerationHealth {
  bool operator_disabled{};
  std::string circuit_state{"closed"};
  std::size_t rolling_day_generations{};
  std::int64_t rolling_day_micro_usd{};
  std::int64_t calendar_month_micro_usd{};
  bool generation_limit_exhausted{};
  bool rolling_day_budget_exhausted{};
  bool calendar_month_budget_exhausted{};
};

class AiGenerationRepository {
public:
  virtual ~AiGenerationRepository() = default;
  [[nodiscard]] virtual AiGenerationAdmissionResult
  reserve(const DiscordSnowflake &guild_id,
          const AiGenerationReservation &reservation,
          const AiGenerationPolicy &policy) = 0;
  virtual void mark_submitted(std::string_view attempt_id,
                              std::int64_t now_ms) = 0;
  virtual void complete(std::string_view attempt_id,
                        const AiGenerationUsage &usage) = 0;
  virtual void fail(std::string_view attempt_id, std::string_view result_code,
                    std::string_view provider_request_id,
                    std::int64_t now_ms) = 0;
  virtual void cancel(std::string_view attempt_id, std::string_view result_code,
                      std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t recover_reserved(std::int64_t now_ms) = 0;
  [[nodiscard]] virtual std::size_t recover_submitted(std::int64_t now_ms) = 0;
  virtual void restart_provider(std::int64_t now_ms,
                                std::string transition_id) = 0;
  [[nodiscard]] virtual ProviderCircuitAdmission
  admit_provider(std::int64_t now_ms, std::string transition_id) = 0;
  virtual void provider_succeeded(std::int64_t now_ms,
                                  std::string transition_id) = 0;
  virtual void release_provider_probe(std::int64_t now_ms,
                                      std::string transition_id) = 0;
  virtual void provider_failed(AiProviderErrorCategory category,
                               std::int64_t now_ms,
                               std::string transition_id) = 0;
  [[nodiscard]] virtual AiGenerationHealth
  health(const DiscordSnowflake &guild_id, std::int64_t now_ms,
         const AiGenerationPolicy &policy) = 0;
};

class AiGenerationAdmissionError : public std::runtime_error {
public:
  explicit AiGenerationAdmissionError(AiGenerationAdmission status);
  [[nodiscard]] AiGenerationAdmission status() const noexcept;

private:
  AiGenerationAdmission status_;
};

class AiGenerationService final : public AiClient {
public:
  AiGenerationService(std::unique_ptr<AiClient> provider,
                      std::unique_ptr<AiGenerationRepository> repository,
                      const Clock &clock, PersistentIdGenerator &ids,
                      ServerScopeConfiguration scope,
                      AiGenerationPolicy policy);

  [[nodiscard]] AiResult generate(
      const AiRequest &request, std::stop_token stop_token,
      const std::function<void()> &transmission_started = {}) const override;
  [[nodiscard]] AiGenerationHealth health() const;

private:
  std::unique_ptr<AiClient> provider_;
  std::unique_ptr<AiGenerationRepository> repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  ServerScopeConfiguration scope_;
  AiGenerationPolicy policy_;
};

[[nodiscard]] std::int64_t
estimated_ai_cost_micro_usd(std::size_t input_tokens, std::size_t output_tokens,
                            std::int64_t input_rate_micro_usd_per_million,
                            std::int64_t output_rate_micro_usd_per_million);

} // namespace sanguinius
