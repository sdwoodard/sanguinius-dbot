#include "sanguinius/wagers.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] std::int64_t unix_milliseconds(const Clock &clock) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock.now().time_since_epoch())
      .count();
}

[[nodiscard]] std::int64_t checked_hours_ms(const std::int64_t hours) {
  constexpr std::int64_t milliseconds_per_hour = 3'600'000;
  if (hours <= 0 ||
      hours > std::numeric_limits<std::int64_t>::max() / milliseconds_per_hour)
    throw std::invalid_argument{"Wager duration is invalid."};
  return hours * milliseconds_per_hour;
}

[[nodiscard]] bool blank(const std::string_view value) {
  return value.empty() ||
         std::ranges::all_of(value, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

template <typename Value>
[[nodiscard]] std::optional<Value>
option(const IncomingInteraction &interaction, const std::string_view name) {
  const auto found = std::ranges::find(interaction.command_options, name,
                                       &InteractionOption::name);
  if (found == interaction.command_options.end())
    return std::nullopt;
  const auto *value = std::get_if<Value>(&found->value);
  if (value == nullptr)
    throw std::invalid_argument{"Wager command option type is invalid."};
  return *value;
}

[[nodiscard]] std::optional<std::string>
modal_field(const IncomingInteraction &interaction,
            const std::string_view name) {
  const auto found =
      std::ranges::find(interaction.modal_fields, name,
                        &decltype(interaction.modal_fields)::value_type::first);
  return found == interaction.modal_fields.end()
             ? std::nullopt
             : std::optional<std::string>{found->second};
}

[[nodiscard]] const ResolvedUserSnapshot &
resolved_user(const IncomingInteraction &interaction,
              const DiscordSnowflake &user_id) {
  const auto found = std::ranges::find(interaction.resolved_users, user_id,
                                       &ResolvedUserSnapshot::user_id);
  if (found == interaction.resolved_users.end())
    throw std::invalid_argument{"The selected wager user was not resolved."};
  return *found;
}

[[nodiscard]] std::int64_t outcome_window(const std::string_view value) {
  if (value == "1h")
    return checked_hours_ms(1);
  if (value == "6h")
    return checked_hours_ms(6);
  if (value == "24h")
    return checked_hours_ms(24);
  if (value == "72h")
    return checked_hours_ms(72);
  if (value == "7d")
    return checked_hours_ms(24 * 7);
  throw std::invalid_argument{"Wager outcome window is invalid."};
}

[[nodiscard]] std::string state_sentence(const WagerRecord &wager) {
  std::ostringstream output;
  output << "`" << wager.wager_id << "` — " << wager_state_name(wager.state);
  if (wager.stake)
    output << ", " << *wager.stake << " Fate each";
  return output.str();
}

[[nodiscard]] std::string bounded_discord_content(std::string content) {
  constexpr std::size_t limit = 1'900;
  constexpr std::string_view suffix{
      "\n… Additional private evidence remains in the durable wager audit."};
  if (content.size() <= limit)
    return content;
  auto boundary = limit - suffix.size();
  while (boundary > 0 &&
         (static_cast<unsigned char>(content[boundary]) & 0xc0U) == 0x80U)
    --boundary;
  content.resize(boundary);
  content += suffix;
  return content;
}

[[nodiscard]] std::string discord_timestamp(const std::int64_t time_ms) {
  return "<t:" + std::to_string(time_ms / 1'000) +
         ":F> (<t:" + std::to_string(time_ms / 1'000) + ":R>)";
}

[[nodiscard]] InteractionMessage
render_wager(const WagerMutationResult &result) {
  switch (result.status) {
  case WagerMutationStatus::forbidden:
    return text_message("That wager action is not available to you.");
  case WagerMutationStatus::invalid_state:
    return text_message(
        "That wager is no longer in a state that permits this action.");
  case WagerMutationStatus::expired:
    return text_message("That wager control has expired.");
  case WagerMutationStatus::insufficient_funds:
    return text_message(
        "Both participants must have enough Fate before escrow can be "
        "funded. The offer remains open.");
  case WagerMutationStatus::stale:
    return text_message(
        "That wager control is stale. Open `/tarot wagers` for current "
        "controls.");
  case WagerMutationStatus::not_found:
    return text_message("No visible wager matches that reference.");
  case WagerMutationStatus::applied:
  case WagerMutationStatus::unchanged:
    break;
  }
  if (!result.wager)
    return text_message("The wager action is already recorded.");

  const auto &wager = *result.wager;
  std::ostringstream output;
  output << "**Wager " << wager.wager_id << "**";
  if (wager.is_test)
    output << " `[TEST]`";
  output << "\nStatus: " << wager_state_name(wager.state) << "\nCreator: <@"
         << wager.creator_user_id.str() << ">"
         << "\nTarget: <@" << wager.target_user_id.str() << ">";
  if (wager.proposition)
    output << "\nProposition: " << *wager.proposition;
  if (wager.stake)
    output << "\nStake: " << *wager.stake << " Fate each ("
           << (*wager.stake * 2) << " total escrow)";
  output << "\nOutcome window: " << wager.outcome_window_ms / 3'600'000
         << " hours after acceptance"
         << "\nResolution grace: " << wager.resolution_grace_ms / 3'600'000
         << " hours after the outcome deadline";
  output << "\nVisibility: "
         << (wager.visibility == WagerVisibility::sealed ? "sealed" : "public")
         << "\nResolution: "
         << (wager.resolution_policy == WagerResolutionPolicy::mutual
                 ? "mutual"
                 : "designated judge");
  if (wager.judge_user_id)
    output << " <@" << wager.judge_user_id->str() << ">";
  if (wager.offer_expires_at_ms)
    output << "\nOffer expires: "
           << discord_timestamp(*wager.offer_expires_at_ms);
  if (wager.outcome_due_at_ms)
    output << "\nOutcome due: " << discord_timestamp(*wager.outcome_due_at_ms);
  if (wager.resolution_grace_until_ms)
    output << "\nOwner escalation after: "
           << discord_timestamp(*wager.resolution_grace_until_ms);
  if (wager.winner)
    output << "\nWinner: "
           << (*wager.winner == WagerRole::creator ? "creator" : "target");
  if (wager.terminal_reason)
    output << "\nResolution reason: " << *wager.terminal_reason;
  output << "\nRevision: " << wager.revision;
  if (result.status == WagerMutationStatus::unchanged)
    output << "\nThis request was already recorded; no Fate moved again.";
  InteractionMessage message =
      text_message(bounded_discord_content(output.str()));
  for (const auto &control : result.controls) {
    message.buttons.push_back(ButtonPayload{
        .custom_id = control.custom_id,
        .label = control.action,
        .disabled = false,
        .style = control.action == "Discard" || control.action == "Decline" ||
                         control.action == "Cancel"
                     ? ButtonStyle::secondary
                     : ButtonStyle::primary,
    });
  }
  return message;
}

[[nodiscard]] InteractionMessage
render_history(const WagerHistoryResult &result, const bool disputes_only) {
  if (result.status == WagerMutationStatus::forbidden)
    return text_message("Those wager records are private.");
  if (result.status == WagerMutationStatus::expired)
    return text_message(
        "That wager history page expired. Run `/tarot wagers` again.");
  if (result.status == WagerMutationStatus::not_found)
    return text_message("That wager record or history page is unavailable.");
  if (result.wagers.empty())
    return text_message(disputes_only
                            ? "No visible wager disputes await review."
                            : "No participant wager history is available.");
  std::ostringstream output;
  if (result.exact) {
    const auto &wager = result.wagers.front();
    output << "**Wager " << wager.wager_id << "**";
    if (wager.is_test)
      output << " `[TEST]`";
    output << "\nStatus: " << wager_state_name(wager.state) << "\nCreator: <@"
           << wager.creator_user_id.str() << ">"
           << "\nTarget: <@" << wager.target_user_id.str() << ">";
    if (wager.proposition)
      output << "\nProposition: " << *wager.proposition;
    if (wager.stake)
      output << "\nStake: " << *wager.stake << " Fate each ("
             << (*wager.stake * 2) << " total escrow)";
    output << "\nVisibility: "
           << (wager.visibility == WagerVisibility::sealed ? "sealed"
                                                           : "public")
           << "\nOutcome window: " << wager.outcome_window_ms / 3'600'000
           << " hours after acceptance"
           << "\nResolution grace: " << wager.resolution_grace_ms / 3'600'000
           << " hours after the outcome deadline"
           << "\nResolution: "
           << (wager.resolution_policy == WagerResolutionPolicy::mutual
                   ? "mutual"
                   : "designated judge");
    if (wager.judge_user_id)
      output << " <@" << wager.judge_user_id->str() << ">";
    if (wager.evidence_instructions)
      output << "\nEvidence instructions: " << *wager.evidence_instructions;
    if (wager.offer_expires_at_ms)
      output << "\nOffer expires: "
             << discord_timestamp(*wager.offer_expires_at_ms);
    if (wager.outcome_due_at_ms)
      output << "\nOutcome due: "
             << discord_timestamp(*wager.outcome_due_at_ms);
    if (wager.resolution_grace_until_ms)
      output << "\nOwner escalation after: "
             << discord_timestamp(*wager.resolution_grace_until_ms);
    if (wager.winner)
      output << "\nWinner: "
             << (*wager.winner == WagerRole::creator ? "creator" : "target");
    if (wager.terminal_reason)
      output << "\nResolution reason: " << *wager.terminal_reason;
    if (!result.outcomes.empty()) {
      output << "\n\n**Private resolution activity**";
      for (const auto &entry : result.outcomes)
        output << "\n- " << entry;
    }
  } else {
    output << (disputes_only ? "**Visible wager disputes**"
                             : "**Your recent wagers**");
    for (const auto &wager : result.wagers)
      output << "\n" << state_sentence(wager);
  }
  if (!result.evidence.empty()) {
    output << "\n\n**Private evidence**";
    for (const auto &entry : result.evidence)
      output << "\n- " << entry;
  }
  if (result.evidence_total_count > result.evidence.size())
    output << "\n- " << (result.evidence_total_count - result.evidence.size())
           << " additional evidence entries remain in the durable audit.";
  auto message = text_message(bounded_discord_content(output.str()));
  if (result.next_cursor_id) {
    message.buttons.push_back({.custom_id = std::string{wager_history_prefix} +
                                            *result.next_cursor_id,
                               .label = "Older wagers",
                               .style = ButtonStyle::secondary});
  }
  for (const auto &control : result.controls) {
    message.buttons.push_back({.custom_id = control.custom_id,
                               .label = control.action,
                               .style = control.action == "Decline" ||
                                                control.action == "Cancel" ||
                                                control.action == "Discard"
                                            ? ButtonStyle::secondary
                                            : ButtonStyle::primary});
  }
  return message;
}

[[nodiscard]] WagerAction action_value(const std::string_view value) {
  if (value == "accept")
    return WagerAction::accept;
  if (value == "decline")
    return WagerAction::decline;
  if (value == "cancel")
    return WagerAction::cancel;
  if (value == "agree")
    return WagerAction::agree;
  if (value == "dispute")
    return WagerAction::dispute;
  if (value == "void")
    return WagerAction::void_wager;
  throw std::invalid_argument{"Wager action is invalid."};
}

[[nodiscard]] WagerRole participant_winner(const std::string_view value) {
  if (value == "creator")
    return WagerRole::creator;
  if (value == "target")
    return WagerRole::target;
  throw std::invalid_argument{"Wager winner is invalid."};
}

[[nodiscard]] WagerJudgment judgment_value(const std::string_view value) {
  if (value == "creator")
    return WagerJudgment::creator;
  if (value == "target")
    return WagerJudgment::target;
  if (value == "void")
    return WagerJudgment::void_wager;
  throw std::invalid_argument{"Wager judgment result is invalid."};
}

[[nodiscard]] WagerRole role_value(const std::string_view value) {
  if (value == "creator")
    return WagerRole::creator;
  if (value == "target")
    return WagerRole::target;
  if (value == "judge")
    return WagerRole::judge;
  if (value == "owner")
    return WagerRole::owner;
  throw std::invalid_argument{"Simulated wager role is invalid."};
}

[[nodiscard]] WagerDeadlinePhase deadline_value(const std::string_view value) {
  if (value == "draft")
    return WagerDeadlinePhase::draft_expiry;
  if (value == "offer")
    return WagerDeadlinePhase::offer_expiry;
  if (value == "reminder")
    return WagerDeadlinePhase::reminder;
  if (value == "outcome")
    return WagerDeadlinePhase::outcome_due;
  if (value == "grace")
    return WagerDeadlinePhase::grace;
  throw std::invalid_argument{"Wager deadline phase is invalid."};
}

} // namespace

void WagerPolicy::validate() const {
  if (minimum_stake < 1 || maximum_stake < minimum_stake ||
      maximum_stake > 100 || offer_expiry_hours < 1 ||
      offer_expiry_hours > 8'760 || default_outcome_hours < 1 ||
      default_outcome_hours > 24 * 7 || resolution_grace_hours < 1 ||
      resolution_grace_hours > 24 * 7)
    throw std::invalid_argument{"Wager policy is invalid."};
}

TarotWagerService::TarotWagerService(
    TarotWagerRepository &repository, const Clock &clock,
    PersistentIdGenerator &ids, WagerPolicy policy,
    const std::int64_t starting_fate, const ServerScopeConfiguration scope,
    const bool test_mode, Diagnostics &diagnostics,
    std::function<void()> wake_scheduler, std::function<void()> wake_outbox,
    std::function<void(std::string_view)> observer)
    : repository_{repository}, clock_{clock}, ids_{ids}, policy_{policy},
      starting_fate_{starting_fate}, scope_{scope}, test_mode_{test_mode},
      diagnostics_{diagnostics}, wake_scheduler_{std::move(wake_scheduler)},
      wake_outbox_{std::move(wake_outbox)}, observer_{std::move(observer)} {
  policy_.validate();
  if (starting_fate_ < 1 || !scope_.guild_id.is_set() ||
      !scope_.primary_channel_id.is_set() || !scope_.owner_user_id.is_set() ||
      !wake_scheduler_ || !wake_outbox_)
    throw std::invalid_argument{"Wager service dependencies are incomplete."};
}

WagerInvocation
TarotWagerService::invocation(const IncomingInteraction &interaction) const {
  return WagerInvocation{
      .user_id = interaction.user_id,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .interaction_idempotency_key =
          "wager:" + interaction.interaction_id.str(),
      .correlation_id = interaction.correlation_id,
      .now_ms = unix_milliseconds(clock_),
      .owner = interaction.user_id == scope_.owner_user_id,
      .test_mode = test_mode_,
  };
}

WagerIdFactory TarotWagerService::id_factory() {
  return [this] { return ids_.next_id(); };
}

InteractionMessage
TarotWagerService::create(const IncomingInteraction &interaction) {
  const auto target = option<DiscordSnowflake>(interaction, "target");
  if (!target)
    throw std::invalid_argument{"A wager target is required."};
  const auto &target_user = resolved_user(interaction, *target);
  if (target_user.is_bot)
    throw std::invalid_argument{"Bots cannot participate in peer wagers."};

  const auto visibility_option = option<std::string>(interaction, "visibility");
  const auto resolution_option = option<std::string>(interaction, "resolution");
  const auto judge = option<DiscordSnowflake>(interaction, "judge");
  const auto window = option<std::string>(interaction, "outcome-in");
  if (visibility_option && *visibility_option != "public" &&
      *visibility_option != "sealed")
    throw std::invalid_argument{"Wager visibility is invalid."};
  if (resolution_option && *resolution_option != "mutual" &&
      *resolution_option != "designated")
    throw std::invalid_argument{"Wager resolution policy is invalid."};
  const auto visibility =
      visibility_option == std::optional<std::string>{"sealed"}
          ? WagerVisibility::sealed
          : WagerVisibility::public_offer;
  const auto resolution =
      resolution_option == std::optional<std::string>{"designated"}
          ? WagerResolutionPolicy::designated
          : WagerResolutionPolicy::mutual;
  if ((resolution == WagerResolutionPolicy::designated) != judge.has_value())
    throw std::invalid_argument{
        "Designated resolution requires exactly one judge."};
  const bool self_test = *target == interaction.user_id;
  if (judge) {
    const auto &judge_user = resolved_user(interaction, *judge);
    if (judge_user.is_bot)
      throw std::invalid_argument{
          "The wager judge must be a distinct human member."};
    if (self_test && *judge != interaction.user_id)
      throw std::invalid_argument{
          "A designated self-test wager must simulate the owner as judge."};
    if (!self_test && (*judge == interaction.user_id || *judge == *target))
      throw std::invalid_argument{
          "The wager judge must be a distinct human member."};
  }
  if (self_test && !(test_mode_ && interaction.user_id == scope_.owner_user_id))
    throw std::invalid_argument{
        "Self-targeted wagers require owner test mode."};

  const auto result = repository_.create_draft(WagerCreateRequest{
      .invocation = invocation(interaction),
      .target_user_id = *target,
      .judge_user_id = judge,
      .visibility = visibility,
      .resolution_policy = resolution,
      .outcome_window_ms =
          window ? outcome_window(*window)
                 : checked_hours_ms(policy_.default_outcome_hours),
      .resolution_grace_ms = checked_hours_ms(policy_.resolution_grace_hours),
      .draft_expires_at_ms =
          unix_milliseconds(clock_) + wager_control_lifetime_ms,
      .is_test = self_test,
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::preview(const IncomingInteraction &interaction) {
  const auto token = parse_wager_form(interaction.custom_id);
  const auto proposition = modal_field(interaction, "proposition");
  const auto stake_text = modal_field(interaction, "stake");
  const auto instructions = modal_field(interaction, "evidence_instructions");
  if (!token || !proposition || !stake_text ||
      interaction.modal_fields.size() != 3 || blank(*proposition) ||
      proposition->size() > 500 ||
      (instructions && !instructions->empty() && instructions->size() > 500))
    throw std::invalid_argument{"Wager form fields are invalid."};
  std::int64_t stake{};
  const auto parsed = std::from_chars(
      stake_text->data(), stake_text->data() + stake_text->size(), stake);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != stake_text->data() + stake_text->size() ||
      stake < policy_.minimum_stake || stake > policy_.maximum_stake)
    throw std::invalid_argument{
        "Wager stake is outside the configured equal-stake range."};
  auto optional_instructions = instructions;
  if (optional_instructions && blank(*optional_instructions))
    optional_instructions.reset();
  const auto result = repository_.preview(WagerPreviewRequest{
      .invocation = invocation(interaction),
      .token_id = *token,
      .proposition = *proposition,
      .stake = stake,
      .evidence_instructions = std::move(optional_instructions),
      .offer_expiry_ms = checked_hours_ms(policy_.offer_expiry_hours),
      .next_id = id_factory(),
  });
  post_commit(result);
  auto message = render_wager(result);
  if (result.status == WagerMutationStatus::applied && result.wager) {
    message.content +=
        "\nOffer expiry: " +
        std::to_string(result.wager->offer_duration_ms.value_or(0) /
                       3'600'000) +
        " hours after confirmation."
        "\nOutcome window: " +
        std::to_string(result.wager->outcome_window_ms / 3'600'000) +
        " hours after acceptance."
        "\nResolution grace: " +
        std::to_string(result.wager->resolution_grace_ms / 3'600'000) +
        " hours after the outcome deadline; timeout never awards Fate.";
  }
  return message;
}

InteractionMessage
TarotWagerService::apply_component(const IncomingInteraction &interaction) {
  const auto token = parse_wager_component(interaction.custom_id);
  if (!token)
    return text_message(
        "This wager control is invalid or no longer available.");
  const auto result = repository_.act(WagerActionRequest{
      .invocation = invocation(interaction),
      .wager_id = {},
      .token_id = *token,
      .action = WagerAction::cancel,
      .starting_fate = starting_fate_,
      .offer_expiry_ms = checked_hours_ms(policy_.offer_expiry_hours),
      .resolution_grace_ms = checked_hours_ms(policy_.resolution_grace_hours),
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::action(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto selected_action = option<std::string>(interaction, "action");
  if (!reference || !valid_uuid_v4(*reference) || !selected_action)
    throw std::invalid_argument{"Wager reference or action is invalid."};
  const auto result = repository_.act(WagerActionRequest{
      .invocation = invocation(interaction),
      .wager_id = *reference,
      .token_id = std::nullopt,
      .action = action_value(*selected_action),
      .starting_fate = starting_fate_,
      .offer_expiry_ms = checked_hours_ms(policy_.offer_expiry_hours),
      .resolution_grace_ms = checked_hours_ms(policy_.resolution_grace_hours),
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::outcome(const IncomingInteraction &interaction) {
  std::optional<std::string> token;
  std::optional<std::string> reference;
  std::optional<std::string> winner;
  if (interaction.kind == InteractionKind::modal_submit) {
    token = parse_wager_outcome_form(interaction.custom_id);
    winner = modal_field(interaction, "winner");
  } else {
    reference = option<std::string>(interaction, "reference");
    winner = option<std::string>(interaction, "winner");
  }
  if ((!token && (!reference || !valid_uuid_v4(*reference))) || !winner ||
      (interaction.kind == InteractionKind::modal_submit &&
       interaction.modal_fields.size() != 1))
    throw std::invalid_argument{"Wager outcome request is invalid."};
  const auto result = repository_.submit_outcome(WagerOutcomeRequest{
      .invocation = invocation(interaction),
      .wager_id = reference.value_or(""),
      .token_id = token,
      .winner = participant_winner(*winner),
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::evidence(const IncomingInteraction &interaction) {
  std::optional<std::string> token;
  std::optional<std::string> reference;
  std::optional<std::string> body;
  if (interaction.kind == InteractionKind::modal_submit) {
    token = parse_wager_evidence_form(interaction.custom_id);
    body = modal_field(interaction, "evidence");
  } else {
    reference = option<std::string>(interaction, "reference");
    body = option<std::string>(interaction, "evidence");
  }
  if ((!token && (!reference || !valid_uuid_v4(*reference))) || !body ||
      blank(*body) || body->size() > 1000)
    throw std::invalid_argument{"Wager evidence is invalid."};
  const auto result = repository_.add_evidence(WagerEvidenceRequest{
      .invocation = invocation(interaction),
      .wager_id = reference.value_or(""),
      .token_id = token,
      .body = *body,
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::judgment(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto result_option = option<std::string>(interaction, "result");
  const auto reason = option<std::string>(interaction, "reason");
  if (!reference || !valid_uuid_v4(*reference) || !result_option || !reason ||
      blank(*reason) || reason->size() > 200)
    throw std::invalid_argument{"Wager judgment is invalid."};
  const auto result = repository_.judge(WagerJudgmentRequest{
      .invocation = invocation(interaction),
      .wager_id = *reference,
      .judgment = judgment_value(*result_option),
      .reason = *reason,
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::wagers(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto cursor = parse_wager_history(interaction.custom_id);
  if (reference && !valid_uuid_v4(*reference))
    throw std::invalid_argument{"Wager reference is invalid."};
  if (cursor && interaction.kind != InteractionKind::button)
    throw std::invalid_argument{"Wager history cursor is invalid."};
  return render_history(repository_.history(WagerHistoryRequest{
                            .invocation = invocation(interaction),
                            .wager_id = reference,
                            .cursor_id = cursor,
                            .next_id = id_factory(),
                        }),
                        false);
}

InteractionMessage
TarotWagerService::disputes(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  if (reference && !valid_uuid_v4(*reference))
    throw std::invalid_argument{"Wager reference is invalid."};
  return render_history(repository_.disputes(WagerHistoryRequest{
                            .invocation = invocation(interaction),
                            .wager_id = reference,
                            .cursor_id = std::nullopt,
                            .next_id = id_factory(),
                        }),
                        true);
}

InteractionMessage
TarotWagerService::set_test_role(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto role = option<std::string>(interaction, "role");
  if (!reference || !valid_uuid_v4(*reference) || !role)
    throw std::invalid_argument{"Test wager role request is invalid."};
  const auto result = repository_.set_test_role(WagerTestRoleRequest{
      .invocation = invocation(interaction),
      .wager_id = *reference,
      .role = role_value(*role),
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::force_test_deadline(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto phase = option<std::string>(interaction, "phase");
  if (!reference || !valid_uuid_v4(*reference) || !phase)
    throw std::invalid_argument{"Test wager deadline request is invalid."};
  const auto result = repository_.force_test_deadline(WagerTestDeadlineRequest{
      .invocation = invocation(interaction),
      .wager_id = *reference,
      .phase = deadline_value(*phase),
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

InteractionMessage
TarotWagerService::cleanup_test_wager(const IncomingInteraction &interaction) {
  const auto reference = option<std::string>(interaction, "reference");
  const auto reason = option<std::string>(interaction, "reason");
  if (!reference || !valid_uuid_v4(*reference) || !reason || blank(*reason) ||
      reason->size() > 200)
    throw std::invalid_argument{"Test wager cleanup request is invalid."};
  const auto result = repository_.cleanup_test_wager(WagerTestCleanupRequest{
      .invocation = invocation(interaction),
      .wager_id = *reference,
      .reason = *reason,
      .next_id = id_factory(),
  });
  post_commit(result);
  return render_wager(result);
}

void TarotWagerService::handle_deadline(const ClaimedScheduledJob &job) {
  const auto result = repository_.handle_deadline(WagerDeadlineRequest{
      .job = job,
      .now_ms = unix_milliseconds(clock_),
      .next_id = id_factory(),
  });
  post_commit(result);
}

WagerInvariantReport TarotWagerService::check_invariants() {
  return repository_.check_invariants();
}

ModalPayload TarotWagerService::wager_form(std::string token_id) {
  if (!valid_uuid_v4(token_id))
    throw std::invalid_argument{"Wager modal token is invalid."};
  return ModalPayload{
      .custom_id = std::string{wager_form_prefix} + token_id,
      .title = "Prepare a peer wager",
      .fields =
          {
              {.custom_id = "proposition",
               .label = "Proposition",
               .minimum_length = 1,
               .maximum_length = 500,
               .required = true,
               .style = ModalFieldPayload::Style::paragraph},
              {.custom_id = "stake",
               .label = "Equal stake per participant",
               .minimum_length = 1,
               .maximum_length = 3,
               .required = true},
              {.custom_id = "evidence_instructions",
               .label = "Evidence instructions (optional)",
               .minimum_length = 0,
               .maximum_length = 500,
               .required = false,
               .style = ModalFieldPayload::Style::paragraph},
          },
  };
}

ModalPayload TarotWagerService::evidence_form(std::string token_id) {
  if (!valid_uuid_v4(token_id))
    throw std::invalid_argument{"Wager evidence token is invalid."};
  return ModalPayload{
      .custom_id = std::string{wager_evidence_prefix} + token_id,
      .title = "Add private wager evidence",
      .fields = {{.custom_id = "evidence",
                  .label = "Evidence",
                  .minimum_length = 1,
                  .maximum_length = 1000,
                  .required = true,
                  .style = ModalFieldPayload::Style::paragraph}},
  };
}

ModalPayload TarotWagerService::outcome_form(std::string token_id) {
  if (!valid_uuid_v4(token_id))
    throw std::invalid_argument{"Wager outcome token is invalid."};
  return ModalPayload{
      .custom_id = std::string{wager_outcome_prefix} + token_id,
      .title = "Submit wager outcome",
      .fields = {{.custom_id = "winner",
                  .label = "Winner (creator or target)",
                  .minimum_length = 6,
                  .maximum_length = 7,
                  .required = true}},
  };
}

void TarotWagerService::post_commit(
    const WagerMutationResult &result) const noexcept {
  if (result.status != WagerMutationStatus::applied)
    return;
  const auto invoke = [this](auto &&callback) {
    try {
      callback();
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error, "wager.post_commit",
                         error.what(), std::nullopt});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error, "wager.post_commit",
                         "Unknown wager post-commit observer failure.",
                         std::nullopt});
    }
  };
  if (result.public_delivery_created)
    invoke([this] { wake_outbox_(); });
  invoke([this] { wake_scheduler_(); });
  if (observer_) {
    for (const auto &event : result.committed_event_types)
      invoke([this, &event] { observer_(event); });
  }
}

std::optional<std::string>
parse_wager_component(const std::string_view custom_id) {
  if (!custom_id.starts_with(wager_component_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(wager_component_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::optional<std::string> parse_wager_form(const std::string_view custom_id) {
  if (!custom_id.starts_with(wager_form_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(wager_form_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::optional<std::string>
parse_wager_evidence_form(const std::string_view custom_id) {
  if (!custom_id.starts_with(wager_evidence_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(wager_evidence_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::optional<std::string>
parse_wager_outcome_form(const std::string_view custom_id) {
  if (!custom_id.starts_with(wager_outcome_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(wager_outcome_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::optional<std::string>
parse_wager_history(const std::string_view custom_id) {
  if (!custom_id.starts_with(wager_history_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(wager_history_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

const char *wager_state_name(const WagerState state) noexcept {
  switch (state) {
  case WagerState::draft:
    return "Draft";
  case WagerState::offered:
    return "Offered";
  case WagerState::accepted_funded:
    return "Funded";
  case WagerState::awaiting_resolution:
    return "Awaiting resolution";
  case WagerState::disputed:
    return "Disputed";
  case WagerState::resolved:
    return "Resolved";
  case WagerState::void_refunded:
    return "Void — refunded";
  case WagerState::cancelled:
    return "Cancelled";
  case WagerState::declined:
    return "Declined";
  case WagerState::expired:
    return "Expired";
  }
  return "Unknown";
}

const char *wager_role_name(const WagerRole role) noexcept {
  switch (role) {
  case WagerRole::creator:
    return "creator";
  case WagerRole::target:
    return "target";
  case WagerRole::judge:
    return "judge";
  case WagerRole::owner:
    return "owner";
  case WagerRole::scheduler:
    return "scheduler";
  }
  return "unknown";
}

const char *wager_deadline_phase_name(const WagerDeadlinePhase phase) noexcept {
  switch (phase) {
  case WagerDeadlinePhase::draft_expiry:
    return "draft_expiry";
  case WagerDeadlinePhase::offer_expiry:
    return "offer_expiry";
  case WagerDeadlinePhase::reminder:
    return "reminder";
  case WagerDeadlinePhase::outcome_due:
    return "outcome_due";
  case WagerDeadlinePhase::grace:
    return "grace";
  }
  return "unknown";
}

} // namespace sanguinius
