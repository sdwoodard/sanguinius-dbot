#include "sanguinius/durable_work_controls.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

constexpr std::int64_t notice_lifetime_ms = 24LL * 60LL * 60LL * 1'000LL;
constexpr std::int64_t scheduled_delay_ms = 60'000;
constexpr std::size_t maximum_inspection_response_size = 1'900;
constexpr std::size_t maximum_inspection_field_size = 48;

[[nodiscard]] std::int64_t now_ms(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string
interaction_key(const IncomingInteraction &interaction,
                const std::string_view suffix) {
  return "interaction:" + interaction.interaction_id.str() + ':' +
         std::string{suffix};
}

void require_scoped_owner_interaction(const IncomingInteraction &interaction,
                                      const ServerScopeConfiguration &scope) {
  if (!interaction.interaction_id.is_set() || !interaction.user_id.is_set() ||
      interaction.user_id != scope.owner_user_id ||
      interaction.guild_id != scope.guild_id ||
      interaction.channel_id != scope.primary_channel_id) {
    throw std::invalid_argument{"Durable owner control is outside its scope."};
  }
}

[[nodiscard]] std::string bounded_field(const std::string_view value) {
  if (value.size() <= maximum_inspection_field_size) {
    return std::string{value};
  }
  return std::string{value.substr(0, maximum_inspection_field_size - 3)} +
         "...";
}

} // namespace

DurableWorkControlService::DurableWorkControlService(
    DurableWorkRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, const ServerScopeConfiguration scope,
    std::function<void()> scheduler_wakeup, std::function<void()> outbox_wakeup)
    : repository_{repository}, clock_{clock}, ids_{ids}, scope_{scope},
      scheduler_wakeup_{std::move(scheduler_wakeup)},
      outbox_wakeup_{std::move(outbox_wakeup)} {
  if (!scope_.guild_id.is_set() || !scope_.primary_channel_id.is_set() ||
      !scope_.owner_user_id.is_set() || !scheduler_wakeup_ || !outbox_wakeup_) {
    throw std::invalid_argument{"Durable owner controls are incomplete."};
  }
}

bool DurableWorkControlService::queue_test_notice(
    const IncomingInteraction &interaction) {
  require_scoped_owner_interaction(interaction, scope_);
  const auto current = now_ms(clock_);
  auto payload = test_notice_payload(interaction, current);
  auto event = control_event(interaction, "owner.test_notice_queued.v1",
                             "test-notice:event", current);
  const auto outbox_id = ids_.next_id();
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{pending_notice_outbox_kind},
      .aggregate_type = "owner_interaction",
      .aggregate_id = interaction.interaction_id.str(),
      .target_guild_id = scope_.guild_id,
      .target_channel_id = scope_.primary_channel_id,
      .target_user_id = interaction.user_id,
      .available_at_ms = current,
      .max_attempts = 5,
      .idempotency_key = interaction_key(interaction, "test-notice:outbox"),
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = current,
  };
  const bool created = repository_.enqueue_notice(event, outbox, payload);
  if (created) {
    outbox_wakeup_();
  }
  return created;
}

bool DurableWorkControlService::schedule_test_notice(
    const IncomingInteraction &interaction) {
  require_scoped_owner_interaction(interaction, scope_);
  const auto current = now_ms(clock_);
  auto payload = test_notice_payload(interaction, current + scheduled_delay_ms);
  auto event = control_event(interaction, "owner.test_notice_scheduled.v1",
                             "test-schedule-notice:event", current);
  const ScheduledJobEnqueue job{
      .job_id = ids_.next_id(),
      .job_type = std::string{owner_test_notice_job_type},
      .aggregate_type = "owner_interaction",
      .aggregate_id = interaction.interaction_id.str(),
      .due_at_ms = current + scheduled_delay_ms,
      .max_attempts = 5,
      .idempotency_key =
          interaction_key(interaction, "test-schedule-notice:job"),
      .created_at_ms = current,
  };
  const bool created = repository_.schedule_notice(event, job, payload);
  if (created) {
    scheduler_wakeup_();
  }
  return created;
}

