#pragma once

#include "sanguinius/tarot_house.hpp"
#include "sanguinius/tarot_integration.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sanguinius::test {

class FakeTarotCatalogRepository final : public TarotCatalogRepository {
public:
  void install(const TarotDeckCatalog &deck, const TarotHouseCatalog &house,
               const std::int64_t installed_at_ms) override {
    ++install_calls;
    installed_deck_version = deck.version;
    installed_house_version = house.version;
    last_installed_at_ms = installed_at_ms;
  }

  std::size_t install_calls{};
  std::string installed_deck_version;
  std::string installed_house_version;
  std::int64_t last_installed_at_ms{};
};

class FakeTarotDrawRepository final : public TarotDrawRepository {
public:
  [[nodiscard]] TarotDrawResult draw(const TarotDrawRequest &request) override {
    ++draw_calls;
    last_visibility = request.visibility;
    last_bypass_cooldown = request.bypass_cooldown;
    last_is_test = request.is_test;
    const auto receipt =
        receipts.find(request.invocation.interaction_idempotency_key);
    if (receipt != receipts.end()) {
      auto replay = receipt->second;
      replay.status = TarotDrawStatus::replay;
      replay.event_created = false;
      return replay;
    }
    const auto [card, flavor] = request.sample();
    TarotDrawRecord record{
        .draw_id = request.draw_id,
        .user_id = request.invocation.user_id,
        .visibility = request.visibility,
        .catalog_version = std::string{emperor_tarot_catalog_version},
        .card_ordinal = card,
        .flavor_variant = flavor,
        .drawn_at_ms = request.invocation.now_ms,
        .cooldown_until_ms = request.invocation.now_ms + request.cooldown_ms,
        .is_test = request.is_test,
        .card_name = "The Broken Wing",
        .card_meaning = "Endurance without denial.",
        .flavor_text = "The cast settles without reversal."};
    TarotDrawResult result{.status = TarotDrawStatus::drawn,
                           .draw = record,
                           .cooldown_until_ms = record.cooldown_until_ms,
                           .event_created = true,
                           .public_delivery_created =
                               request.visibility ==
                               TarotVisibility::public_result};
    receipts.emplace(request.invocation.interaction_idempotency_key, result);
    draws.emplace(record.draw_id, std::move(record));
    return result;
  }

  [[nodiscard]] std::optional<TarotDrawRecord>
  find(const std::string_view draw_id,
       const DiscordSnowflake &requester) override {
    const auto found = draws.find(std::string{draw_id});
    if (found == draws.end() || found->second.user_id != requester)
      return std::nullopt;
    return found->second;
  }

  std::size_t draw_calls{};
  TarotVisibility last_visibility{TarotVisibility::private_result};
  bool last_bypass_cooldown{};
  bool last_is_test{};
  std::unordered_map<std::string, TarotDrawResult> receipts;
  std::unordered_map<std::string, TarotDrawRecord> draws;
};

class FakeTarotHouseRepository final : public TarotHouseRepository {
public:
  [[nodiscard]] HouseMutationResult
  play(const HousePlayRequest &request) override {
    ++play_calls;
    last_template =
        request.definition == nullptr ? "" : request.definition->slug;
    last_choice = request.choice_slug;
    last_stake = request.stake;
    if (play_result.status == HouseMutationStatus::not_found) {
      play_result.status = HouseMutationStatus::applied;
      play_result.wager = HouseWagerRecord{
          .wager_id = request.next_id(),
          .user_id = request.invocation.user_id,
          .template_slug = request.definition->slug,
          .catalog_version = request.catalog_version,
          .proposition = request.definition->proposition,
          .choice_slug = request.choice_slug,
          .choice_label = request.choice_slug,
          .odds_numerator = 1,
          .odds_denominator = 1,
          .stake = request.stake,
          .profit = request.stake,
          .visibility = request.visibility,
          .authority = request.definition->authority,
          .state = HouseWagerState::accepted_funded,
          .result = std::nullopt,
          .accepted_at_ms = request.invocation.now_ms,
          .outcome_due_at_ms =
              request.invocation.now_ms + request.definition->outcome_window_ms,
          .terminal_cooldown_ms = request.definition->terminal_cooldown_ms,
          .recovery = request.definition->recovery,
          .is_test = request.is_test};
    }
    return play_result;
  }

