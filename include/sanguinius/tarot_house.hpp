#pragma once

#include "sanguinius/clock.hpp"
#include "sanguinius/diagnostics.hpp"
#include "sanguinius/discord_types.hpp"
#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistent_id.hpp"
#include "sanguinius/random.hpp"
#include "sanguinius/server_scope_policy.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/tarot_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sanguinius {

inline constexpr std::int64_t normal_tarot_draw_cooldown_ms = 86'400'000;
inline constexpr std::int64_t default_house_exposure_cap = 100;
inline constexpr std::int64_t default_house_profit_cap = 20;
inline constexpr std::string_view tarot_house_component_prefix{"sgh:1:"};
inline constexpr std::string_view tarot_house_timezone{"America/New_York"};

struct TarotHousePolicy {
  bool house_enabled{true};
  bool integration_enabled{true};
  std::int64_t draw_cooldown_ms{normal_tarot_draw_cooldown_ms};
  std::int64_t exposure_cap{default_house_exposure_cap};
  std::int64_t profit_cap{default_house_profit_cap};

  void validate() const;
};

class TarotCatalogRepository {
public:
  virtual ~TarotCatalogRepository() = default;
  virtual void install(const TarotDeckCatalog &deck,
                       const TarotHouseCatalog &house,
                       std::int64_t installed_at_ms) = 0;
};

enum class TarotDrawStatus { drawn, replay, cooldown };

struct TarotDrawRecord {
  std::string draw_id;
  DiscordSnowflake user_id;
  TarotVisibility visibility{TarotVisibility::public_result};
  std::string catalog_version;
  std::int64_t card_ordinal{};
  std::int64_t flavor_variant{};
  std::int64_t drawn_at_ms{};
  std::int64_t cooldown_until_ms{};
  bool is_test{};
  std::string card_name{};
  std::string card_meaning{};
  std::string flavor_text{};
};

struct TarotDrawRequest {
  TarotInvocation invocation;
  TarotVisibility visibility{TarotVisibility::public_result};
  std::int64_t cooldown_ms{};
  bool bypass_cooldown{};
  bool is_test{};
  std::string draw_id;
  std::string event_id;
  std::string public_outbox_id{};
  std::function<std::pair<std::int64_t, std::int64_t>()> sample;
};

struct TarotDrawResult {
  TarotDrawStatus status{TarotDrawStatus::cooldown};
  std::optional<TarotDrawRecord> draw;
  std::int64_t cooldown_until_ms{};
  bool event_created{};
  bool public_delivery_created{};
};

class TarotDrawRepository {
public:
  virtual ~TarotDrawRepository() = default;
  [[nodiscard]] virtual TarotDrawResult
  draw(const TarotDrawRequest &request) = 0;
  [[nodiscard]] virtual std::optional<TarotDrawRecord>
  find(std::string_view draw_id, const DiscordSnowflake &requester) = 0;
};

class TarotDrawService {
public:
  TarotDrawService(TarotDrawRepository &repository,
                   const TarotDeckCatalog &deck, const Clock &clock,
                   PersistentIdGenerator &ids, Random &random,
                   TarotHousePolicy policy, ServerScopeConfiguration scope,
                   bool test_mode, Diagnostics &diagnostics,
                   std::function<void(const TarotDrawRecord &)> observer = {});

  [[nodiscard]] InteractionMessage draw(const IncomingInteraction &interaction,
                                        bool bypass_cooldown = false);
  [[nodiscard]] InteractionMessage
  replay(const IncomingInteraction &interaction);

private:
  [[nodiscard]] TarotInvocation
  invocation(const IncomingInteraction &interaction) const;
  [[nodiscard]] InteractionMessage render(const TarotDrawRecord &draw,
                                          bool replay) const;

  TarotDrawRepository &repository_;
  const TarotDeckCatalog &deck_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  Random &random_;
  TarotHousePolicy policy_;
  ServerScopeConfiguration scope_;
  bool test_mode_{};
  Diagnostics &diagnostics_;
  std::function<void(const TarotDrawRecord &)> observer_;
};

enum class HouseWagerState { accepted_funded, resolved, void_refunded };
enum class HouseResult { win, loss, void_wager };
enum class HouseMutationStatus {
  applied,
  replay,
  not_found,
  forbidden,
  ineligible,
  cooldown,
  insufficient_funds,
  exposure_blocked,
  invalid_state,
};

