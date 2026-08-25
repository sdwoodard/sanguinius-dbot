#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sanguinius {

inline constexpr std::string_view tarot_integration_job_type{
    "tarot.integration-scan.v1"};
inline constexpr std::string_view tarot_house_deadline_job_type{
    "tarot.house-deadline.v1"};
inline constexpr std::string_view tarot_house_offer_expiry_job_type{
    "tarot.house-offer-expiry.v1"};
inline constexpr std::string_view tarot_house_weekly_offer_job_type{
    "tarot.house-weekly-offer.v1"};

struct TarotIntegrationReport {
  std::size_t pending{};
  std::size_t completed{};
  std::size_t suppressed{};
  std::size_t failed{};
  std::size_t effects{};
};

struct TarotIntegrationSinkPolicy {
  bool chronicle_enabled{true};
};

class TarotIntegrationRepository {
public:
  virtual ~TarotIntegrationRepository() = default;
  virtual void ensure_schedule(std::int64_t now_ms,
                               std::string job_id) = 0;
  [[nodiscard]] virtual TarotIntegrationReport
  scan(std::int64_t now_ms, std::size_t limit,
       std::function<std::string()> next_id,
       TarotIntegrationSinkPolicy sink_policy = {}) = 0;
  [[nodiscard]] virtual bool retry(std::string_view source_event_id,
                                   std::int64_t now_ms) = 0;
  [[nodiscard]] virtual TarotIntegrationReport inspect() = 0;
};

class TarotIntegrationService {
public:
  TarotIntegrationService(TarotIntegrationRepository &repository,
                          const Clock &clock, PersistentIdGenerator &ids,
                          Diagnostics &diagnostics, bool enabled,
                          TarotIntegrationSinkPolicy sink_policy);

  [[nodiscard]] TarotIntegrationReport scan();
  void ensure_schedule();
  void validate_scan_job(const ClaimedScheduledJob &job) const;
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] InteractionMessage
  preview(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage retry(const IncomingInteraction &interaction);
  void observe_committed_event(std::string_view event_type) noexcept;

private:
  TarotIntegrationRepository &repository_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Diagnostics &diagnostics_;
  bool enabled_{};
  TarotIntegrationSinkPolicy sink_policy_;
};

} // namespace sanguinius
