#include "sanguinius/tarot_house.hpp"
#include "sanguinius/tarot_integration.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sanguinius {
namespace {

[[nodiscard]] const InteractionOption *
option(const IncomingInteraction &interaction, const std::string_view name) {
  const auto found = std::ranges::find(interaction.command_options, name,
                                       &InteractionOption::name);
  return found == interaction.command_options.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::string>
string_option(const IncomingInteraction &interaction,
              const std::string_view name) {
  const auto *candidate = option(interaction, name);
  if (candidate == nullptr)
    return std::nullopt;
  const auto *value = std::get_if<std::string>(&candidate->value);
  if (value == nullptr)
    throw std::invalid_argument{std::string{name} + " must be text."};
  return *value;
}

[[nodiscard]] std::optional<std::int64_t>
integer_option(const IncomingInteraction &interaction,
               const std::string_view name) {
  const auto *candidate = option(interaction, name);
  if (candidate == nullptr)
    return std::nullopt;
  const auto *value = std::get_if<std::int64_t>(&candidate->value);
  if (value == nullptr)
    throw std::invalid_argument{std::string{name} + " must be an integer."};
  return *value;
}

[[nodiscard]] TarotVisibility
visibility(const IncomingInteraction &interaction,
           const TarotVisibility fallback = TarotVisibility::private_result) {
  const auto requested = string_option(interaction, "visibility");
  if (!requested)
    return fallback;
  if (*requested == "public")
    return TarotVisibility::public_result;
  if (*requested == "private")
    return TarotVisibility::private_result;
  throw std::invalid_argument{"Tarot visibility is invalid."};
}

[[nodiscard]] bool blank(const std::string_view value) {
  return value.empty() ||
         std::ranges::all_of(value, [](const unsigned char character) {
           return std::isspace(character) != 0;
         });
}

[[nodiscard]] std::string house_state_name(const HouseWagerState state) {
  switch (state) {
  case HouseWagerState::accepted_funded:
    return "Funded";
  case HouseWagerState::resolved:
    return "Resolved";
  case HouseWagerState::void_refunded:
    return "Void/refunded";
  }
  return "Unknown";
}

[[nodiscard]] InteractionMessage
house_result_message(const HouseMutationResult &result) {
  switch (result.status) {
  case HouseMutationStatus::not_found:
    return text_message("That House wager was not found.");
  case HouseMutationStatus::forbidden:
    return text_message("You are not authorized to change that House wager.");
  case HouseMutationStatus::ineligible:
    return text_message("You are not currently eligible for that augury.");
  case HouseMutationStatus::cooldown:
    return text_message(
        "That augury is already active or still on its terminal cooldown.");
  case HouseMutationStatus::insufficient_funds:
    return text_message("Your available Fate cannot fund that stake.");
  case HouseMutationStatus::exposure_blocked:
    return text_message(
        "The House is at its current exposure limit. No Fate moved.");
  case HouseMutationStatus::invalid_state:
    return text_message("That House wager is no longer in the required state.");
  case HouseMutationStatus::applied:
  case HouseMutationStatus::replay:
    break;
  }
  if (!result.wager)
    return text_message("The House operation was recorded.");
  const auto &wager = *result.wager;
  std::ostringstream output;
  if (result.status == HouseMutationStatus::replay)
    output << "Replayed persisted House wager.\n";
  output << wager.template_slug << " · " << house_state_name(wager.state)
         << " · " << wager.choice_label << " at " << wager.odds_numerator << ":"
         << wager.odds_denominator << " profit · reference " << wager.wager_id;
  if (wager.state == HouseWagerState::accepted_funded) {
    output << "\nYour selected stake is " << wager.stake
           << " Fate; potential profit is " << wager.profit
           << "; outcome deadline is UTC millisecond "
           << wager.outcome_due_at_ms << ".";
  } else if (wager.result) {
    output << "\nResult: " << house_result_name(*wager.result) << ".";
  }
  return text_message(output.str());
}

} // namespace

std::optional<std::string>
parse_tarot_house_component(const std::string_view custom_id) {
  if (!custom_id.starts_with(tarot_house_component_prefix))
    return std::nullopt;
  std::string token{custom_id.substr(tarot_house_component_prefix.size())};
  return valid_uuid_v4(token) ? std::optional<std::string>{std::move(token)}
                              : std::nullopt;
}

std::int64_t next_house_weekly_offer_ms(const std::int64_t now_ms,
                                        const std::string_view timezone) {
  using namespace std::chrono;
  if (now_ms < 0 || timezone.empty())
    throw std::invalid_argument{"House weekly schedule is invalid."};
  const auto *zone = locate_zone(std::string{timezone});
  const sys_time<milliseconds> now{milliseconds{now_ms}};
  const auto local_now = zoned_time{zone, now}.get_local_time();
  const auto local_day = floor<days>(local_now);
  const auto weekday_number = weekday{local_day}.c_encoding();
  const auto days_until_friday =
      static_cast<unsigned int>((5U + 7U - weekday_number) % 7U);
  auto candidate = local_day + days{days_until_friday} + hours{18};
  if (candidate <= local_now)
    candidate += days{7};
  return duration_cast<milliseconds>(zone->to_sys(candidate).time_since_epoch())
      .count();
}

HouseWeeklyBoundaries
house_weekly_boundaries_ms(const std::int64_t offer_slot_ms) {
  using namespace std::chrono;
  if (offer_slot_ms < 0)
    throw std::invalid_argument{"House weekly offer slot is invalid."};
  const auto *zone = locate_zone(std::string{tarot_house_timezone});
  const sys_time<milliseconds> slot{milliseconds{offer_slot_ms}};
  const auto local_slot = zoned_time{zone, slot}.get_local_time();
  const auto local_day = floor<days>(local_slot);
  const auto closes = local_day + days{2};
  const auto resolution_due = local_day + days{3} + minutes{15};
  return {
      .closes_at_ms =
          duration_cast<milliseconds>(zone->to_sys(closes).time_since_epoch())
              .count(),
      .resolution_due_at_ms =
          duration_cast<milliseconds>(
              zone->to_sys(resolution_due).time_since_epoch())
              .count(),
  };
}

void TarotHousePolicy::validate() const {
  if (draw_cooldown_ms < 60'000 || draw_cooldown_ms > 31 * 86'400'000LL ||
      exposure_cap < 1 || exposure_cap > 1'000'000 || profit_cap < 1 ||
      profit_cap > exposure_cap)
    throw std::invalid_argument{"Tarot House policy values are invalid."};
}

TarotDrawService::TarotDrawService(
    TarotDrawRepository &repository, const TarotDeckCatalog &deck,
    const Clock &clock, PersistentIdGenerator &ids, Random &random,
    TarotHousePolicy policy, ServerScopeConfiguration scope,
    const bool test_mode, Diagnostics &diagnostics,
    std::function<void(const TarotDrawRecord &)> observer)
    : repository_{repository}, deck_{deck}, clock_{clock}, ids_{ids},
      random_{random}, policy_{policy}, scope_{std::move(scope)},
      test_mode_{test_mode}, diagnostics_{diagnostics},
      observer_{std::move(observer)} {
  policy_.validate();
  if (deck_.version != emperor_tarot_catalog_version ||
      deck_.cards.size() != 22)
    throw std::invalid_argument{
        "Tarot draw service requires the curated v1 deck."};
}

TarotInvocation
TarotDrawService::invocation(const IncomingInteraction &interaction) const {
  return {.user_id = interaction.user_id,
          .guild_id = interaction.guild_id,
          .channel_id = interaction.channel_id,
          .display_name = interaction.display_name.empty()
                              ? interaction.username
                              : interaction.display_name,
          .interaction_idempotency_key =
              "tarot.draw:" + interaction.interaction_id.str(),
          .correlation_id = interaction.correlation_id,
          .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock_.now().time_since_epoch())
                        .count()};
}