struct HouseWagerRecord {
  std::string wager_id;
  DiscordSnowflake user_id;
  std::string template_slug;
  std::string catalog_version;
  std::string proposition;
  std::string choice_slug;
  std::string choice_label;
  std::int64_t odds_numerator{};
  std::int64_t odds_denominator{};
  std::int64_t stake{};
  std::int64_t profit{};
  TarotVisibility visibility{TarotVisibility::private_result};
  HouseResolutionAuthority authority{HouseResolutionAuthority::draw};
  HouseWagerState state{HouseWagerState::accepted_funded};
  std::optional<HouseResult> result;
  std::int64_t accepted_at_ms{};
  std::int64_t outcome_due_at_ms{};
  std::int64_t terminal_cooldown_ms{};
  bool recovery{};
  bool is_test{};
};

struct HousePlayRequest {
  TarotInvocation invocation;
  const HouseTemplateDefinition *definition{};
  std::string catalog_version;
  std::string choice_slug;
  std::int64_t stake{};
  TarotVisibility visibility{TarotVisibility::private_result};
  std::int64_t exposure_cap{};
  std::int64_t profit_cap{};
  std::int64_t starting_fate{};
  bool is_test{};
  std::optional<std::string> offer_id;
  std::function<std::string()> next_id;
};

struct HouseResolveRequest {
  TarotInvocation invocation;
  std::string wager_id;
  HouseResult result{HouseResult::void_wager};
  std::optional<std::string> observed_choice;
  std::string reason;
  bool automatic{};
  bool test_mode{};
  std::function<std::string()> next_id;
};

struct HouseDeadlineRequest {
  ClaimedScheduledJob job;
  std::int64_t now_ms{};
  std::function<std::string()> next_id;
};

struct HouseTestCleanupRequest {
  TarotInvocation invocation;
  std::string wager_id;
  std::string reason;
  bool owner{};
  bool test_mode{};
  std::function<std::string()> next_id;
};

struct HouseMutationResult {
  HouseMutationStatus status{HouseMutationStatus::not_found};
  std::optional<HouseWagerRecord> wager;
  std::vector<std::string> event_types;
};

struct TarotPlayerRecord {
  std::size_t wins{};
  std::size_t losses{};
  std::size_t current_win_streak{};
  std::size_t current_loss_streak{};
  std::size_t settled_house_wagers{};
  std::vector<std::string> pending_titles;
};

enum class HouseAvailabilityStatus {
  available,
  ineligible,
  cooldown,
  no_scheduled_offer,
};

struct HouseAvailability {
  HouseAvailabilityStatus status{HouseAvailabilityStatus::available};
  std::optional<std::string> offer_id;
  std::int64_t cooldown_until_ms{};
};

struct TarotPlayerProjectionReport {
  bool valid{true};
  std::size_t event_count{};
  std::size_t projection_count{};
  std::size_t mismatch_count{};
};

struct HouseEconomyReport {
  bool valid{true};
  std::int64_t issued_fate{};
  std::int64_t account_total{};
  std::int64_t human_fate{};
  std::int64_t house_fate{};
  std::int64_t recovery_issuance{};
  std::int64_t non_test_exposure{};
  std::int64_t test_exposure{};
  std::int64_t escrow_balance{};
  std::int64_t expected_house_escrow{};
  std::size_t open_house_wagers{};
  std::size_t malformed_transfer_count{};
  std::size_t malformed_offer_deadline_count{};
};

enum class HouseWeeklyOfferStatus { created, replay, skipped, deferred };

struct HouseWeeklyOfferRequest {
  ClaimedScheduledJob job;
  const HouseTemplateDefinition *definition{};
  std::string catalog_version;
  ServerScopeConfiguration scope;
  std::int64_t now_ms{};
  std::int64_t exposure_cap{};
  bool operational{};
  bool is_test{};
  std::function<std::string()> next_id;
};

struct HouseWeeklyOfferResult {
  HouseWeeklyOfferStatus status{HouseWeeklyOfferStatus::skipped};
  std::optional<std::string> offer_id;
  bool outbox_created{};
};

enum class HouseOfferExpiryStatus { expired, replay };

struct HouseOfferExpiryRequest {
  ClaimedScheduledJob job;
  std::int64_t now_ms{};
  std::function<std::string()> next_id;
};

struct HouseOfferExpiryResult {
  HouseOfferExpiryStatus status{HouseOfferExpiryStatus::replay};
  std::string offer_id;
};

enum class HouseControlStatus {
  available,
  invalid_token,
  wrong_user,
  wrong_scope,
  expired,
  unavailable,
};

struct HouseControlRequest {
  TarotInvocation invocation;
  std::string token_id;
};

struct HouseControlResult {
  HouseControlStatus status{HouseControlStatus::invalid_token};
  std::optional<std::string> offer_id;
  std::optional<std::string> template_slug;
  std::int64_t closes_at_ms{};
};

