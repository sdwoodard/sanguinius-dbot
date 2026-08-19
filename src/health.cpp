#include "sanguinius/health.hpp"

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

[[nodiscard]] std::string bounded_health_message(std::string message) {
  if (message.size() <= maximum_health_message_size) {
    return message;
  }

  constexpr std::string_view truncation_marker{"...\n"};
  message.resize(maximum_health_message_size - truncation_marker.size());
  message += truncation_marker;
  return message;
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
