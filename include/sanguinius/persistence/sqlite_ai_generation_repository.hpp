#pragma once

#include "sanguinius/ai_generation.hpp"

#include <memory>

namespace sanguinius::persistence {

class SqliteRepositoryContext;

class SqliteAiGenerationRepository final : public AiGenerationRepository {
public:
  explicit SqliteAiGenerationRepository(
      std::shared_ptr<SqliteRepositoryContext> context);

  [[nodiscard]] AiGenerationAdmissionResult
  reserve(const DiscordSnowflake &guild_id,
          const AiGenerationReservation &reservation,
          const AiGenerationPolicy &policy) override;
  void mark_submitted(std::string_view attempt_id,
                      std::int64_t now_ms) override;
  void complete(std::string_view attempt_id,
                const AiGenerationUsage &usage) override;
  void fail(std::string_view attempt_id, std::string_view result_code,
            std::string_view provider_request_id, std::int64_t now_ms) override;
  void cancel(std::string_view attempt_id, std::string_view result_code,
              std::int64_t now_ms) override;
  [[nodiscard]] std::size_t recover_reserved(std::int64_t now_ms) override;
  [[nodiscard]] std::size_t recover_submitted(std::int64_t now_ms) override;
  void restart_provider(std::int64_t now_ms,
                        std::string transition_id) override;
  [[nodiscard]] ProviderCircuitAdmission
  admit_provider(std::int64_t now_ms, std::string transition_id) override;
  void provider_succeeded(std::int64_t now_ms,
                          std::string transition_id) override;
  void release_provider_probe(std::int64_t now_ms,
                              std::string transition_id) override;
  void provider_failed(AiProviderErrorCategory category, std::int64_t now_ms,
                       std::string transition_id) override;
  [[nodiscard]] AiGenerationHealth
  health(const DiscordSnowflake &guild_id, std::int64_t now_ms,
         const AiGenerationPolicy &policy) override;

private:
  std::shared_ptr<SqliteRepositoryContext> context_;
};

} // namespace sanguinius::persistence
