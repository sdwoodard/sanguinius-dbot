#include "sanguinius/health.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace sanguinius {
namespace {

constexpr std::size_t maximum_build_metadata_size = 128;

[[nodiscard]] bool safe_metadata_character(const char character) noexcept {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '.' ||
         character == '-' || character == '+' || character == '_';
}

[[nodiscard]] std::string safe_build_metadata(const std::string_view value) {
  if (value.empty()) {
    return "unknown";
  }

  const bool truncated = value.size() > maximum_build_metadata_size;
  const auto content_limit =
      truncated ? maximum_build_metadata_size - 3 : value.size();
  std::string result;
  result.reserve(maximum_build_metadata_size);
  for (std::size_t index = 0; index < content_limit; ++index) {
    result.push_back(safe_metadata_character(value[index]) ? value[index]
                                                           : '_');
  }
  if (truncated) {
    result += "...";
  }
  return result;
}

[[nodiscard]] const char *enabled(const bool value) noexcept {
  return value ? "enabled" : "disabled";
}

void append_queue(std::ostringstream &output, const char *name,
                  const QueueSnapshot &queue) {
  output << name << "_queue=" << queue.queued << '/' << queue.capacity
         << " queued, " << queue.active << " active, "
         << (queue.accepting ? "accepting" : "stopped") << '\n';
}

} // namespace

std::string bounded_health_message(std::string message) {
  if (message.size() <= maximum_health_message_size) {
    return message;
  }

  constexpr std::string_view truncation_marker{"...\n"};
  message.resize(maximum_health_message_size - truncation_marker.size());
  message += truncation_marker;
  return message;
}

HealthService::HealthService(BuildInfo build, ControlConfiguration controls,
                             FeatureConfiguration features,
                             PersistenceHealth persistence,
                             HealthRuntimeProviders runtime)
    : build_{std::move(build)}, controls_{controls}, features_{features},
      persistence_{std::move(persistence)}, runtime_{std::move(runtime)} {
  if (!runtime_.interaction_queue || !runtime_.scheduler_queue ||
      !runtime_.outbox_queue || !runtime_.pending_notice_count ||
      !runtime_.durable_work) {
    throw std::invalid_argument{"Health runtime providers are required."};
  }
}

HealthSnapshot HealthService::snapshot(const QueueSnapshot message_queue,
                                       const QueueSnapshot ai_queue,
                                       const bool scope_matched) const {
  return HealthSnapshot{
      .build = build_,
      .message_queue = message_queue,
      .ai_queue = ai_queue,
      .interaction_queue = runtime_.interaction_queue(),
      .scheduler_queue = runtime_.scheduler_queue(),
      .outbox_queue = runtime_.outbox_queue(),
      .controls = controls_,
      .features = features_,
      .persistence = persistence_,
      .discord = runtime_.discord_status == nullptr
                     ? DiscordRuntimeStatus{}
                     : runtime_.discord_status->status(),
      .pending_notice_count = runtime_.pending_notice_count(),
      .durable_work = runtime_.durable_work(),
      .tarot = runtime_.tarot ? runtime_.tarot() : std::nullopt,
      .wagers = runtime_.wagers ? runtime_.wagers() : std::nullopt,
      .house = runtime_.house ? runtime_.house() : std::nullopt,
      .vox = runtime_.vox ? runtime_.vox() : std::nullopt,
      .scope_matched = scope_matched,
  };
}