InteractionMessage TarotDrawService::render(const TarotDrawRecord &draw,
                                            const bool replayed) const {
  if (draw.card_name.empty() || draw.card_meaning.empty() ||
      draw.flavor_text.empty())
    throw std::runtime_error{"Persisted Tarot presentation is invalid."};
  std::ostringstream output;
  if (draw.is_test)
    output << "[TEST] ";
  if (replayed)
    output << "Persisted draw replay\n";
  if (draw.visibility == TarotVisibility::public_result)
    output << "Public delivery is durably queued.\n";
  output << draw.card_name << "\n"
         << draw.card_meaning << "\n"
         << draw.flavor_text << "\nReference: " << draw.draw_id;
  auto message = text_message(output.str());
  message.allowed_user_mentions.clear();
  return message;
}

InteractionMessage
TarotDrawService::draw(const IncomingInteraction &interaction,
                       const bool bypass_cooldown) {
  if (bypass_cooldown &&
      (!test_mode_ || interaction.user_id != scope_.owner_user_id))
    throw std::invalid_argument{"Draw-test requires owner test mode."};
  const auto call = invocation(interaction);
  const auto selected_visibility =
      visibility(interaction, TarotVisibility::public_result);
  auto result = repository_.draw(
      {.invocation = call,
       .visibility = selected_visibility,
       .cooldown_ms = policy_.draw_cooldown_ms,
       .bypass_cooldown = bypass_cooldown,
       .is_test = bypass_cooldown,
       .draw_id = ids_.next_id(),
       .event_id = ids_.next_id(),
       .public_outbox_id = selected_visibility == TarotVisibility::public_result
                               ? ids_.next_id()
                               : std::string{},
       .sample = [this] {
         const auto card_index = random_.uniform(deck_.cards.size());
         const auto &card =
             deck_.cards.at(static_cast<std::size_t>(card_index));
         const auto variant = random_.uniform(card.flavor_variants.size());
         return std::pair{static_cast<std::int64_t>(card_index),
                          static_cast<std::int64_t>(variant)};
       }});
  if (result.status == TarotDrawStatus::cooldown)
    return text_message("The Tarot may be drawn again after UTC millisecond " +
                        std::to_string(result.cooldown_until_ms) + ".");
  if (!result.draw)
    throw std::runtime_error{"Tarot repository returned no persisted draw."};
  if (result.draw->visibility == TarotVisibility::public_result &&
      !result.public_delivery_created)
    throw std::runtime_error{
        "Public Tarot draw was not linked to durable delivery."};
  if (result.event_created && observer_) {
    try {
      observer_(*result.draw);
    } catch (const std::exception &error) {
      diagnostics_.emit(
          {DiagnosticSeverity::error, "tarot.draw_observer", error.what(), {}});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "tarot.draw_observer",
                         "Unknown Tarot draw observer failure.",
                         {}});
    }
  }
  return render(*result.draw, result.status == TarotDrawStatus::replay);
}

