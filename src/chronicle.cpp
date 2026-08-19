#include "sanguinius/chronicle.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace sanguinius {
namespace {

constexpr auto preview_lifetime = std::chrono::minutes{15};
constexpr auto notice_lifetime = std::chrono::hours{24 * 7};

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::optional<std::string>
field(const IncomingInteraction &interaction, const std::string_view name) {
  const auto found = std::find_if(
      interaction.modal_fields.begin(), interaction.modal_fields.end(),
      [name](const auto &item) { return item.first == name; });
  if (found == interaction.modal_fields.end()) {
    return std::nullopt;
  }
  return found->second;
}

[[nodiscard]] bool
exact_modal_fields(const IncomingInteraction &interaction,
                   const std::initializer_list<std::string_view> expected) {
  if (interaction.modal_fields.size() != expected.size())
    return false;
  return std::all_of(expected.begin(), expected.end(),
                     [&interaction](const std::string_view name) {
                       return std::count_if(interaction.modal_fields.begin(),
                                            interaction.modal_fields.end(),
                                            [name](const auto &item) {
                                              return item.first == name;
                                            }) == 1;
                     });
}

[[nodiscard]] std::optional<std::string>
string_option(const IncomingInteraction &interaction,
              const std::string_view name) {
  const auto found = std::find_if(
      interaction.command_options.begin(), interaction.command_options.end(),
      [name](const InteractionOption &option) { return option.name == name; });
  if (found == interaction.command_options.end()) {
    return std::nullopt;
  }
  if (const auto *value = std::get_if<std::string>(&found->value)) {
    return *value;
  }
  return std::nullopt;
}

[[nodiscard]] std::string bounded_utf8(const std::string_view value,
                                       const std::size_t maximum) {
  if (value.size() <= maximum)
    return std::string{value};
  auto end = maximum;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return std::string{value.substr(0, end)};
}

[[nodiscard]] std::string
concise_source_body(const ContextMessageSnapshot &source) {
  if (std::any_of(source.content.begin(), source.content.end(),
                  [](const char byte) {
                    return !std::isspace(static_cast<unsigned char>(byte));
                  })) {
    return bounded_utf8(source.content, maximum_chronicle_body_size);
  }
  return source.attachments.empty()
             ? "A moment was preserved in the Chronicle."
             : "An attachment was preserved by metadata.";
}

[[nodiscard]] std::string summary(std::string_view value,
                                  const std::size_t maximum = 160) {
  if (value.size() <= maximum) {
    return std::string{value};
  }
  if (maximum <= 3)
    return bounded_utf8(value, maximum);
  return bounded_utf8(value, maximum - 3) + "...";
}

[[nodiscard]] std::string owner_test_marker(const ChronicleEntry &entry) {
  return is_owner_test_entry(entry) ? "**TEST DATA — OWNER SELF-APPROVAL**\n"
                                    : std::string{};
}

[[nodiscard]] InteractionMessage
proposal_preview(const ProposalResult &result) {
  if (!result.entry.has_value()) {
    return text_message("The Chronicle proposal could not be prepared.");
  }
  const auto &entry = *result.entry;
  if (result.control_mode == ProposalControlMode::awaiting_confirmations ||
      result.control_mode == ProposalControlMode::confirmations_reissued) {
    return text_message(
        owner_test_marker(entry) + "**Chronicle proposal**\n**" + entry.title +
        "**\nReference: `" + entry.entry_id.substr(0, 8) + "`. " +
        (result.control_mode == ProposalControlMode::confirmations_reissued
             ? "Expired sealed confirmation notices were reissued."
             : "Required sealed confirmations are still pending."));
  }
  if (!result.actions.has_value()) {
    return text_message("The Chronicle proposal could not be prepared.");
  }
  if (result.control_mode == ProposalControlMode::owner_stale_resolution) {
    return InteractionMessage{
        .content = owner_test_marker(entry) +
                   "**Stale Chronicle proposal**\n**" + entry.title + "**\n" +
                   entry.body +
                   "\nThe required reviewer did not answer within seven days. "
                   "Resolve this shared proposal as owner.",
        .embed = EmbedPayload{.color = 0x8B0000U,
                              .title = "Captured source provenance",
                              .description =
                                  render_chronicle_provenance(entry, 4'096)},
        .buttons = {ButtonPayload{.custom_id = make_chronicle_component(
                                      chronicle_component_prefix,
                                      result.actions->submit_token_id),
                                  .label = "Approve"},
                    ButtonPayload{.custom_id = make_chronicle_component(
                                      chronicle_component_prefix,
                                      result.actions->retract_token_id),
                                  .label = "Decline"}},
        .allowed_user_mentions = {},
    };
  }
  return InteractionMessage{
      .content = owner_test_marker(entry) + "**Chronicle proposal**\n**" +
                 entry.title + "**\n" + entry.body + "\nVisibility: `" +
                 chronicle_visibility_name(entry.visibility) + "`",
      .embed = EmbedPayload{.color = 0x8B0000U,
                            .title = "Captured source provenance",
                            .description =
                                render_chronicle_provenance(entry, 4'096)},
      .buttons = {ButtonPayload{.custom_id = make_chronicle_component(
                                    chronicle_modal_prefix,
                                    result.actions->edit_token_id),
                                .label = "Edit"},
                  ButtonPayload{.custom_id = make_chronicle_component(
                                    chronicle_component_prefix,
                                    result.actions->submit_token_id),
                                .label = "Submit"},
                  ButtonPayload{.custom_id = make_chronicle_component(
                                    chronicle_component_prefix,
                                    result.actions->retract_token_id),
                                .label = "Retract"}},
      .allowed_user_mentions = {},
  };
}

[[nodiscard]] std::string entry_line(const ChronicleEntry &entry) {
  const auto marker =
      is_owner_test_entry(entry) ? "**[TEST DATA]** " : std::string{};
  return marker + "`" + entry.entry_id.substr(0, 8) + "` **" + entry.title +
         "** — " + summary(entry.body);
}

[[nodiscard]] std::string memory_line(const ExplicitMemory &memory) {
  return "`" + memory.memory_id.substr(0, 8) + "` " + summary(memory.text);
}

[[nodiscard]] std::int64_t expiry_from_name(const std::string_view value,
                                            const std::int64_t now_ms) {
  using namespace std::chrono;
  if (value == "never") {
    return 0;
  }
  if (value == "30d") {
    return now_ms + duration_cast<milliseconds>(hours{24 * 30}).count();
  }
  if (value == "90d") {
    return now_ms + duration_cast<milliseconds>(hours{24 * 90}).count();
  }
  if (value == "1y") {
    return now_ms + duration_cast<milliseconds>(hours{24 * 365}).count();
  }
  throw std::invalid_argument{"Invalid Chronicle expiry."};
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::size_t continuation = 0;
    std::uint32_t codepoint = 0;
    if (first <= 0x7FU) {
      codepoint = first;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      continuation = 1;
      codepoint = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      continuation = 2;
      codepoint = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      continuation = 3;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (index + continuation >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((continuation == 2 && codepoint < 0x800U) ||
        (continuation == 3 && codepoint < 0x10000U) || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      return false;
    }
    if ((codepoint < 0x20U && codepoint != 0x09U && codepoint != 0x0AU) ||
        (codepoint >= 0x7FU && codepoint <= 0x9FU)) {
      return false;
    }
    index += continuation + 1;
  }
  return true;
}

} // namespace

const char *chronicle_entry_type_name(const ChronicleEntryType type) noexcept {
  switch (type) {
  case ChronicleEntryType::quote:
    return "quote";
  case ChronicleEntryType::deed:
    return "deed";
  case ChronicleEntryType::prediction:
    return "prediction";
  case ChronicleEntryType::incident:
    return "incident";
  case ChronicleEntryType::custom:
    return "custom";
  case ChronicleEntryType::session_summary:
    return "session_summary";
  case ChronicleEntryType::title_award:
    return "title_award";
  }
  return "custom";
}

const char *
chronicle_visibility_name(const ChronicleVisibility value) noexcept {
  return value == ChronicleVisibility::shared ? "shared" : "participant_only";
}

const char *
chronicle_entry_status_name(const ChronicleEntryStatus value) noexcept {
  switch (value) {
  case ChronicleEntryStatus::proposed:
    return "proposed";
  case ChronicleEntryStatus::canon:
    return "canon";
  case ChronicleEntryStatus::retracted:
    return "retracted";
  }
  return "retracted";
}

const char *memory_visibility_name(const MemoryVisibility value) noexcept {
  return value == MemoryVisibility::shared ? "shared" : "self_only";
}

const char *memory_sensitivity_name(const MemorySensitivity value) noexcept {
  switch (value) {
  case MemorySensitivity::ordinary:
    return "ordinary";
  case MemorySensitivity::personal:
    return "personal";
  case MemorySensitivity::sensitive:
    return "sensitive";
  }
  return "sensitive";
}

const char *memory_status_name(const MemoryStatus value) noexcept {
  switch (value) {
  case MemoryStatus::confirmed:
    return "confirmed";
  case MemoryStatus::retracted:
    return "retracted";
  case MemoryStatus::expired:
    return "expired";
  }
  return "expired";
}

std::optional<ChronicleEntryTransition>
transition_chronicle_entry(const ChronicleEntryStatus status,
                           const bool submitted,
                           const ChronicleEntryAction action,
                           const bool approvals_remaining) noexcept {
  if (status == ChronicleEntryStatus::retracted)
    return std::nullopt;
  if (status == ChronicleEntryStatus::canon) {
    if (action != ChronicleEntryAction::retract)
      return std::nullopt;
    return ChronicleEntryTransition{.status = ChronicleEntryStatus::retracted,
                                    .submitted = true,
                                    .changed = true};
  }
  switch (action) {
  case ChronicleEntryAction::edit:
    if (submitted)
      return std::nullopt;
    return ChronicleEntryTransition{
        .status = status, .submitted = false, .changed = true};
  case ChronicleEntryAction::submit:
    if (submitted)
      return std::nullopt;
    return ChronicleEntryTransition{
        .status = status, .submitted = true, .changed = true};
  case ChronicleEntryAction::approval_completed:
    if (!submitted)
      return std::nullopt;
    return ChronicleEntryTransition{
        .status = approvals_remaining ? ChronicleEntryStatus::proposed
                                      : ChronicleEntryStatus::canon,
        .submitted = true,
        .changed = true};
  case ChronicleEntryAction::decline:
    if (!submitted)
      return std::nullopt;
    return ChronicleEntryTransition{.status = ChronicleEntryStatus::retracted,
                                    .submitted = true,
                                    .changed = true};
  case ChronicleEntryAction::retract:
    return ChronicleEntryTransition{.status = ChronicleEntryStatus::retracted,
                                    .submitted = submitted,
                                    .changed = true};
  }
  return std::nullopt;
}

std::optional<MemoryStatus>
transition_memory(const MemoryStatus status,
                  const MemoryAction action) noexcept {
  if (status != MemoryStatus::confirmed)
    return std::nullopt;
  return action == MemoryAction::retract ? MemoryStatus::retracted
                                         : MemoryStatus::expired;
}

std::optional<ChronicleEntryType>
parse_chronicle_entry_type(const std::string_view value) noexcept {
  if (value == "quote")
    return ChronicleEntryType::quote;
  if (value == "deed")
    return ChronicleEntryType::deed;
  if (value == "prediction")
    return ChronicleEntryType::prediction;
  if (value == "incident")
    return ChronicleEntryType::incident;
  if (value == "custom")
    return ChronicleEntryType::custom;
  if (value == "session_summary")
    return ChronicleEntryType::session_summary;
  if (value == "title_award")
    return ChronicleEntryType::title_award;
  return std::nullopt;
}

std::optional<ChronicleVisibility>
parse_chronicle_visibility(const std::string_view value) noexcept {
  if (value == "shared")
    return ChronicleVisibility::shared;
  if (value == "participant_only")
    return ChronicleVisibility::participant_only;
  return std::nullopt;
}

std::optional<MemoryVisibility>
parse_memory_visibility(const std::string_view value) noexcept {
  if (value == "shared")
    return MemoryVisibility::shared;
  if (value == "self_only")
    return MemoryVisibility::self_only;
  return std::nullopt;
}

std::optional<MemorySensitivity>
parse_memory_sensitivity(const std::string_view value) noexcept {
  if (value == "ordinary")
    return MemorySensitivity::ordinary;
  if (value == "personal")
    return MemorySensitivity::personal;
  if (value == "sensitive")
    return MemorySensitivity::sensitive;
  return std::nullopt;
}

bool valid_chronicle_text(const std::string_view value,
                          const std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum && valid_utf8(value) &&
         std::any_of(value.begin(), value.end(), [](const char byte) {
           return !std::isspace(static_cast<unsigned char>(byte));
         });
}

bool valid_chronicle_snapshot_text(const std::string_view value,
                                   const std::size_t maximum) noexcept {
  return value.size() <= maximum && valid_utf8(value);
}

std::vector<std::string> parse_chronicle_tags(const std::string_view value) {
  if (std::all_of(value.begin(), value.end(), [](const char character) {
        return std::isspace(static_cast<unsigned char>(character));
      })) {
    return {};
  }
  std::vector<std::string> tags;
  std::unordered_set<std::string> seen;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find(',', begin);
    auto tag = std::string{value.substr(begin, end == std::string_view::npos
                                                   ? value.size() - begin
                                                   : end - begin)};
    while (!tag.empty() &&
           std::isspace(static_cast<unsigned char>(tag.front()))) {
      tag.erase(tag.begin());
    }
    while (!tag.empty() &&
           std::isspace(static_cast<unsigned char>(tag.back()))) {
      tag.pop_back();
    }
    std::transform(tag.begin(), tag.end(), tag.begin(),
                   [](const char character) {
                     return static_cast<char>(
                         std::tolower(static_cast<unsigned char>(character)));
                   });
    const auto valid =
        !tag.empty() && tag.size() <= 32 &&
        std::all_of(tag.begin(), tag.end(), [](const char character) {
          return std::isalnum(static_cast<unsigned char>(character)) ||
                 character == '_' || character == '-';
        });
    if (!valid || (!seen.insert(tag).second)) {
      throw std::invalid_argument{
          "Chronicle tags must be unique lowercase words."};
    }
    tags.push_back(std::move(tag));
    if (tags.size() > maximum_chronicle_tags) {
      throw std::invalid_argument{"Too many Chronicle tags."};
    }
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return tags;
}

std::optional<std::string>
parse_chronicle_component(const std::string_view custom_id,
                          const std::string_view prefix) {
  if (!custom_id.starts_with(prefix) ||
      custom_id.size() != prefix.size() + 36) {
    return std::nullopt;
  }
  std::string token{custom_id.substr(prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::string make_chronicle_component(const std::string_view prefix,
                                     const std::string_view token_id) {
  if (prefix != chronicle_component_prefix &&
      prefix != chronicle_modal_prefix &&
      prefix != memory_draft_component_prefix) {
    throw std::invalid_argument{"Unknown Chronicle component family."};
  }
  const std::string token{token_id};
  if (!valid_uuid_v4(token)) {
    throw std::invalid_argument{"Chronicle component token must be a UUIDv4."};
  }
  return std::string{prefix} + token;
}

VolatileChronicleActions::VolatileChronicleActions(
    const std::size_t capacity, NowProvider now_provider,
    const std::chrono::milliseconds cleanup_interval)
    : capacity_{capacity}, now_provider_{std::move(now_provider)},
      cleanup_interval_{cleanup_interval} {
  if (capacity_ == 0)
    throw std::invalid_argument{"Draft capacity must be positive."};
  if (cleanup_interval_ <= std::chrono::milliseconds::zero())
    throw std::invalid_argument{"Draft cleanup interval must be positive."};
  if (now_provider_) {
    cleanup_thread_ = std::jthread{
        [this](const std::stop_token stop_token) { cleanup_loop(stop_token); }};
  }
}

VolatileChronicleActions::~VolatileChronicleActions() {
  cleanup_thread_.request_stop();
  cleanup_condition_.notify_all();
}

void VolatileChronicleActions::purge_expired(const std::int64_t now_ms) {
  std::lock_guard lock{mutex_};
  purge_expired_locked(now_ms);
}

void VolatileChronicleActions::purge_expired_locked(const std::int64_t now_ms) {
  std::erase_if(actions_, [now_ms](const auto &item) {
    return item.second.state != RecordState::in_progress &&
           item.second.action.expires_at_ms <= now_ms;
  });
}

void VolatileChronicleActions::cleanup_loop(
    const std::stop_token stop_token) noexcept {
  while (!stop_token.stop_requested()) {
    std::unique_lock wait_lock{cleanup_mutex_};
    cleanup_condition_.wait_for(wait_lock, cleanup_interval_, [&stop_token] {
      return stop_token.stop_requested();
    });
    if (stop_token.stop_requested())
      return;
    wait_lock.unlock();
    try {
      purge_expired(now_provider_());
    } catch (...) {
      // Cleanup is best-effort and must never terminate the bot. The next
      // interval retries with the injected clock.
    }
  }
}

void VolatileChronicleActions::put(std::string token_id,
                                   VolatileAction action) {
  std::vector<std::pair<std::string, VolatileAction>> group;
  group.emplace_back(std::move(token_id), std::move(action));
  put_group(std::move(group));
}

void VolatileChronicleActions::put_group(
    std::vector<std::pair<std::string, VolatileAction>> actions) {
  if (actions.empty() || actions.size() > capacity_) {
    throw std::invalid_argument{"Invalid volatile Chronicle action group."};
  }
  const auto group_id = actions.front().second.group_id;
  std::unordered_set<std::string> token_ids;
  for (const auto &[token_id, action] : actions) {
    if (!valid_uuid_v4(token_id) || !token_ids.insert(token_id).second ||
        group_id.empty() || action.group_id != group_id ||
        !action.guild_id.is_set() || !action.channel_id.is_set() ||
        !action.expected_user_id.is_set() || action.expires_at_ms < 0) {
      throw std::invalid_argument{"Invalid volatile Chronicle action group."};
    }
  }
  std::lock_guard lock{mutex_};
  if (std::any_of(actions.begin(), actions.end(), [this](const auto &item) {
        return actions_.contains(item.first);
      })) {
    throw std::invalid_argument{"Duplicate volatile Chronicle token."};
  }
  while (actions_.size() + actions.size() > capacity_) {
    auto oldest = actions_.end();
    for (auto candidate = actions_.begin(); candidate != actions_.end();
         ++candidate) {
      const auto &candidate_group = candidate->second.action.group_id;
      const bool in_progress =
          std::any_of(actions_.begin(), actions_.end(),
                      [&candidate_group](const auto &item) {
                        return item.second.action.group_id == candidate_group &&
                               item.second.state == RecordState::in_progress;
                      });
      if (candidate_group == group_id || in_progress) {
        continue;
      }
      if (oldest == actions_.end() ||
          candidate->second.action.expires_at_ms <
              oldest->second.action.expires_at_ms ||
          (candidate->second.action.expires_at_ms ==
               oldest->second.action.expires_at_ms &&
           candidate->first < oldest->first)) {
        oldest = candidate;
      }
    }
    if (oldest == actions_.end()) {
      throw std::overflow_error{
          "Volatile Chronicle action group exceeds capacity."};
    }
    const auto evicted_group = oldest->second.action.group_id;
    std::erase_if(actions_, [&evicted_group](const auto &item) {
      return item.second.action.group_id == evicted_group;
    });
  }
  for (auto &[token_id, action] : actions) {
    actions_.emplace(std::move(token_id), Record{.action = std::move(action)});
  }
}

VolatileActionClaim
VolatileChronicleActions::claim(const std::string_view token_id,
                                const IncomingInteraction &interaction,
                                const std::int64_t now_ms) {
  std::lock_guard lock{mutex_};
  const auto found = actions_.find(std::string{token_id});
  if (found == actions_.end() ||
      found->second.action.guild_id != interaction.guild_id ||
      found->second.action.channel_id != interaction.channel_id ||
      found->second.action.expected_user_id != interaction.user_id) {
    purge_expired_locked(now_ms);
    return {};
  }
  if (found->second.state != RecordState::in_progress &&
      found->second.action.expires_at_ms <= now_ms) {
    actions_.erase(found);
    purge_expired_locked(now_ms);
    return {.status = VolatileClaimStatus::expired};
  }
  purge_expired_locked(now_ms);
  switch (found->second.state) {
  case RecordState::active:
    if (std::any_of(actions_.begin(), actions_.end(),
                    [&found](const auto &item) {
                      return item.first != found->first &&
                             item.second.action.group_id ==
                                 found->second.action.group_id &&
                             item.second.state == RecordState::in_progress;
                    })) {
      return {.status = VolatileClaimStatus::busy};
    }
    found->second.state = RecordState::in_progress;
    return {.status = VolatileClaimStatus::claimed,
            .action = found->second.action};
  case RecordState::in_progress:
    return {.status = VolatileClaimStatus::busy};
  case RecordState::completed:
    return {.status = VolatileClaimStatus::completed,
            .result = found->second.result};
  }
  return {};
}

void VolatileChronicleActions::finish(const std::string_view token_id,
                                      ChronicleMutationResult result) {
  std::lock_guard lock{mutex_};
  const auto found = actions_.find(std::string{token_id});
  if (found == actions_.end() ||
      found->second.state != RecordState::in_progress) {
    throw std::logic_error{"Volatile Chronicle action is not in progress."};
  }
  const auto group_id = found->second.action.group_id;
  std::erase_if(actions_, [&group_id, token_id](const auto &item) {
    return item.first != token_id && item.second.action.group_id == group_id;
  });
  // A completed record retains only the scope needed for duplicate-delivery
  // replay. Private draft prose and management details must not live until the
  // token's original expiry after confirm or cancel.
  found->second.action.memory.reset();
  found->second.action.item.reset();
  if (result.entry) {
    ChronicleEntry replay_entry;
    replay_entry.entry_id = result.entry->entry_id;
    replay_entry.status = result.entry->status;
    replay_entry.revision = result.entry->revision;
    result.entry = std::move(replay_entry);
  }
  found->second.state = RecordState::completed;
  found->second.result = std::move(result);
}

void VolatileChronicleActions::release(const std::string_view token_id) {
  std::lock_guard lock{mutex_};
  const auto found = actions_.find(std::string{token_id});
  if (found != actions_.end() &&
      found->second.state == RecordState::in_progress) {
    found->second.state = RecordState::active;
  }
}

void VolatileChronicleActions::cancel_group(const std::string_view group_id) {
  std::lock_guard lock{mutex_};
  std::erase_if(actions_, [group_id](const auto &item) {
    return item.second.action.group_id == group_id &&
           item.second.state != RecordState::in_progress;
  });
}

std::size_t VolatileChronicleActions::size() const {
  std::lock_guard lock{mutex_};
  return actions_.size();
}

ChronicleService::ChronicleService(
    ChronicleRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, ServerScopeConfiguration scope,
    ControlConfiguration controls, std::function<void()> outbox_wakeup,
    std::function<void()> scheduler_wakeup, const std::size_t draft_capacity,
    std::function<void()> canon_observer)
    : repository_{repository}, clock_{clock}, ids_{ids},
      scope_{std::move(scope)}, controls_{controls},
      outbox_wakeup_{std::move(outbox_wakeup)},
      scheduler_wakeup_{std::move(scheduler_wakeup)},
      canon_observer_{std::move(canon_observer)},
      volatile_actions_{draft_capacity, [clock_pointer = &clock] {
                          return unix_milliseconds(*clock_pointer);
                        }} {}

ProposalResult
ChronicleService::canonize_message(const IncomingInteraction &interaction) {
  if (!interaction.context_message.has_value()) {
    return {.code = ChronicleResultCode::invalid_state};
  }
  const auto now_ms = unix_milliseconds(clock_);
  const auto &source = *interaction.context_message;
  if (source.reference.guild_id != interaction.guild_id ||
      source.reference.channel_id != interaction.channel_id) {
    return {.code = ChronicleResultCode::unauthorized,
            .entry = std::nullopt,
            .actions = std::nullopt};
  }
  const auto actions =
      ProposalActionIds{ids_.next_id(), ids_.next_id(), ids_.next_id()};
  std::vector<ApprovalRenewalDispatch> renewal_dispatches;
  constexpr std::size_t renewal_count = maximum_chronicle_mentions + 2;
  renewal_dispatches.reserve(renewal_count);
  for (std::size_t index = 0; index < renewal_count; ++index) {
    renewal_dispatches.push_back(ApprovalRenewalDispatch{
        .notice_id = ids_.next_id(),
        .notice_open_token_id = ids_.next_id(),
        .approve_token_id = ids_.next_id(),
        .decline_token_id = ids_.next_id(),
        .notice_event_id = ids_.next_id(),
        .notice_outbox_id = ids_.next_id(),
    });
  }
  auto result = repository_.create_or_get_proposal(CreateProposalRequest{
      .entry_id = ids_.next_id(),
      .event_id = ids_.next_id(),
      .actions = actions,
      .source = source,
      .proposer_user_id = interaction.user_id,
      .owner_user_id = scope_.owner_user_id,
      .title = "A moment worth remembering",
      .body = concise_source_body(source),
      .type = ChronicleEntryType::quote,
      .visibility = ChronicleVisibility::shared,
      .owner_test =
          controls_.test_mode && interaction.user_id == scope_.owner_user_id,
      .correlation_id = interaction.correlation_id,
      .idempotency_key =
          "chronicle:proposal:" + source.reference.message_id.str(),
      .now_ms = now_ms,
      .action_expires_at_ms =
          now_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
                       preview_lifetime)
                       .count(),
      .notice_expires_at_ms =
          now_ms +
          std::chrono::duration_cast<std::chrono::milliseconds>(notice_lifetime)
              .count(),
      .renewal_dispatches = std::move(renewal_dispatches),
  });
  if (result.wake_outbox && outbox_wakeup_)
    outbox_wakeup_();
  return result;
}

ChronicleMutationResult
ChronicleService::edit_proposal(const IncomingInteraction &interaction) {
  if (!exact_modal_fields(interaction,
                          {"title", "body", "type", "visibility", "tags"})) {
    return {.code = ChronicleResultCode::invalid_state};
  }
  const auto token =
      parse_chronicle_component(interaction.custom_id, chronicle_modal_prefix);
  const auto title = field(interaction, "title");
  const auto body = field(interaction, "body");
  const auto type = field(interaction, "type");
  const auto visibility = field(interaction, "visibility");
  const auto tags = field(interaction, "tags");
  if (!token || !title || !body || !type || !visibility || !tags ||
      !valid_chronicle_text(*title, maximum_chronicle_title_size) ||
      !valid_chronicle_text(*body, maximum_chronicle_body_size)) {
    return {.code = ChronicleResultCode::invalid_token};
  }
  const auto parsed_type = parse_chronicle_entry_type(*type);
  const auto parsed_visibility = parse_chronicle_visibility(*visibility);
  if (!parsed_type || !parsed_visibility) {
    return {.code = ChronicleResultCode::invalid_state};
  }
  try {
    return repository_.edit_proposal(EditProposalRequest{
        .token_id = *token,
        .guild_id = interaction.guild_id,
        .channel_id = interaction.channel_id,
        .actor_user_id = interaction.user_id,
        .title = *title,
        .body = *body,
        .type = *parsed_type,
        .visibility = *parsed_visibility,
        .tags = parse_chronicle_tags(*tags),
        .event_id = ids_.next_id(),
        .correlation_id = interaction.correlation_id,
        .interaction_idempotency_key =
            "chronicle:edit:" + interaction.interaction_id.str(),
        .now_ms = unix_milliseconds(clock_),
    });
  } catch (const std::invalid_argument &) {
    return {.code = ChronicleResultCode::invalid_state};
  }
}

ChronicleMutationResult
ChronicleService::submit_proposal(const IncomingInteraction &interaction) {
  const auto token = parse_chronicle_component(interaction.custom_id,
                                               chronicle_component_prefix);
  if (!token)
    return {.code = ChronicleResultCode::invalid_token};
  const auto now_ms = unix_milliseconds(clock_);
  std::vector<SubmitProposalRequest::ReviewerDispatch> dispatches;
  const auto dispatch_count =
      interaction.context_message.has_value()
          ? std::min<std::size_t>(
                12, interaction.context_message->mentioned_users.size() + 2)
          : 12;
  dispatches.reserve(dispatch_count);
  for (std::size_t index = 0; index < dispatch_count; ++index) {
    dispatches.push_back(SubmitProposalRequest::ReviewerDispatch{
        .approval_id = ids_.next_id(),
        .notice_id = ids_.next_id(),
        .notice_open_token_id = ids_.next_id(),
        .approve_token_id = ids_.next_id(),
        .decline_token_id = ids_.next_id(),
        .notice_event_id = ids_.next_id(),
        .notice_outbox_id = ids_.next_id(),
    });
  }
  auto result = repository_.submit_proposal(SubmitProposalRequest{
      .token_id = *token,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .actor_user_id = interaction.user_id,
      .owner_user_id = scope_.owner_user_id,
      .proposer_approval_id = ids_.next_id(),
      .submit_event_id = ids_.next_id(),
      .immediate_canon_event_id = ids_.next_id(),
      .reviewer_dispatches = std::move(dispatches),
      .correlation_id = interaction.correlation_id,
      .interaction_idempotency_key =
          "chronicle:submit:" + interaction.interaction_id.str(),
      .now_ms = now_ms,
      .notice_expires_at_ms =
          now_ms +
          std::chrono::duration_cast<std::chrono::milliseconds>(notice_lifetime)
              .count(),
  });
  if (result.wake_outbox && outbox_wakeup_)
    outbox_wakeup_();
  if (result.became_canon && canon_observer_) {
    try {
      canon_observer_();
    } catch (...) {
    }
  }
  return result;
}

ChronicleMutationResult
ChronicleService::apply_component(const IncomingInteraction &interaction) {
  if (const auto token = parse_chronicle_component(
          interaction.custom_id, chronicle_component_prefix)) {
    auto submitted = submit_proposal(interaction);
    if (submitted.code != ChronicleResultCode::invalid_token) {
      return submitted;
    }
    auto result = repository_.apply_approval(ApplyApprovalRequest{
        .token_id = *token,
        .guild_id = interaction.guild_id,
        .channel_id = interaction.channel_id,
        .actor_user_id = interaction.user_id,
        .owner_user_id = scope_.owner_user_id,
        .action_event_id = ids_.next_id(),
        .canon_event_id = ids_.next_id(),
        .public_outbox_id = ids_.next_id(),
        .correlation_id = interaction.correlation_id,
        .interaction_idempotency_key =
            "chronicle:action:" + interaction.interaction_id.str(),
        .now_ms = unix_milliseconds(clock_),
    });
    if (result.wake_outbox && outbox_wakeup_)
      outbox_wakeup_();
    if (result.became_canon && canon_observer_) {
      try {
        canon_observer_();
      } catch (...) {
      }
    }
    return result;
  }

  const auto token = parse_chronicle_component(interaction.custom_id,
                                               memory_draft_component_prefix);
  if (!token)
    return {.code = ChronicleResultCode::invalid_token};
  const auto now_ms = unix_milliseconds(clock_);
  auto claim = volatile_actions_.claim(*token, interaction, now_ms);
  if (claim.status == VolatileClaimStatus::expired) {
    return {.code = ChronicleResultCode::expired};
  }
  if (claim.status == VolatileClaimStatus::completed) {
    return claim.result.value_or(
        ChronicleMutationResult{.code = ChronicleResultCode::invalid_state});
  }
  if (claim.status == VolatileClaimStatus::busy) {
    return {.code = ChronicleResultCode::invalid_state};
  }
  if (claim.status != VolatileClaimStatus::claimed || !claim.action) {
    return {.code = ChronicleResultCode::invalid_token};
  }

  ChronicleMutationResult result;
  try {
    const auto &action = *claim.action;
    if (action.kind == VolatileActionKind::cancel_memory) {
      result = {.code = ChronicleResultCode::updated, .draft_cancelled = true};
    } else if (action.kind == VolatileActionKind::confirm_memory &&
               action.memory) {
      result = repository_.confirm_memory(ConfirmMemoryRequest{
          .memory_id = ids_.next_id(),
          .event_id = ids_.next_id(),
          .expiry_job_id = action.memory->expires_at_ms
                               ? std::optional{ids_.next_id()}
                               : std::nullopt,
          .draft = *action.memory,
          .correlation_id = interaction.correlation_id,
          .interaction_idempotency_key = "chronicle:memory-confirm:" + *token,
          .now_ms = now_ms,
      });
    } else if (action.item) {
      const auto request = RetractItemRequest{
          .entity_id = action.item->entity_id,
          .expected_revision = action.item->revision,
          .guild_id = interaction.guild_id,
          .channel_id = interaction.channel_id,
          .actor_user_id = interaction.user_id,
          .owner_user_id = scope_.owner_user_id,
          .event_id = ids_.next_id(),
          .public_outbox_id = ids_.next_id(),
          .correlation_id = interaction.correlation_id,
          .interaction_idempotency_key = "chronicle:retract:" + *token,
          .now_ms = now_ms,
      };
      result = action.kind == VolatileActionKind::retract_memory
                   ? repository_.retract_memory(request)
                   : repository_.retract_entry(request);
    } else {
      result = {.code = ChronicleResultCode::invalid_state};
    }
  } catch (...) {
    volatile_actions_.release(*token);
    throw;
  }
  volatile_actions_.finish(*token, result);
  if (result.wake_scheduler && scheduler_wakeup_)
    scheduler_wakeup_();
  if (result.wake_outbox && outbox_wakeup_)
    outbox_wakeup_();
  return result;
}

ChronicleMutationResult
ChronicleService::complete_expiry(const ClaimedScheduledJob &job) {
  return repository_.expire_memory(job, ids_.next_id(),
                                   unix_milliseconds(clock_));
}

InteractionMessage
ChronicleService::begin_memory_preview(const IncomingInteraction &interaction) {
  const bool base_fields =
      exact_modal_fields(interaction,
                         {"text", "visibility", "sensitivity", "expiry"});
  const bool tagged_fields = exact_modal_fields(
      interaction, {"text", "tags", "visibility", "sensitivity", "expiry"});
  if (!base_fields && !tagged_fields) {
    return text_message("The memory preview fields are invalid.");
  }
  const auto text = field(interaction, "text");
  const auto visibility_text =
      field(interaction, "visibility").value_or("shared");
  const auto sensitivity_text =
      field(interaction, "sensitivity").value_or("ordinary");
  const auto expiry_text = field(interaction, "expiry").value_or("never");
  const auto visibility = parse_memory_visibility(visibility_text);
  const auto sensitivity = parse_memory_sensitivity(sensitivity_text);
  if (!text || !valid_chronicle_text(*text, maximum_memory_text_size) ||
      !visibility || !sensitivity) {
    return text_message("The memory preview is invalid or exceeds its bounds.");
  }
  std::vector<std::string> tags;
  try {
    tags = parse_chronicle_tags(field(interaction, "tags").value_or(""));
  } catch (const std::invalid_argument &) {
    return text_message(
        "Use at most five unique topic tags made of lowercase letters, "
        "numbers, `_`, or `-`.");
  }
  const auto now_ms = unix_milliseconds(clock_);
  std::int64_t expiry{};
  try {
    expiry = expiry_from_name(expiry_text, now_ms);
  } catch (const std::invalid_argument &) {
    return text_message("Choose an expiry of never, 30d, 90d, or 1y.");
  }
  auto effective_visibility = *visibility;
  const bool coerced = *sensitivity != MemorySensitivity::ordinary &&
                       effective_visibility != MemoryVisibility::self_only;
  if (coerced)
    effective_visibility = MemoryVisibility::self_only;
  const auto group = ids_.next_id();
  const auto confirm = ids_.next_id();
  const auto cancel = ids_.next_id();
  const auto expires_at =
      now_ms +
      std::chrono::duration_cast<std::chrono::milliseconds>(preview_lifetime)
          .count();
  const MemoryDraft draft{.text = *text,
                          .tags = tags,
                          .visibility = effective_visibility,
                          .sensitivity = *sensitivity,
                          .expires_at_ms = expiry == 0 ? std::nullopt
                                                       : std::optional{expiry},
                          .guild_id = interaction.guild_id,
                          .channel_id = interaction.channel_id,
                          .user_id = interaction.user_id};
  std::vector<std::pair<std::string, VolatileAction>> actions;
  actions.emplace_back(confirm, VolatileAction{
                                    .kind = VolatileActionKind::confirm_memory,
                                    .guild_id = interaction.guild_id,
                                    .channel_id = interaction.channel_id,
                                    .expected_user_id = interaction.user_id,
                                    .expires_at_ms = expires_at,
                                    .memory = draft,
                                    .group_id = group,
                                });
  actions.emplace_back(cancel, VolatileAction{
                                   .kind = VolatileActionKind::cancel_memory,
                                   .guild_id = interaction.guild_id,
                                   .channel_id = interaction.channel_id,
                                   .expected_user_id = interaction.user_id,
                                   .expires_at_ms = expires_at,
                                   .memory = draft,
                                   .group_id = group,
                               });
  volatile_actions_.put_group(std::move(actions));
  std::string tag_line;
  if (!tags.empty()) {
    tag_line = "\nTopic tags: `";
    for (std::size_t index = 0; index < tags.size(); ++index) {
      if (index != 0) tag_line += "`, `";
      tag_line += tags[index];
    }
    tag_line += "`.";
  }
  return InteractionMessage{
      .content = "**Memory preview**\n" + *text + "\nVisibility: `" +
                 memory_visibility_name(effective_visibility) +
                 "`; sensitivity: `" + memory_sensitivity_name(*sensitivity) +
                 "`." + tag_line +
                 (coerced ? " Sensitive/personal memories are self-only." : ""),
      .embed = std::nullopt,
      .buttons = {ButtonPayload{.custom_id = make_chronicle_component(
                                    memory_draft_component_prefix, confirm),
                                .label = "Confirm"},
                  ButtonPayload{.custom_id = make_chronicle_component(
                                    memory_draft_component_prefix, cancel),
                                .label = "Cancel"}},
      .allowed_user_mentions = {},
  };
}

InteractionMessage
ChronicleService::recall(const IncomingInteraction &interaction) {
  const auto results = repository_.recall(
      interaction.user_id, string_option(interaction, "query").value_or(""),
      unix_milliseconds(clock_), 5);
  std::string content = "**Chronicle recall**";
  if (!results.ordered_items.empty()) {
    for (const auto &item : results.ordered_items) {
      if (const auto *entry = std::get_if<ChronicleEntry>(&item)) {
        content += "\n" + entry_line(*entry);
      } else if (const auto *memory = std::get_if<ExplicitMemory>(&item)) {
        content += "\n" + memory_line(*memory);
      }
    }
  } else {
    for (const auto &entry : results.entries)
      content += "\n" + entry_line(entry);
    for (const auto &memory : results.memories)
      content += "\n" + memory_line(memory);
  }
  if (results.entries.empty() && results.memories.empty()) {
    content += "\nNothing matching is available to you.";
  }
  return text_message(std::move(content));
}

InteractionMessage
ChronicleService::timeline(const IncomingInteraction &interaction) {
  const auto period = string_option(interaction, "period").value_or("30d");
  const auto now_ms = unix_milliseconds(clock_);
  std::optional<std::int64_t> since;
  if (period == "7d")
    since = now_ms - 7LL * 24 * 60 * 60 * 1000;
  else if (period == "30d")
    since = now_ms - 30LL * 24 * 60 * 60 * 1000;
  else if (period != "all")
    return text_message("Choose 7d, 30d, or all.");
  const auto entries = repository_.timeline(since, now_ms, 5);
  std::string content = "**The Living Chronicle**";
  for (const auto &entry : entries)
    content += "\n" + entry_line(entry);
  if (entries.empty())
    content += "\nNo shared canon entries fall in that period.";
  return text_message(std::move(content));
}

InteractionMessage
ChronicleService::forget(const IncomingInteraction &interaction) {
  const auto reference = string_option(interaction, "reference").value_or("");
  const auto now_ms = unix_milliseconds(clock_);
  auto items =
      repository_.manageable(interaction.user_id, scope_.owner_user_id,
                             reference, now_ms, reference.empty() ? 5 : 2);
  if (items.empty())
    return text_message("No controllable Chronicle records were found.");
  if (!reference.empty()) {
    if (items.size() != 1) {
      std::string content =
          "That reference is ambiguous. Retry `/chronicle forget` with a "
          "longer reference:";
      for (const auto &item : items) {
        content += "\n`" + item.entity_id + "` " + summary(item.summary);
      }
      return text_message(std::move(content));
    }
    const auto &item = items.front();
    const auto request = RetractItemRequest{
        .entity_id = item.entity_id,
        .expected_revision = item.revision,
        .guild_id = interaction.guild_id,
        .channel_id = interaction.channel_id,
        .actor_user_id = interaction.user_id,
        .owner_user_id = scope_.owner_user_id,
        .event_id = ids_.next_id(),
        .public_outbox_id = ids_.next_id(),
        .correlation_id = interaction.correlation_id,
        .interaction_idempotency_key =
            "chronicle:retract-command:" + interaction.interaction_id.str(),
        .now_ms = now_ms,
    };
    auto result = item.kind == ManageableKind::memory
                      ? repository_.retract_memory(request)
                      : repository_.retract_entry(request);
    if (result.wake_outbox && outbox_wakeup_)
      outbox_wakeup_();
    return render_chronicle_mutation(result);
  }
  std::string content = "**Choose a record to retract**";
  InteractionMessage message;
  const auto group = ids_.next_id();
  const auto expires_at =
      unix_milliseconds(clock_) +
      std::chrono::duration_cast<std::chrono::milliseconds>(preview_lifetime)
          .count();
  std::vector<std::pair<std::string, VolatileAction>> actions;
  actions.reserve(items.size());
  for (const auto &item : items) {
    const auto token = ids_.next_id();
    content +=
        "\n`" + item.entity_id.substr(0, 8) + "` " + summary(item.summary);
    actions.emplace_back(token,
                         VolatileAction{
                             .kind = item.kind == ManageableKind::memory
                                         ? VolatileActionKind::retract_memory
                                         : VolatileActionKind::retract_entry,
                             .guild_id = interaction.guild_id,
                             .channel_id = interaction.channel_id,
                             .expected_user_id = interaction.user_id,
                             .expires_at_ms = expires_at,
                             .item = item,
                             .group_id = group,
                         });
    message.buttons.push_back(ButtonPayload{
        .custom_id =
            make_chronicle_component(memory_draft_component_prefix, token),
        .label = "Retract " + item.entity_id.substr(0, 8),
    });
  }
  volatile_actions_.put_group(std::move(actions));
  message.content = std::move(content);
  return message;
}

ModalPayload ChronicleService::remember_modal() {
  return ModalPayload{
      .custom_id = "chronicle.remember:1",
      .title = "Remember in the Chronicle",
      .fields = {ModalFieldPayload{.custom_id = "text",
                                   .label = "Memory",
                                   .maximum_length = maximum_memory_text_size,
                                   .style =
                                       ModalFieldPayload::Style::paragraph},
                 ModalFieldPayload{.custom_id = "tags",
                                   .label = "Optional topic tags, comma-separated",
                                   .maximum_length = 164,
                                   .required = false},
                 ModalFieldPayload{.custom_id = "visibility",
                                   .label = "Visibility: shared or self_only",
                                   .value = "shared",
                                   .minimum_length = 6,
                                   .maximum_length = 9},
                 ModalFieldPayload{.custom_id = "sensitivity",
                                   .label = "ordinary, personal, or sensitive",
                                   .value = "ordinary",
                                   .minimum_length = 8,
                                   .maximum_length = 9},
                 ModalFieldPayload{.custom_id = "expiry",
                                   .label = "never, 30d, 90d, or 1y",
                                   .value = "never",
                                   .minimum_length = 2,
                                   .maximum_length = 5}},
  };
}

ModalPayload ChronicleService::edit_entry_modal(std::string token_id) {
  return ModalPayload{
      .custom_id = make_chronicle_component(chronicle_modal_prefix, token_id),
      .title = "Edit Chronicle proposal",
      .fields =
          {ModalFieldPayload{.custom_id = "title",
                             .label = "Heading",
                             .minimum_length = 1,
                             .maximum_length = maximum_chronicle_title_size},
           ModalFieldPayload{.custom_id = "body",
                             .label = "Chronicle entry",
                             .minimum_length = 1,
                             .maximum_length = maximum_chronicle_body_size,
                             .style = ModalFieldPayload::Style::paragraph},
           ModalFieldPayload{.custom_id = "type",
                             .label =
                                 "quote, deed, prediction, incident, custom",
                             .value = "quote",
                             .minimum_length = 4,
                             .maximum_length = 10},
           ModalFieldPayload{.custom_id = "visibility",
                             .label = "shared or participant_only",
                             .value = "shared",
                             .minimum_length = 6,
                             .maximum_length = 16},
           ModalFieldPayload{.custom_id = "tags",
                             .label = "Comma-separated tags",
                             .maximum_length = 164,
                             .required = false}},
  };
}

InteractionMessage render_chronicle_proposal(const ProposalResult &result) {
  if (result.code == ChronicleResultCode::opted_out) {
    return text_message("This message cannot be proposed because a required "
                        "participant has not opted into the Chronicle.");
  }
  if ((result.code == ChronicleResultCode::existing ||
       result.code == ChronicleResultCode::created) &&
      result.entry && result.entry->status != ChronicleEntryStatus::proposed) {
    return text_message(
        "That source message already has a permanent Chronicle record (`" +
        result.entry->entry_id.substr(0, 8) + "`).");
  }
  return proposal_preview(result);
}

InteractionMessage
render_chronicle_mutation(const ChronicleMutationResult &result) {
  switch (result.code) {
  case ChronicleResultCode::created:
    return text_message("The memory is now confirmed in the Chronicle.");
  case ChronicleResultCode::updated:
    if (result.draft_cancelled) {
      return text_message("The unconfirmed memory draft was discarded.");
    }
    if (result.became_canon) {
      return text_message("The proposal is now canon.");
    }
    if (result.entry &&
        result.entry->status == ChronicleEntryStatus::retracted) {
      return text_message("The Chronicle entry has been retracted.");
    }
    return text_message("The Chronicle was updated.");
  case ChronicleResultCode::unchanged:
    return text_message("That action was already applied.");
  case ChronicleResultCode::not_found:
    return text_message("That Chronicle record no longer exists.");
  case ChronicleResultCode::unauthorized:
    return text_message(
        "You are not permitted to change that Chronicle record.");
  case ChronicleResultCode::opted_out:
    return text_message("Your Chronicle opt-in is not enabled.");
  case ChronicleResultCode::invalid_state:
    return text_message(
        "That Chronicle action is not valid in the current state.");
  case ChronicleResultCode::stale_revision:
    return text_message("That control is stale. Open a fresh Chronicle view.");
  case ChronicleResultCode::expired:
    return text_message("That Chronicle control has expired.");
  case ChronicleResultCode::invalid_token:
    return text_message(
        "That Chronicle control is invalid or no longer available.");
  case ChronicleResultCode::existing:
    return text_message("That Chronicle record already exists.");
  }
  return text_message("The Chronicle action could not be completed.");
}

bool is_owner_test_entry(const ChronicleEntry &entry) noexcept {
  return std::find(entry.tags.begin(), entry.tags.end(), "owner-test") !=
         entry.tags.end();
}

std::string render_chronicle_provenance(const ChronicleEntry &entry,
                                        const std::size_t maximum_size) {
  if (maximum_size == 0) {
    return {};
  }

  std::string rendered =
      "Source author: `" + entry.source_author_user_id.str() + "`\n";
  if (entry.source_message_id) {
    rendered += "Source message: guild `" + entry.source_guild_id.str() +
                "`, channel `" + entry.source_channel_id.str() +
                "`, message `" + entry.source_message_id->str() + "`\n";
  } else {
    rendered += "Source: approved Chronicle system record.\n";
  }
  if (!entry.attachments.empty()) {
    rendered +=
        "Attachments (" + std::to_string(entry.attachments.size()) + "):\n";
    for (std::size_t index = 0; index < entry.attachments.size(); ++index) {
      const auto &attachment = entry.attachments[index];
      rendered += "- " + std::to_string(index + 1) + ": id `" +
                  attachment.attachment_id.str() + "`, file " +
                  summary(attachment.filename, 80) + ", type " +
                  summary(attachment.content_type.value_or("unknown"), 48) +
                  ", " + std::to_string(attachment.byte_size) + " bytes";
      if (attachment.width && attachment.height) {
        rendered += ", " + std::to_string(*attachment.width) + "x" +
                    std::to_string(*attachment.height);
      }
      if (attachment.ephemeral)
        rendered += ", ephemeral";
      if (attachment.spoiler)
        rendered += ", spoiler";
      rendered += "\n";
    }
  }
  rendered += "Captured source text:\n";
  rendered +=
      entry.source_text.empty() ? "(no message text)" : entry.source_text;
  if (entry.source_text_truncated) {
    rendered += "\n[Source text snapshot truncated at capture.]";
  }

  if (rendered.size() <= maximum_size) {
    return rendered;
  }
  constexpr std::string_view marker{"\n[Provenance view truncated.]"};
  if (maximum_size <= marker.size()) {
    return bounded_utf8(rendered, maximum_size);
  }
  return bounded_utf8(rendered, maximum_size - marker.size()) +
         std::string{marker};
}

} // namespace sanguinius
