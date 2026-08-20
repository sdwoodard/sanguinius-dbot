#include "sanguinius/tarot.hpp"

#include "sanguinius/durable_work.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace sanguinius {
namespace {

constexpr std::int64_t maximum_fate = 1'000'000'000;
constexpr std::int64_t milliseconds_per_hour = 60 * 60 * 1'000;

[[nodiscard]] const InteractionOption *
option(const IncomingInteraction &interaction, const std::string_view name) {
  const auto found = std::ranges::find(interaction.command_options, name,
                                       &InteractionOption::name);
  return found == interaction.command_options.end() ? nullptr : &*found;
}

[[nodiscard]] std::string recovery_name(const TarotRecoveryKind kind) {
  return kind == TarotRecoveryKind::grace ? "Grace of the Throne"
                                          : "Trial of Renewal";
}

[[nodiscard]] bool blank_reason(const std::string_view reason) {
  return reason.empty() ||
         std::ranges::all_of(reason, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

[[nodiscard]] InteractionMessage history_message(const TarotHistoryPage &page) {
  if (page.status != TarotPageStatus::available) {
    switch (page.status) {
    case TarotPageStatus::invalid_token:
      return text_message("That Fate history control is invalid.");
    case TarotPageStatus::wrong_user:
      return text_message("That Fate history belongs to another participant.");
    case TarotPageStatus::wrong_scope:
      return text_message(
          "Use that Fate history control in its original channel.");
    case TarotPageStatus::expired:
      return text_message("That Fate history snapshot has expired. Run `/tarot "
                          "history` again.");
    case TarotPageStatus::available:
      break;
    }
  }

  std::ostringstream output;
  output << "Your immutable Fate history";
  if (page.total == 0) {
    output << "\nNo ledger entries are available.";
  } else {
    output << " (" << (page.offset + 1) << "-"
           << std::min(page.offset + page.entries.size(), page.total) << " of "
           << page.total << ")";
    for (const auto &entry : page.entries) {
      output << "\n"
             << (entry.amount > 0 ? "+" : "") << entry.amount << " Fate · "
             << entry.transaction_type << " · balance " << entry.balance_after
             << " · #" << entry.ledger_sequence << " · "
             << entry.transaction_id;
      if (entry.is_test)
        output << " [TEST]";
      if (entry.reason)
        output << " · " << *entry.reason;
    }
  }
  auto message = text_message(output.str());
  if (page.next_custom_id) {
    message.buttons.push_back(ButtonPayload{.custom_id = *page.next_custom_id,
                                            .label = "Next five",
                                            .disabled = false,
                                            .style = ButtonStyle::secondary});
  }
  return message;
}

[[nodiscard]] InteractionMessage
recovery_message(const TarotRecoveryResult &result) {
  const auto name = recovery_name(result.kind);
  switch (result.status) {
  case TarotRecoveryStatus::pending: {
    std::ostringstream output;
    output << name << " is ready. This sealed choice expires in 15 minutes.";
    if (result.kind == TarotRecoveryKind::trial && result.prompt_variant) {
      static constexpr std::string_view prompts[]{
          "Choose the vow that steadies your hand.",
          "Choose the oath by which you rise again.",
          "Choose the promise you will carry forward."};
      output << "\n"
             << prompts[static_cast<std::size_t>(*result.prompt_variant)];
    }
    auto message = text_message(output.str());
    if (result.kind == TarotRecoveryKind::grace) {
      message.buttons.push_back(
          {.custom_id = result.custom_ids.at(0), .label = "Claim Grace"});
    } else {
      static constexpr std::string_view labels[]{"I will endure",
                                                 "I will serve", "I will rise"};
      for (std::size_t index = 0; index < 3; ++index) {
        message.buttons.push_back({.custom_id = result.custom_ids.at(index),
                                   .label = std::string{labels[index]}});
      }
      message.buttons.push_back({.custom_id = result.custom_ids.at(3),
                                 .label = "Abandon",
                                 .disabled = false,
                                 .style = ButtonStyle::secondary});
    }
    return message;
  }
  case TarotRecoveryStatus::completed: {
    std::ostringstream output;
    output << name << " is complete. ";
    if (result.reward)
      output << *result.reward << " Fate was entered in the immutable ledger. ";
    output << "Your balance is now " << result.balance << ".";
    return text_message(output.str());
  }
  case TarotRecoveryStatus::ineligible:
    return text_message("You are not currently eligible for " + name +
                        ". Your balance is " + std::to_string(result.balance) +
                        ".");
  case TarotRecoveryStatus::cooldown:
    return text_message(name +
                        " is still on cooldown in this ledger namespace.");
  case TarotRecoveryStatus::expired:
    return text_message(name + " expired without awarding Fate.");
  case TarotRecoveryStatus::abandoned:
    return text_message(name + " was abandoned without awarding Fate.");
  case TarotRecoveryStatus::lost_eligibility:
    return text_message("Your eligibility changed before the claim completed; "
                        "no Fate or cooldown was recorded.");
  case TarotRecoveryStatus::wrong_user:
    return text_message("That recovery choice belongs to another participant.");
  case TarotRecoveryStatus::wrong_scope:
    return text_message("Use that recovery choice in its original channel.");
  case TarotRecoveryStatus::invalid_token:
    return text_message("That recovery choice is invalid or unavailable.");
  }
  return text_message("That recovery choice is unavailable.");
}

} // namespace

void TarotPolicy::validate() const {
  const auto valid_fate = [](const std::int64_t value) {
    return value >= 1 && value <= maximum_fate;
  };
  const auto valid_hours = [](const std::int64_t value) {
    return value >= 1 && value <= 8'760;
  };
  if (!valid_fate(starting_fate) || !valid_fate(grace_threshold) ||
      !valid_fate(grace_target) || !valid_fate(trial_threshold) ||
      !valid_fate(trial_reward_min) || !valid_fate(trial_reward_max) ||
      !valid_hours(grace_cooldown_hours) ||
      !valid_hours(trial_cooldown_hours) || grace_target <= grace_threshold ||
      trial_reward_min > trial_reward_max) {
    throw std::invalid_argument{"Tarot policy values are invalid."};
  }
}

std::optional<std::string>
parse_tarot_component(const std::string_view custom_id) {
  if (!custom_id.starts_with(tarot_component_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(tarot_component_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

TarotService::TarotService(TarotRepository &repository, const Clock &clock,
                           PersistentIdGenerator &ids, Random &random,
                           TarotPolicy policy, ServerScopeConfiguration scope,
                           const bool test_mode, Diagnostics &diagnostics,
                           std::function<void()> wake_outbox,
                           std::function<void(std::string_view)> observer)
    : repository_{repository}, clock_{clock}, ids_{ids}, random_{random},
      policy_{policy}, scope_{std::move(scope)}, test_mode_{test_mode},
      diagnostics_{diagnostics}, wake_outbox_{std::move(wake_outbox)},
      observer_{std::move(observer)} {
  policy_.validate();
  if (!scope_.guild_id.is_set() || !scope_.primary_channel_id.is_set() ||
      !scope_.owner_user_id.is_set() || !wake_outbox_) {
    throw std::invalid_argument{"Tarot service dependencies are invalid."};
  }
}

void TarotService::initialize() {
  std::vector<std::string> ids;
  ids.reserve(4);
  for (std::size_t index = 0; index < 4; ++index)
    ids.push_back(ids_.next_id());
  repository_.initialize_system_accounts(
      ids, std::chrono::duration_cast<std::chrono::milliseconds>(
               clock_.now().time_since_epoch())
               .count());
  const auto report = repository_.check_invariants();
  if (!report.valid)
    throw std::runtime_error{
        "Tarot ledger invariant verification failed during startup."};
}

TarotInvocation
TarotService::invocation(const IncomingInteraction &interaction) const {
  return TarotInvocation{
      .user_id = interaction.user_id,
      .guild_id = interaction.guild_id,
      .channel_id = interaction.channel_id,
      .display_name = interaction.display_name.empty()
                          ? interaction.username
                          : interaction.display_name,
      .interaction_idempotency_key =
          "tarot.interaction:" + interaction.interaction_id.str(),
      .correlation_id = interaction.correlation_id,
      .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock_.now().time_since_epoch())
                    .count(),
  };
}

TarotAccountProvisionResult
TarotService::ensure_account(const TarotInvocation &call) {
  auto result = repository_.ensure_account(TarotAccountProvisionRequest{
      .invocation = call,
      .starting_fate = policy_.starting_fate,
      .account_id = ids_.next_id(),
      .transaction_id = ids_.next_id(),
      .event_id = ids_.next_id(),
      .mint_posting_id = ids_.next_id(),
      .human_posting_id = ids_.next_id(),
  });
  if (result.created)
    observe("tarot.starting_grant.v1");
  return result;
}

InteractionMessage
TarotService::balance(const IncomingInteraction &interaction) {
  const auto provisioned = ensure_account(invocation(interaction));
  return text_message("Your Fate balance is " +
                      std::to_string(provisioned.balance) + ".");
}

InteractionMessage
TarotService::history(const IncomingInteraction &interaction) {
  const auto call = invocation(interaction);
  static_cast<void>(ensure_account(call));
  std::vector<std::string> tokens;
  tokens.reserve((tarot_history_maximum_items / tarot_history_page_size) - 1);
  for (std::size_t index = tarot_history_page_size;
       index < tarot_history_maximum_items; index += tarot_history_page_size)
    tokens.push_back(ids_.next_id());
  return history_message(repository_.create_history_snapshot(
      {.invocation = call,
       .cursor_id = ids_.next_id(),
       .page_token_ids = std::move(tokens)}));
}

InteractionMessage
TarotService::standings(const IncomingInteraction &interaction) {
  static_cast<void>(ensure_account(invocation(interaction)));
  const auto rows = repository_.standings();
  std::ostringstream output;
  output << "Fate standings";
  if (rows.empty()) {
    output << "\nNo participants are publicly listed.";
  } else {
    std::size_t rank = 1;
    for (const auto &row : rows)
      output << "\n"
             << rank++ << ". " << row.display_name << " — " << row.balance;
  }
  auto message = text_message(output.str());
  message.allowed_user_mentions.clear();
  return message;
}

InteractionMessage
TarotService::set_standings_visibility(const IncomingInteraction &interaction) {
  const auto call = invocation(interaction);
  static_cast<void>(ensure_account(call));
  const auto *mode_option = option(interaction, "mode");
  const auto *mode = mode_option == nullptr
                         ? nullptr
                         : std::get_if<std::string>(&mode_option->value);
  if (mode == nullptr || (*mode != "public" && *mode != "private"))
    throw std::invalid_argument{"Tarot standings visibility is invalid."};
  const auto result = repository_.set_standings_visibility(
      {.invocation = call,
       .public_standings = *mode == "public",
       .event_id = ids_.next_id()});
  if (result.changed)
    observe("tarot.standings_visibility_changed.v1");
  return text_message(std::string{"Your Fate standings visibility is "} +
                      (result.public_standings ? "public." : "private."));
}

TarotVisibility TarotService::requested_visibility(
    const IncomingInteraction &interaction) const {
  const auto *visibility_option = option(interaction, "visibility");
  if (visibility_option == nullptr)
    return TarotVisibility::public_result;
  const auto *visibility = std::get_if<std::string>(&visibility_option->value);
  if (visibility == nullptr ||
      (*visibility != "public" && *visibility != "private"))
    throw std::invalid_argument{"Tarot recovery visibility is invalid."};
  return *visibility == "private" ? TarotVisibility::private_result
                                  : TarotVisibility::public_result;
}

InteractionMessage
TarotService::start_recovery(const IncomingInteraction &interaction,
                             const TarotRecoveryKind kind) {
  const auto call = invocation(interaction);
  static_cast<void>(ensure_account(call));
  std::function<TarotTrialDraw()> trial_draw;
  std::optional<std::string> draw_id;
  std::vector<std::string> tokens;
  if (kind == TarotRecoveryKind::trial) {
    trial_draw = [this] {
      const auto variant = static_cast<std::int64_t>(random_.uniform(3));
      const auto reward_span = static_cast<std::uint64_t>(
          policy_.trial_reward_max - policy_.trial_reward_min + 1);
      return TarotTrialDraw{
          .reward = policy_.trial_reward_min +
                    static_cast<std::int64_t>(random_.uniform(reward_span)),
          .prompt_variant = variant};
    };
    draw_id = ids_.next_id();
    for (std::size_t index = 0; index < 4; ++index)
      tokens.push_back(ids_.next_id());
  } else {
    tokens.push_back(ids_.next_id());
  }
  const auto result = repository_.start_recovery(TarotRecoveryStartRequest{
      .invocation = call,
      .kind = kind,
      .visibility = requested_visibility(interaction),
      .is_test = test_mode_ && call.user_id == scope_.owner_user_id,
      .threshold = kind == TarotRecoveryKind::grace ? policy_.grace_threshold
                                                    : policy_.trial_threshold,
      .grace_target = kind == TarotRecoveryKind::grace
                          ? std::optional<std::int64_t>{policy_.grace_target}
                          : std::nullopt,
      .cooldown_ms =
          (kind == TarotRecoveryKind::grace ? policy_.grace_cooldown_hours
                                            : policy_.trial_cooldown_hours) *
          milliseconds_per_hour,
      .trial_draw = std::move(trial_draw),
      .claim_id = ids_.next_id(),
      .draw_id = std::move(draw_id),
      .started_event_id = ids_.next_id(),
      .expired_event_id = ids_.next_id(),
      .token_ids = std::move(tokens),
  });
  for (const auto &event_type : result.committed_event_types)
    observe(event_type);
  return recovery_message(result);
}

InteractionMessage
TarotService::start_grace(const IncomingInteraction &interaction) {
  return start_recovery(interaction, TarotRecoveryKind::grace);
}

InteractionMessage
TarotService::start_trial(const IncomingInteraction &interaction) {
  return start_recovery(interaction, TarotRecoveryKind::trial);
}

InteractionMessage
TarotService::apply_component(const IncomingInteraction &interaction) {
  const auto token = parse_tarot_component(interaction.custom_id);
  if (!token)
    return text_message("That Tarot control is invalid.");
  const auto call = invocation(interaction);
  auto page =
      repository_.history_page({.invocation = call, .token_id = *token});
  if (page.status != TarotPageStatus::invalid_token)
    return history_message(page);

  const auto outbox_id = ids_.next_id();
  const auto result = repository_.complete_recovery(
      {.invocation = call,
       .token_id = *token,
       .action = TarotRecoveryAction::claim,
       .grace_cooldown_ms =
           policy_.grace_cooldown_hours * milliseconds_per_hour,
       .trial_cooldown_ms =
           policy_.trial_cooldown_hours * milliseconds_per_hour,
       .transaction_id = ids_.next_id(),
       .event_id = ids_.next_id(),
       .mint_posting_id = ids_.next_id(),
       .human_posting_id = ids_.next_id(),
       .outbox_id = outbox_id,
       .provider_nonce = discord_nonce_from_uuid(outbox_id)});
  for (const auto &event_type : result.committed_event_types)
    observe(event_type);
  if (result.public_delivery_created)
    wake_outbox_();
  return recovery_message(result);
}

InteractionMessage
TarotService::adjust(const IncomingInteraction &interaction) {
  if (!test_mode_ || interaction.user_id != scope_.owner_user_id ||
      interaction.guild_id != scope_.guild_id ||
      interaction.channel_id != scope_.primary_channel_id)
    throw std::invalid_argument{"Tarot test adjustment is not authorized."};
  const auto call = invocation(interaction);
  static_cast<void>(ensure_account(call));
  const auto *amount_option = option(interaction, "amount");
  const auto *reason_option = option(interaction, "reason");
  const auto *amount = amount_option == nullptr
                           ? nullptr
                           : std::get_if<std::int64_t>(&amount_option->value);
  const auto *reason = reason_option == nullptr
                           ? nullptr
                           : std::get_if<std::string>(&reason_option->value);
  if (amount == nullptr || *amount == 0 || *amount < -maximum_fate ||
      *amount > maximum_fate || reason == nullptr || blank_reason(*reason) ||
      reason->size() > 200)
    throw std::invalid_argument{"Tarot test adjustment is invalid."};
  const auto result = repository_.adjust({.invocation = call,
                                          .amount = *amount,
                                          .reason = *reason,
                                          .transaction_id = ids_.next_id(),
                                          .event_id = ids_.next_id(),
                                          .system_posting_id = ids_.next_id(),
                                          .human_posting_id = ids_.next_id()});
  if (result.status == TarotMutationStatus::applied)
    observe("tarot.admin_adjusted.v1");
  if (result.status == TarotMutationStatus::would_overdraw)
    return text_message(
        "That [TEST] adjustment would make the balance negative.");
  return text_message("[TEST] adjustment " + result.transaction_id +
                      " recorded. Balance: " + std::to_string(result.balance) +
                      ".");
}

InteractionMessage
TarotService::reverse(const IncomingInteraction &interaction) {
  if (!test_mode_ || interaction.user_id != scope_.owner_user_id ||
      interaction.guild_id != scope_.guild_id ||
      interaction.channel_id != scope_.primary_channel_id)
    throw std::invalid_argument{"Tarot test reversal is not authorized."};
  const auto call = invocation(interaction);
  static_cast<void>(ensure_account(call));
  const auto *reference_option = option(interaction, "transaction");
  const auto *reason_option = option(interaction, "reason");
  const auto *reference =
      reference_option == nullptr
          ? nullptr
          : std::get_if<std::string>(&reference_option->value);
  const auto *reason = reason_option == nullptr
                           ? nullptr
                           : std::get_if<std::string>(&reason_option->value);
  if (reference == nullptr || !valid_uuid_v4(*reference) || reason == nullptr ||
      blank_reason(*reason) || reason->size() > 200)
    throw std::invalid_argument{"Tarot test reversal is invalid."};
  const auto result =
      repository_.reverse({.invocation = call,
                           .original_transaction_id = *reference,
                           .reason = *reason,
                           .transaction_id = ids_.next_id(),
                           .event_id = ids_.next_id(),
                           .first_posting_id = ids_.next_id(),
                           .second_posting_id = ids_.next_id()});
  if (result.status == TarotMutationStatus::applied)
    observe("tarot.transaction_reversed.v1");
  if (result.status == TarotMutationStatus::forbidden)
    return text_message("Only an unreversed [TEST] transaction for your "
                        "account may be reversed.");
  if (result.status == TarotMutationStatus::would_overdraw)
    return text_message("That reversal would make the balance negative.");
  if (result.status == TarotMutationStatus::not_found)
    return text_message("That test transaction was not found.");
  return text_message("[TEST] reversal " + result.transaction_id +
                      " recorded. Balance: " + std::to_string(result.balance) +
                      ".");
}

TarotInvariantReport TarotService::check_invariants() {
  return repository_.check_invariants();
}

std::string TarotService::privacy_summary(const DiscordSnowflake &user_id) {
  return std::string{"Fate standings: "} +
         (repository_.standings_visibility(user_id) ? "public" : "private") +
         "\nFate ledger: retained as an immutable financial audit; balances "
         "are derived from postings.";
}

void TarotService::observe(const std::string_view event_type) const noexcept {
  if (!observer_)
    return;
  try {
    observer_(event_type);
  } catch (const std::exception &error) {
    diagnostics_.emit(
        {DiagnosticSeverity::error, "tarot.observer", error.what(), {}});
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error,
                       "tarot.observer",
                       "Unknown Tarot observer failure.",
                       {}});
  }
}

} // namespace sanguinius