InteractionMessage
TarotDrawService::replay(const IncomingInteraction &interaction) {
  const auto reference = string_option(interaction, "reference");
  if (!reference)
    throw std::invalid_argument{"Draw replay requires a reference."};
  const auto found = repository_.find(*reference, interaction.user_id);
  if (!found)
    return text_message("That Tarot draw is unavailable.");
  return render(*found, true);
}

const char *house_result_name(const HouseResult result) noexcept {
  switch (result) {
  case HouseResult::win:
    return "win";
  case HouseResult::loss:
    return "loss";
  case HouseResult::void_wager:
    return "void";
  }
  return "unknown";
}

TarotHouseService::TarotHouseService(
    TarotHouseRepository &repository, TarotRepository &tarot_repository,
    const TarotHouseCatalog &catalog, const Clock &clock,
    PersistentIdGenerator &ids, TarotHousePolicy policy,
    TarotPolicy tarot_policy, ServerScopeConfiguration scope,
    const bool test_mode, Diagnostics &diagnostics,
    std::function<void(std::string_view)> observer)
    : repository_{repository}, tarot_repository_{tarot_repository},
      catalog_{catalog}, clock_{clock}, ids_{ids}, policy_{policy},
      tarot_policy_{tarot_policy}, scope_{std::move(scope)},
      test_mode_{test_mode}, diagnostics_{diagnostics},
      observer_{std::move(observer)} {
  policy_.validate();
  tarot_policy_.validate();
}