class TarotHouseRepository {
public:
  virtual ~TarotHouseRepository() = default;
  [[nodiscard]] virtual HouseMutationResult
  play(const HousePlayRequest &request) = 0;
  [[nodiscard]] virtual HouseMutationResult
  resolve(const HouseResolveRequest &request) = 0;
  [[nodiscard]] virtual std::vector<HouseMutationResult>
  observe_draw(const TarotDrawRecord &draw, std::int64_t now_ms,
               std::function<std::string()> next_id) = 0;
  [[nodiscard]] virtual std::vector<HouseMutationResult>
  reconcile_draws(std::int64_t now_ms,
                  std::function<std::string()> next_id) = 0;
  [[nodiscard]] virtual std::vector<HouseMutationResult>
  resolve_due(std::int64_t now_ms, bool test_only,
              std::function<std::string()> next_id) = 0;
  [[nodiscard]] virtual HouseMutationResult
  handle_deadline(const HouseDeadlineRequest &request) = 0;
  [[nodiscard]] virtual HouseMutationResult
  cleanup_test_wager(const HouseTestCleanupRequest &request) = 0;
  virtual void ensure_weekly_schedule(std::int64_t now_ms,
                                      std::int64_t due_at_ms,
                                      std::string catalog_version,
                                      std::string job_id) = 0;
  [[nodiscard]] virtual HouseWeeklyOfferResult
  handle_weekly_offer(const HouseWeeklyOfferRequest &request) = 0;
  [[nodiscard]] virtual HouseOfferExpiryResult
  handle_offer_expiry(const HouseOfferExpiryRequest &request) = 0;
  [[nodiscard]] virtual HouseControlResult
  inspect_control(const HouseControlRequest &request) = 0;
  [[nodiscard]] virtual HouseAvailability
  availability(const TarotInvocation &invocation,
               const HouseTemplateDefinition &definition, bool is_test,
               std::int64_t starting_fate) = 0;
  [[nodiscard]] virtual std::vector<HouseWagerRecord>
  history(const DiscordSnowflake &user_id,
          std::optional<std::string_view> reference) = 0;
  [[nodiscard]] virtual TarotPlayerRecord
  record(const DiscordSnowflake &user_id) = 0;
  [[nodiscard]] virtual HouseEconomyReport economy() = 0;
  [[nodiscard]] virtual TarotPlayerProjectionReport
  check_player_projection() = 0;
  [[nodiscard]] virtual TarotPlayerProjectionReport
  rebuild_player_projection() = 0;
};

class TarotHouseService {
public:
  TarotHouseService(TarotHouseRepository &repository,
                    TarotRepository &tarot_repository,
                    const TarotHouseCatalog &catalog, const Clock &clock,
                    PersistentIdGenerator &ids, TarotHousePolicy policy,
                    TarotPolicy tarot_policy, ServerScopeConfiguration scope,
                    bool test_mode, Diagnostics &diagnostics,
                    std::function<void(std::string_view)> observer = {});

  [[nodiscard]] InteractionMessage
  offers(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage play(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  apply_component(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  history(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  record(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  resolve(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  deadline(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  offer_test(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  cleanup_test(const IncomingInteraction &interaction);
  [[nodiscard]] InteractionMessage
  economy(const IncomingInteraction &interaction);
  void handle_deadline(const ClaimedScheduledJob &job);
  void ensure_weekly_schedule();
  [[nodiscard]] HouseWeeklyOfferResult
  handle_weekly_offer(const ClaimedScheduledJob &job, bool operational);
  void handle_offer_expiry(const ClaimedScheduledJob &job);
  void observe_draw(const TarotDrawRecord &draw) noexcept;
  void reconcile_draws();
  [[nodiscard]] HouseEconomyReport check_invariants();

private:
  [[nodiscard]] TarotInvocation
  invocation(const IncomingInteraction &interaction) const;
  [[nodiscard]] std::function<std::string()> id_factory();
  void observe(const HouseMutationResult &result) const noexcept;

  TarotHouseRepository &repository_;
  TarotRepository &tarot_repository_;
  const TarotHouseCatalog &catalog_;
  const Clock &clock_;
  PersistentIdGenerator &ids_;
  TarotHousePolicy policy_;
  TarotPolicy tarot_policy_;
  ServerScopeConfiguration scope_;
  bool test_mode_{};
  Diagnostics &diagnostics_;
  std::function<void(std::string_view)> observer_;
};

[[nodiscard]] std::int64_t
next_house_weekly_offer_ms(std::int64_t now_ms, std::string_view timezone);

struct HouseWeeklyBoundaries {
  std::int64_t closes_at_ms{};
  std::int64_t resolution_due_at_ms{};
};

[[nodiscard]] HouseWeeklyBoundaries
house_weekly_boundaries_ms(std::int64_t offer_slot_ms);

[[nodiscard]] const char *house_result_name(HouseResult result) noexcept;
[[nodiscard]] std::optional<std::string>
parse_tarot_house_component(std::string_view custom_id);

} // namespace sanguinius