  [[nodiscard]] HouseMutationResult
  resolve(const HouseResolveRequest &request) override {
    ++resolve_calls;
    last_resolve_test_mode = request.test_mode;
    return resolve_result;
  }
  [[nodiscard]] std::vector<HouseMutationResult>
  observe_draw(const TarotDrawRecord &, std::int64_t,
               std::function<std::string()>) override {
    ++observe_draw_calls;
    return observed_results;
  }
  [[nodiscard]] std::vector<HouseMutationResult>
  reconcile_draws(std::int64_t, std::function<std::string()>,
                  std::size_t limit) override {
    ++reconcile_calls;
    return {reconciled_results.begin(),
            reconciled_results.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(limit, reconciled_results.size()))};
  }
  [[nodiscard]] std::vector<HouseMutationResult>
  resolve_due(std::int64_t, bool, std::function<std::string()>) override {
    ++resolve_due_calls;
    return due_results;
  }
  [[nodiscard]] HouseMutationResult
  handle_deadline(const HouseDeadlineRequest &) override {
    ++deadline_calls;
    return deadline_result;
  }
  [[nodiscard]] HouseMutationResult
  cleanup_test_wager(const HouseTestCleanupRequest &) override {
    ++cleanup_calls;
    return cleanup_result;
  }
  void ensure_weekly_schedule(std::int64_t, std::int64_t,
                              std::string catalog_version,
                              std::string) override {
    ++schedule_calls;
    scheduled_catalog_version = std::move(catalog_version);
  }
  [[nodiscard]] HouseWeeklyOfferResult
  handle_weekly_offer(const HouseWeeklyOfferRequest &) override {
    ++weekly_offer_calls;
    if (weekly_offer_hook)
      weekly_offer_hook();
    return weekly_offer_result;
  }
  [[nodiscard]] HouseOfferExpiryResult
  handle_offer_expiry(const HouseOfferExpiryRequest &) override {
    ++offer_expiry_calls;
    return offer_expiry_result;
  }
  [[nodiscard]] HouseControlResult
  inspect_control(const HouseControlRequest &) override {
    ++control_calls;
    return control_result;
  }
  [[nodiscard]] HouseAvailability availability(const TarotInvocation &,
                                               const HouseTemplateDefinition &,
                                               bool, std::int64_t) override {
    ++availability_calls;
    return availability_result;
  }
  [[nodiscard]] std::vector<HouseWagerRecord>
  history(const DiscordSnowflake &, std::optional<std::string_view>) override {
    ++history_calls;
    return history_result;
  }
  [[nodiscard]] HouseHistoryPage begin_history(const DiscordSnowflake &,
                                               std::string cursor_id,
                                               std::int64_t) override {
    ++history_calls;
    return history_page(std::move(cursor_id), 0);
  }
  [[nodiscard]] HouseHistoryPage load_history_page(const DiscordSnowflake &,
                                                   std::string_view cursor_id,
                                                   std::size_t page,
                                                   std::int64_t) override {
    ++history_calls;
    return history_page(std::string{cursor_id}, page);
  }
  [[nodiscard]] TarotPlayerRecord record(const DiscordSnowflake &) override {
    ++record_calls;
    return player_record;
  }
  [[nodiscard]] HouseEconomyReport economy() override {
    ++economy_calls;
    return economy_result;
  }
  [[nodiscard]] TarotPlayerProjectionReport check_player_projection() override {
    return projection_result;
  }
  [[nodiscard]] TarotPlayerProjectionReport
  rebuild_player_projection() override {
    ++projection_rebuild_calls;
    return projection_result;
  }

  std::size_t play_calls{};
  std::size_t resolve_calls{};
  std::size_t observe_draw_calls{};
  std::atomic_size_t reconcile_calls{};
  std::size_t resolve_due_calls{};
  std::size_t deadline_calls{};
  std::size_t cleanup_calls{};
  std::size_t schedule_calls{};
  std::size_t weekly_offer_calls{};
  std::size_t offer_expiry_calls{};
  std::size_t control_calls{};
  std::size_t availability_calls{};
  std::size_t history_calls{};
  std::size_t record_calls{};
  std::size_t economy_calls{};
  std::size_t projection_rebuild_calls{};
  std::function<void()> weekly_offer_hook;
  std::string last_template;
  std::string last_choice;
  std::string scheduled_catalog_version;
  std::int64_t last_stake{};
  bool last_resolve_test_mode{};
  HouseMutationResult play_result;
  HouseMutationResult resolve_result;
  HouseMutationResult deadline_result;
  HouseMutationResult cleanup_result;
  std::vector<HouseMutationResult> observed_results;
  std::vector<HouseMutationResult> reconciled_results;
  std::vector<HouseMutationResult> due_results;
  HouseWeeklyOfferResult weekly_offer_result;
  HouseOfferExpiryResult offer_expiry_result;
  HouseControlResult control_result;
  HouseAvailability availability_result;
  std::vector<HouseWagerRecord> history_result;
  TarotPlayerRecord player_record;
  HouseEconomyReport economy_result;
  TarotPlayerProjectionReport projection_result;

private:
  [[nodiscard]] HouseHistoryPage history_page(std::string cursor_id,
                                              const std::size_t page) const {
    const auto first =
        std::min(page * tarot_house_history_page_size, history_result.size());
    const auto last =
        std::min(first + tarot_house_history_page_size, history_result.size());
    return {.cursor_id = std::move(cursor_id),
            .page = page,
            .total = history_result.size(),
            .wagers = std::vector<HouseWagerRecord>(
                history_result.begin() + static_cast<std::ptrdiff_t>(first),
                history_result.begin() + static_cast<std::ptrdiff_t>(last))};
  }
};

class FakeTarotIntegrationRepository final : public TarotIntegrationRepository {
public:
  void ensure_schedule(std::int64_t, std::string) override { ++schedule_calls; }
  [[nodiscard]] TarotIntegrationReport
  scan(std::int64_t, std::size_t, std::function<std::string()>,
       const TarotIntegrationSinkPolicy sink_policy = {}) override {
    last_sink_policy = sink_policy;
    ++scan_calls;
    return report;
  }
  [[nodiscard]] std::size_t suppress_disabled(std::int64_t,
                                              std::size_t) override {
    ++suppress_disabled_calls;
    return suppress_disabled_result;
  }
  [[nodiscard]] bool retry(const std::string_view source_event_id,
                           std::int64_t) override {
    ++retry_calls;
    last_retry_reference = source_event_id;
    return retry_result;
  }
  [[nodiscard]] TarotIntegrationReport inspect() override { return report; }

  std::size_t schedule_calls{};
  std::atomic_size_t scan_calls{};
  std::atomic_size_t suppress_disabled_calls{};
  std::size_t suppress_disabled_result{};
  std::size_t retry_calls{};
  std::string last_retry_reference;
  bool retry_result{};
  TarotIntegrationSinkPolicy last_sink_policy;
  TarotIntegrationReport report;
};

} // namespace sanguinius::test