TarotInvocation
TarotHouseService::invocation(const IncomingInteraction &interaction) const {
  return {.user_id = interaction.user_id,
          .guild_id = interaction.guild_id,
          .channel_id = interaction.channel_id,
          .display_name = interaction.display_name.empty()
                              ? interaction.username
                              : interaction.display_name,
          .interaction_idempotency_key =
              "tarot.house:" + interaction.interaction_id.str(),
          .correlation_id = interaction.correlation_id,
          .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock_.now().time_since_epoch())
                        .count()};
}

std::function<std::string()> TarotHouseService::id_factory() {
  return [this] { return ids_.next_id(); };
}

InteractionMessage
TarotHouseService::offers(const IncomingInteraction &interaction) {
  if (!policy_.house_enabled)
    return text_message("House auguries are currently disabled.");
  const auto call = invocation(interaction);
  const bool is_test =
      test_mode_ && interaction.user_id == scope_.owner_user_id;
  std::ostringstream output;
  output << "House auguries";
  for (const auto &entry : catalog_.templates) {
    output << "\n" << entry.name << " — " << entry.proposition << " — stakes ";
    for (std::size_t index{}; index < entry.allowed_stakes.size(); ++index) {
      if (index != 0)
        output << "/";
      output << entry.allowed_stakes.at(index);
    }
    output << " — choices ";
    for (std::size_t index{}; index < entry.choices.size(); ++index) {
      if (index != 0)
        output << ", ";
      const auto &choice = entry.choices.at(index);
      output << choice.label << " (" << choice.profit_numerator << ":"
             << choice.profit_denominator << " profit)";
    }
    output << " — resolves within " << entry.outcome_window_ms / 3'600'000
           << "h";
    if (entry.recovery)
      output << " — free recovery reward " << entry.recovery_reward << " Fate";
    const auto status = repository_.availability(call, entry, is_test,
                                                 tarot_policy_.starting_fate);
    switch (status.status) {
    case HouseAvailabilityStatus::available:
      output << " — available";
      if (status.offer_id)
        output << " — offer " << *status.offer_id;
      break;
    case HouseAvailabilityStatus::ineligible:
      output << " — not currently eligible";
      break;
    case HouseAvailabilityStatus::cooldown:
      output << " — cooldown until UTC millisecond "
             << status.cooldown_until_ms;
      break;
    case HouseAvailabilityStatus::no_scheduled_offer:
      output << " — no active Friday offer";
      break;
    }
  }
  auto message = text_message(output.str());
  message.allowed_user_mentions.clear();
  return message;
}

InteractionMessage
TarotHouseService::play(const IncomingInteraction &interaction) {
  if (!policy_.house_enabled)
    return text_message("House auguries are currently disabled.");
  const auto template_slug = string_option(interaction, "template");
  const auto choice_slug = string_option(interaction, "choice");
  const auto stake = integer_option(interaction, "stake");
  if (!template_slug || !choice_slug || !stake)
    throw std::invalid_argument{
        "House play requires template, choice, and stake."};
  const auto &definition = house_template(catalog_, *template_slug);
  const auto offer_id = string_option(interaction, "offer");
  if (definition.scheduled && (!offer_id || !valid_uuid_v4(*offer_id)))
    return text_message(
        "That augury requires the reference from its scheduled offer.");
  if (!definition.scheduled && offer_id)
    throw std::invalid_argument{
        "On-demand House play cannot claim a scheduled offer."};

  const auto call = invocation(interaction);
  const auto provisioned = tarot_repository_.ensure_account(
      {.invocation = call,
       .starting_fate = tarot_policy_.starting_fate,
       .account_id = ids_.next_id(),
       .transaction_id = ids_.next_id(),
       .event_id = ids_.next_id(),
       .mint_posting_id = ids_.next_id(),
       .human_posting_id = ids_.next_id()});
  static_cast<void>(provisioned);
  auto result = repository_.play(
      {.invocation = call,
       .definition = &definition,
       .catalog_version = catalog_.version,
       .choice_slug = *choice_slug,
       .stake = *stake,
       .visibility = definition.scheduled ? TarotVisibility::public_result
                                          : visibility(interaction),
       .exposure_cap = policy_.exposure_cap,
       .profit_cap = policy_.profit_cap,
       .starting_fate = tarot_policy_.starting_fate,
       .is_test = test_mode_ && interaction.user_id == scope_.owner_user_id,
       .offer_id = offer_id,
       .next_id = id_factory()});
  observe(result);
  return house_result_message(result);
}