std::string render_health(const HealthSnapshot &snapshot) {
  std::ostringstream output;
  output << "Sanguinius health\n"
         << "version=" << safe_build_metadata(snapshot.build.version) << '\n'
         << "revision=" << safe_build_metadata(snapshot.build.revision) << '\n'
         << "scope=" << (snapshot.scope_matched ? "matched" : "rejected")
         << '\n'
         << "database=" << (snapshot.persistence.ready ? "ready" : "failed")
         << '\n'
         << "schema=" << snapshot.persistence.schema_version << '/'
         << snapshot.persistence.target_schema_version << '\n'
         << "sqlite="
         << safe_build_metadata(snapshot.persistence.sqlite_version) << '\n'
         << "instance=" << safe_build_metadata(snapshot.persistence.instance_id)
         << '\n';
  append_queue(output, "message", snapshot.message_queue);
  append_queue(output, "ai", snapshot.ai_queue);
  if (snapshot.interaction_queue.capacity != 0) {
    append_queue(output, "interaction", snapshot.interaction_queue);
  }
  if (snapshot.scheduler_queue.capacity != 0) {
    append_queue(output, "scheduler", snapshot.scheduler_queue);
  }
  if (snapshot.outbox_queue.capacity != 0) {
    append_queue(output, "outbox", snapshot.outbox_queue);
  }
  output << "discord_ready=" << enabled(snapshot.discord.ready) << '\n'
         << "command_catalog=" << snapshot.discord.command_catalog_version
         << '\n'
         << "command_registration="
         << command_registration_state_name(
                snapshot.discord.command_registration)
         << '\n'
         << "pending_notices=" << snapshot.pending_notice_count << '\n'
         << "jobs=" << snapshot.durable_work.pending_jobs << " pending, "
         << snapshot.durable_work.claimed_jobs << " claimed, "
         << snapshot.durable_work.dead_jobs << " dead, "
         << snapshot.durable_work.job_retries << " retries\n"
         << "outbox_work=" << snapshot.durable_work.pending_outbox
         << " pending, " << snapshot.durable_work.claimed_outbox << " claimed, "
         << snapshot.durable_work.failed_outbox << " failed, "
         << snapshot.durable_work.dead_outbox << " dead, "
         << snapshot.durable_work.outbox_retries << " retries\n"
         << "scheduler_lag_ms=" << snapshot.durable_work.scheduler_lag_ms
         << '\n'
         << "outbox_lag_ms=" << snapshot.durable_work.outbox_lag_ms << '\n';
  if (snapshot.durable_work.last_job_error.has_value()) {
    output << "last_job_error="
           << safe_build_metadata(*snapshot.durable_work.last_job_error)
           << '\n';
  }
  if (snapshot.durable_work.last_outbox_error.has_value()) {
    output << "last_outbox_error="
           << safe_build_metadata(*snapshot.durable_work.last_outbox_error)
           << '\n';
  }
  if (snapshot.tarot) {
    output << "tarot_invariants=" << (snapshot.tarot->valid ? "ok" : "failed")
           << '\n'
           << "tarot_accounts=" << snapshot.tarot->account_count << '\n'
           << "tarot_transactions="
           << snapshot.tarot->committed_transaction_count << '\n'
           << "tarot_postings=" << snapshot.tarot->posting_count << '\n'
           << "tarot_prepared=" << snapshot.tarot->prepared_transaction_count
           << '\n'
           << "tarot_violations="
           << snapshot.tarot->unbalanced_transaction_count +
                  snapshot.tarot->negative_history_count +
                  snapshot.tarot->overflow_count +
                  snapshot.tarot->illegal_reversal_count +
                  snapshot.tarot->claim_mismatch_count +
                  snapshot.tarot->orphaned_link_count
           << '\n';
  }
  if (snapshot.wagers) {
    output << "wager_invariants="
           << (snapshot.wagers->valid ? "ok" : "failed") << '\n'
           << "wager_open_funded="
           << snapshot.wagers->open_funded_obligation_count << '\n'
           << "wager_obligation_fate="
           << snapshot.wagers->open_funded_obligation_amount << '\n'
           << "wager_escrow_fate=" << snapshot.wagers->escrow_balance << '\n'
           << "wager_disputes=" << snapshot.wagers->disputed_count << '\n'
           << "wager_violations="
           << snapshot.wagers->malformed_transfer_count +
                  snapshot.wagers->invalid_deadline_action_link_count +
                  snapshot.wagers->orphaned_link_count
           << '\n';
  }
  if (snapshot.house) {
    output << "house_invariants="
           << (snapshot.house->valid ? "ok" : "failed") << '\n'
           << "fate_issued=" << snapshot.house->issued_fate << '\n'
           << "fate_account_total=" << snapshot.house->account_total << '\n'
           << "fate_human_holdings=" << snapshot.house->human_fate << '\n'
           << "fate_house_balance=" << snapshot.house->house_fate << '\n'
           << "fate_recovery_issuance=" << snapshot.house->recovery_issuance
           << '\n'
           << "house_open_funded=" << snapshot.house->open_house_wagers << '\n'
           << "house_exposure_fate=" << snapshot.house->non_test_exposure
           << '\n'
           << "house_test_exposure_fate=" << snapshot.house->test_exposure
           << '\n'
           << "house_escrow_fate=" << snapshot.house->expected_house_escrow
           << '\n'
           << "house_violations="
           << snapshot.house->malformed_transfer_count +
                  snapshot.house->malformed_offer_deadline_count
           << '\n';
  }
  if (snapshot.vox) {
    output << "vox_state="
           << (snapshot.vox->state ? vox_state_name(*snapshot.vox->state)
                                   : "inactive")
           << '\n'
           << "vox_dave=" << enabled(snapshot.vox->dave_active) << '\n'
           << "vox_reconnects=" << snapshot.vox->reconnect_count << '\n'
           << "vox_callback_drops=" << snapshot.vox->callback_drops << '\n'
           << "vox_reconciliations=" << snapshot.vox->reconciliations << '\n';
    append_queue(output, "vox", snapshot.vox->queue);
    if (snapshot.vox->last_failure_category)
      output << "vox_last_failure="
             << safe_build_metadata(*snapshot.vox->last_failure_category)
             << '\n';
    if (snapshot.vox->speech) {
      const auto &speech = *snapshot.vox->speech;
      output << "tts_provider=" << enabled(speech.provider_enabled) << '\n'
             << "tts_voice=" << safe_build_metadata(speech.voice) << '\n'
             << "tts_day_attempts="
             << speech.repository.usage.rolling_day_attempts << '\n'
             << "tts_day_micro_usd="
             << speech.repository.usage.rolling_day_micro_usd << '\n'
             << "tts_month_micro_usd="
             << speech.repository.usage.calendar_month_micro_usd << '\n'
             << "tts_day_remaining_attempts="
             << (speech.repository.usage.rolling_day_attempts >=
                         speech.usage_policy.rolling_day_attempts
                     ? 0
                     : speech.usage_policy.rolling_day_attempts -
                           speech.repository.usage.rolling_day_attempts)
             << '\n'
             << "tts_day_remaining_micro_usd="
             << std::max<std::int64_t>(
                    0, speech.usage_policy.rolling_day_micro_usd -
                           speech.repository.usage.rolling_day_micro_usd)
             << '\n'
             << "tts_month_remaining_micro_usd="
             << std::max<std::int64_t>(
                    0, speech.usage_policy.calendar_month_micro_usd -
                           speech.repository.usage.calendar_month_micro_usd)
             << '\n'
             << "tts_day_outcomes="
             << speech.repository.usage.rolling_day_succeeded << " succeeded, "
             << speech.repository.usage.rolling_day_failed << " failed, "
             << speech.repository.usage.rolling_day_unknown << " unknown\n"
             << "tts_cache_entries=" << speech.cache.entries << '\n'
             << "tts_cache_bytes=" << speech.cache.bytes << '\n'
             << "tts_cache_hits=" << speech.cache.hits << '\n'
             << "tts_cache_misses=" << speech.cache.misses << '\n'
             << "tts_queue_items="
             << speech.repository.queued + speech.repository.synthesizing +
                    speech.repository.ready + speech.repository.playing
             << '\n'
             << "tts_synthesis_worker_rejections="
             << speech.synthesis_worker_rejections << '\n'
             << "tts_playback_worker_rejections="
             << speech.playback_worker_rejections << '\n';
      if (speech.last_normalization_latency_ms)
        output << "tts_last_decoder_ms="
               << *speech.last_normalization_latency_ms << '\n';
      append_queue(output, "tts_synthesis_worker", speech.synthesis_worker);
      append_queue(output, "tts_playback_worker", speech.playback_worker);
      if (speech.last_failure_category)
        output << "tts_last_failure="
               << safe_build_metadata(*speech.last_failure_category) << '\n';
    }
  }
  output << "admin_commands="
         << enabled(snapshot.controls.admin_commands_enabled) << '\n'
         << "test_mode=" << enabled(snapshot.controls.test_mode) << '\n'
         << "chronicle=" << enabled(snapshot.features.chronicle_enabled) << '\n'
         << "tarot=" << enabled(snapshot.features.tarot_enabled) << '\n'
         << "appearances="
         << appearance_mode_name(snapshot.features.appearances_mode) << '\n'
         << "vox=" << enabled(snapshot.features.vox_enabled) << '\n'
         << "voice_input=" << enabled(snapshot.features.voice_input_enabled)
         << '\n';
  return bounded_health_message(output.str());
}

const char *
command_registration_state_name(const CommandRegistrationState state) noexcept {
  switch (state) {
  case CommandRegistrationState::not_started:
    return "not_started";
  case CommandRegistrationState::synchronizing:
    return "synchronizing";
  case CommandRegistrationState::synchronized:
    return "synchronized";
  case CommandRegistrationState::failed:
    return "failed";
  }
  return "failed";
}

} // namespace sanguinius