bool DurableWorkControlService::queue_test_public_retry(
    const IncomingInteraction &interaction) {
  require_scoped_owner_interaction(interaction, scope_);
  const auto current = now_ms(clock_);
  auto event = control_event(interaction, "owner.test_public_retry_queued.v1",
                             "test-public-retry:event", current);
  const auto outbox_id = ids_.next_id();
  const OutboxEnqueue outbox{
      .outbox_id = outbox_id,
      .kind = std::string{test_public_retry_outbox_kind},
      .aggregate_type = "owner_interaction",
      .aggregate_id = interaction.interaction_id.str(),
      .target_guild_id = scope_.guild_id,
      .target_channel_id = scope_.primary_channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = current,
      .max_attempts = 5,
      .idempotency_key =
          interaction_key(interaction, "test-public-retry:outbox"),
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = current,
  };
  const PublicOutboxPayload payload{
      .request =
          PublicMessageRequest{
              .guild_id = scope_.guild_id,
              .channel_id = scope_.primary_channel_id,
              .message = text_message(
                  "A synthetic delivery retry has completed. No private "
                  "content is attached."),
          },
      .fail_before_first_send = true,
  };
  const bool created = repository_.enqueue_public(event, outbox, payload);
  if (created) {
    outbox_wakeup_();
  }
  return created;
}

std::vector<WorkInspectionEntry> DurableWorkControlService::recent() const {
  return repository_.recent(10);
}

std::vector<WorkInspectionEntry> DurableWorkControlService::dead() const {
  return repository_.dead(10);
}

NoticeOutboxPayload DurableWorkControlService::test_notice_payload(
    const IncomingInteraction &interaction, const std::int64_t created_at_ms) {
  const auto notice_id = ids_.next_id();
  return NoticeOutboxPayload{
      .notice =
          CreatePendingNoticeRequest{
              .notice_id = notice_id,
              .token_id = ids_.next_id(),
              .target_user_id = interaction.user_id,
              .guild_id = scope_.guild_id,
              .channel_id = scope_.primary_channel_id,
              .notice_type = "owner_test.notice.v1",
              .content = {"Sealed notice test",
                          "The sealed-notice test succeeded."},
              .source_aggregate_type = "owner_interaction",
              .source_aggregate_id = interaction.interaction_id.str(),
              .expires_at_ms = created_at_ms + notice_lifetime_ms,
              .notice_idempotency_key =
                  interaction_key(interaction, "test-notice:notice"),
              .token_idempotency_key =
                  interaction_key(interaction, "test-notice:token"),
              .created_at_ms = created_at_ms,
          },
      .announce_publicly = true,
  };
}

EventJournalEntry DurableWorkControlService::control_event(
    const IncomingInteraction &interaction, std::string event_type,
    std::string idempotency_suffix, const std::int64_t at_ms) {
  return EventJournalEntry{
      .event_id = ids_.next_id(),
      .event_type = std::move(event_type),
      .aggregate_type = "owner_interaction",
      .aggregate_id = interaction.interaction_id.str(),
      .actor_user_id = interaction.user_id,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .source_message_id = std::nullopt,
      .occurred_at_ms = at_ms,
      .recorded_at_ms = at_ms,
      .correlation_id = interaction.interaction_id.str(),
      .causation_id = std::nullopt,
      .idempotency_key = interaction_key(interaction, idempotency_suffix),
      .payload_json = "{}",
  };
}

std::string
render_work_inspection(const std::vector<WorkInspectionEntry> &entries,
                       const std::string_view heading) {
  std::ostringstream output;
  output << heading << '\n';
  std::size_t rendered_size = heading.size() + 1;
  if (entries.empty()) {
    output << "No matching durable work.";
    return output.str();
  }
  for (const auto &entry : entries) {
    std::ostringstream line;
    line << bounded_field(entry.category) << ' ' << bounded_field(entry.type)
         << ' ' << bounded_field(entry.state)
         << " id=" << bounded_field(entry.shortened_id)
         << " attempts=" << entry.attempts << " at_ms=" << entry.at_ms;
    if (entry.error_code.has_value()) {
      line << " error=" << bounded_field(*entry.error_code);
    }
    line << '\n';
    auto text = line.str();
    if (rendered_size + text.size() > maximum_inspection_response_size) {
      constexpr std::string_view omitted{"Additional entries omitted."};
      if (rendered_size + omitted.size() <= maximum_inspection_response_size) {
        output << omitted;
      }
      break;
    }
    rendered_size += text.size();
    output << text;
  }
  return output.str();
}

} // namespace sanguinius