InteractionMessage
TarotHouseService::apply_component(const IncomingInteraction &interaction) {
  const auto token = parse_tarot_house_component(interaction.custom_id);
  if (!token)
    return text_message("That House control is invalid.");
  const auto result = repository_.inspect_control(
      {.invocation = invocation(interaction), .token_id = *token});
  switch (result.status) {
  case HouseControlStatus::wrong_user:
    return text_message("That sealed House control belongs to another user.");
  case HouseControlStatus::wrong_scope:
    return text_message("That House control is unavailable in this channel.");
  case HouseControlStatus::expired:
    return text_message("That House offer has expired.");
  case HouseControlStatus::unavailable:
    return text_message("That House offer has already been claimed or closed.");
  case HouseControlStatus::invalid_token:
    return text_message("That House control is invalid.");
  case HouseControlStatus::available:
    break;
  }
  return text_message(
      "The sealed Last Standard offer is available. Use `/tarot house play` "
      "with template `" +
      result.template_slug.value_or("last-standard") + "`, offer `" +
      result.offer_id.value_or("unavailable") +
      "`, choice `yes` or `no`, and stake 1, 5, or 10. Your balance, choice, "
      "stake, and result remain private.");
}

InteractionMessage
TarotHouseService::history(const IncomingInteraction &interaction) {
  const auto reference = string_option(interaction, "reference");
  const auto rows = repository_.history(
      interaction.user_id,
      reference ? std::optional<std::string_view>{*reference} : std::nullopt);
  std::ostringstream output;
  output << "Your House wager history";
  if (rows.empty())
    output << "\nNo visible House wagers were found.";
  for (const auto &row : rows) {
    output << "\n"
           << row.template_slug << " · " << house_state_name(row.state) << " · "
           << row.choice_label << " at " << row.odds_numerator << ":"
           << row.odds_denominator << " · stake " << row.stake << " · profit "
           << row.profit << " · " << row.wager_id;
    if (row.result)
      output << " · " << house_result_name(*row.result);
  }
  return text_message(output.str());
}

InteractionMessage
TarotHouseService::record(const IncomingInteraction &interaction) {
  const auto value = repository_.record(interaction.user_id);
  std::ostringstream output;
  output << "Your Tarot record\nWins " << value.wins << " · losses "
         << value.losses << "\nCurrent win streak " << value.current_win_streak
         << " · loss streak " << value.current_loss_streak
         << "\nSettled House wagers " << value.settled_house_wagers;
  if (!value.pending_titles.empty()) {
    output << "\nTitles awaiting owner approval:";
    for (const auto &title : value.pending_titles)
      output << "\n- " << title;
  }
  return text_message(output.str());
}

