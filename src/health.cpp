#include "sanguinius/health.hpp"

#include <sstream>
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
                             PersistenceHealth persistence)
    : build_{std::move(build)}, controls_{controls}, features_{features},
      persistence_{std::move(persistence)} {}

HealthSnapshot HealthService::snapshot(const QueueSnapshot message_queue,
                                       const QueueSnapshot ai_queue,
                                       const bool scope_matched) const {
  return HealthSnapshot{
      .build = build_,
      .message_queue = message_queue,
      .ai_queue = ai_queue,
      .controls = controls_,
      .features = features_,
      .persistence = persistence_,
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

} // namespace sanguinius
