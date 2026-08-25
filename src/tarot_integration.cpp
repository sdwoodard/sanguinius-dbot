#include "sanguinius/tarot_integration.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace sanguinius {
namespace {

[[nodiscard]] const InteractionOption *
option(const IncomingInteraction &interaction, const std::string_view name) {
  const auto found = std::ranges::find(interaction.command_options, name,
                                       &InteractionOption::name);
  return found == interaction.command_options.end() ? nullptr : &*found;
}

} // namespace

TarotIntegrationService::TarotIntegrationService(
    TarotIntegrationRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, Diagnostics &diagnostics, const bool enabled,
    const TarotIntegrationSinkPolicy sink_policy)
    : repository_{repository}, clock_{clock}, ids_{ids},
      diagnostics_{diagnostics}, enabled_{enabled}, sink_policy_{sink_policy} {}

TarotIntegrationReport TarotIntegrationService::scan() {
  if (!enabled_)
    return repository_.inspect();
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock_.now().time_since_epoch())
                       .count();
  return repository_.scan(now, 32, [this] { return ids_.next_id(); },
                          sink_policy_);
}

void TarotIntegrationService::ensure_schedule() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock_.now().time_since_epoch())
                       .count();
  repository_.ensure_schedule(now, ids_.next_id());
}

void TarotIntegrationService::validate_scan_job(
    const ClaimedScheduledJob &job) const {
  const auto *payload =
      std::get_if<TarotIntegrationScanJobPayload>(&job.payload);
  if (job.job_type != tarot_integration_job_type || payload == nullptr ||
      payload->schedule_key != "singleton")
    throw std::invalid_argument{"Invalid Tarot integration scan payload."};
}

bool TarotIntegrationService::enabled() const noexcept { return enabled_; }

InteractionMessage
TarotIntegrationService::preview(const IncomingInteraction &) {
  const auto report = repository_.inspect();
  std::ostringstream output;
  output << "Tarot integration " << (enabled_ ? "enabled" : "disabled")
         << "\nPending " << report.pending << " · completed "
         << report.completed << " · suppressed " << report.suppressed
         << " · failed " << report.failed << " · effects " << report.effects;
  return text_message(output.str());
}

InteractionMessage
TarotIntegrationService::retry(const IncomingInteraction &interaction) {
  const auto *candidate = option(interaction, "reference");
  const auto *reference =
      candidate == nullptr ? nullptr : std::get_if<std::string>(&candidate->value);
  if (reference == nullptr)
    throw std::invalid_argument{"Integration retry requires a source event UUID."};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock_.now().time_since_epoch())
                       .count();
  if (!repository_.retry(*reference, now))
    return text_message("That integration observation is unavailable.");
  const auto report = scan();
  return text_message("Integration retry processed; pending " +
                      std::to_string(report.pending) + ".");
}

void TarotIntegrationService::observe_committed_event(
    const std::string_view) noexcept {
  if (!enabled_)
    return;
  try {
    static_cast<void>(scan());
  } catch (const std::exception &error) {
    diagnostics_.emit({DiagnosticSeverity::error, "tarot.integration_scan",
                       error.what(), {}});
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error,
                       "tarot.integration_scan",
                       "Unknown Tarot integration failure.",
                       {}});
  }
}

} // namespace sanguinius