InteractionMessage
TarotHouseService::resolve(const IncomingInteraction &interaction) {
  if (interaction.user_id != scope_.owner_user_id)
    return text_message("Only the owner may resolve a manual House wager.");
  const auto reference = string_option(interaction, "reference");
  const auto outcome = string_option(interaction, "outcome");
  const auto reason = string_option(interaction, "reason");
  if (!reference || !outcome || !reason || blank(*reason) ||
      reason->size() > 200)
    throw std::invalid_argument{
        "A bounded reason is required for House resolution."};
  if (*outcome != "yes" && *outcome != "no" && *outcome != "void")
    throw std::invalid_argument{"Unknown House result."};
  const auto parsed =
      *outcome == "void" ? HouseResult::void_wager : HouseResult::loss;
  auto result = repository_.resolve({.invocation = invocation(interaction),
                                     .wager_id = *reference,
                                     .result = parsed,
                                     .observed_choice = *outcome,
                                     .reason = *reason,
                                     .automatic = false,
                                     .test_mode = test_mode_,
                                     .next_id = id_factory()});
  observe(result);
  return house_result_message(result);
}

InteractionMessage
TarotHouseService::deadline(const IncomingInteraction &interaction) {
  if (!test_mode_ || interaction.user_id != scope_.owner_user_id)
    return text_message("House deadline control requires owner test mode.");
  const auto results = repository_.resolve_due(invocation(interaction).now_ms,
                                               true, id_factory());
  for (const auto &result : results)
    observe(result);
  return text_message("[TEST] Processed " + std::to_string(results.size()) +
                      " due House wager(s).");
}

InteractionMessage
TarotHouseService::offer_test(const IncomingInteraction &interaction) {
  if (!test_mode_ || interaction.user_id != scope_.owner_user_id)
    return text_message("House offer control requires owner test mode.");
  const auto call = invocation(interaction);
  const ClaimedScheduledJob job{
      .job_id = ids_.next_id(),
      .job_type = std::string{tarot_house_weekly_offer_job_type},
      .lease_owner = "owner-test",
      .lease_token = ids_.next_id(),
      .attempt_count = 1,
      .max_attempts = 1,
      .due_at_ms = call.now_ms,
      .payload =
          TarotHouseWeeklyOfferJobPayload{.schedule_key =
                                              "friday-1800-america-new-york",
                                          .catalog_version = catalog_.version},
      .correlation_id = call.correlation_id,
      .causation_event_id = std::nullopt};
  const auto result = repository_.handle_weekly_offer(
      {.job = job,
       .definition = &house_template(catalog_, "last-standard"),
       .catalog_version = catalog_.version,
       .scope = scope_,
       .now_ms = call.now_ms,
       .exposure_cap = policy_.exposure_cap,
       .operational = true,
       .is_test = true,
       .next_id = id_factory()});
  if (result.outbox_created && observer_)
    observer_("tarot.house_offer_opened.v1");
  if (result.status == HouseWeeklyOfferStatus::skipped)
    return text_message("[TEST] The Last Standard offer was safely skipped.");
  if (result.status == HouseWeeklyOfferStatus::deferred)
    return text_message(
        "[TEST] The Last Standard offer remains deferred until its slot.");
  return text_message(std::string{"[TEST] Last Standard offer "} +
                      (result.status == HouseWeeklyOfferStatus::replay
                           ? "replayed: "
                           : "opened: ") +
                      result.offer_id.value_or("unavailable"));
}

InteractionMessage
TarotHouseService::cleanup_test(const IncomingInteraction &interaction) {
  if (!test_mode_ || interaction.user_id != scope_.owner_user_id)
    return text_message("House cleanup control requires owner test mode.");
  const auto reference = string_option(interaction, "reference");
  const auto reason = string_option(interaction, "reason");
  if (!reference || !valid_uuid_v4(*reference) || !reason || blank(*reason) ||
      reason->size() > 200)
    throw std::invalid_argument{"Test House cleanup request is invalid."};
  auto result =
      repository_.cleanup_test_wager({.invocation = invocation(interaction),
                                      .wager_id = *reference,
                                      .reason = *reason,
                                      .owner = true,
                                      .test_mode = true,
                                      .next_id = id_factory()});
  observe(result);
  return house_result_message(result);
}

InteractionMessage TarotHouseService::economy(const IncomingInteraction &) {
  const auto report = repository_.economy();
  std::ostringstream output;
  output << "Fate economy health: " << (report.valid ? "healthy" : "FAILED")
         << "\nIssued " << report.issued_fate << " · human holdings "
         << report.human_fate << " · House " << report.house_fate
         << " · account total " << report.account_total
         << "\nRecovery issuance " << report.recovery_issuance
         << "\nHouse exposure " << report.non_test_exposure << "/"
         << policy_.exposure_cap << " · test exposure " << report.test_exposure
         << "\nEscrow " << report.escrow_balance << " · expected House escrow "
         << report.expected_house_escrow << "\nOpen House wagers "
         << report.open_house_wagers << " · malformed links "
         << report.malformed_transfer_count << " · malformed offer deadlines "
         << report.malformed_offer_deadline_count;
  return text_message(output.str());
}

void TarotHouseService::handle_deadline(const ClaimedScheduledJob &job) {
  const auto result = repository_.handle_deadline(
      {.job = job,
       .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     clock_.now().time_since_epoch())
                     .count(),
       .next_id = id_factory()});
  observe(result);
}

void TarotHouseService::handle_offer_expiry(const ClaimedScheduledJob &job) {
  static_cast<void>(repository_.handle_offer_expiry(
      {.job = job,
       .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     clock_.now().time_since_epoch())
                     .count(),
       .next_id = id_factory()}));
}

void TarotHouseService::ensure_weekly_schedule() {
  if (!policy_.house_enabled)
    return;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       clock_.now().time_since_epoch())
                       .count();
  repository_.ensure_weekly_schedule(
      now, next_house_weekly_offer_ms(now, tarot_house_timezone),
      catalog_.version, ids_.next_id());
}

HouseWeeklyOfferResult
TarotHouseService::handle_weekly_offer(const ClaimedScheduledJob &job,
                                       const bool operational) {
  const auto *payload =
      std::get_if<TarotHouseWeeklyOfferJobPayload>(&job.payload);
  if (payload == nullptr || job.job_type != tarot_house_weekly_offer_job_type ||
      payload->schedule_key != "friday-1800-america-new-york" ||
      payload->catalog_version != catalog_.version)
    throw std::invalid_argument{"Invalid House weekly-offer payload."};
  return repository_.handle_weekly_offer(
      {.job = job,
       .definition = &house_template(catalog_, "last-standard"),
       .catalog_version = catalog_.version,
       .scope = scope_,
       .now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     clock_.now().time_since_epoch())
                     .count(),
       .exposure_cap = policy_.exposure_cap,
       .operational = operational,
       .is_test = false,
       .next_id = id_factory()});
}

void TarotHouseService::observe_draw(const TarotDrawRecord &draw) noexcept {
  try {
    const auto results = repository_.observe_draw(
        draw,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock_.now().time_since_epoch())
            .count(),
        id_factory());
    for (const auto &result : results)
      observe(result);
  } catch (const std::exception &error) {
    diagnostics_.emit(
        {DiagnosticSeverity::error, "tarot.house_observer", error.what(), {}});
  } catch (...) {
    diagnostics_.emit({DiagnosticSeverity::error,
                       "tarot.house_observer",
                       "Unknown House draw-observer failure.",
                       {}});
  }
}

void TarotHouseService::reconcile_draws() {
  const auto results = repository_.reconcile_draws(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          clock_.now().time_since_epoch())
          .count(),
      id_factory());
  for (const auto &result : results)
    observe(result);
}

HouseEconomyReport TarotHouseService::check_invariants() {
  return repository_.economy();
}

void TarotHouseService::observe(
    const HouseMutationResult &result) const noexcept {
  if (!observer_)
    return;
  for (const auto &event_type : result.event_types) {
    try {
      observer_(event_type);
    } catch (const std::exception &error) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "tarot.house_integration",
                         error.what(),
                         {}});
    } catch (...) {
      diagnostics_.emit({DiagnosticSeverity::error,
                         "tarot.house_integration",
                         "Unknown House integration observer failure.",
                         {}});
    }
  }
}

} // namespace sanguinius
