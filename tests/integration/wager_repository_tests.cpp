#include "sanguinius/outbox.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"
#include "sanguinius/tarot.hpp"
#include "sanguinius/wagers.hpp"

#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_discord.hpp"
#include "support/fake_id_generator.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace sanguinius;
using persistence::Database;
using persistence::Migrator;
using persistence::SqliteCoreIdentityRepository;
using persistence::SqliteDurableWorkRepository;
using persistence::SqlitePendingNoticeRepository;
using persistence::SqliteRepositoryContext;
using persistence::SqliteTarotRepository;
using persistence::SqliteWagerRepository;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

[[nodiscard]] std::string token_from(const std::string &custom_id,
                                     const std::string_view prefix) {
  REQUIRE(custom_id.starts_with(prefix));
  return custom_id.substr(prefix.size());
}

[[nodiscard]] std::int64_t scalar(SqliteRepositoryContext &context,
                                  const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_int64(0);
}

[[nodiscard]] std::string text_scalar(SqliteRepositoryContext &context,
                                      const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_text(0);
}

class WagerFixture {
public:
  WagerFixture() {
    {
      auto database = Database::open_migration(temporary.path());
      const Migrator migrator{
          persistence::production_migrations(), {"test", "revision"}, clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 12);
    }
    open_runtime();
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({guild, channel, owner}, 100);
    identities.ensure_user({target, "Target", "target", false, 100});
    identities.ensure_user({judge, "Judge", "judge", false, 100});
    tarot = std::make_unique<SqliteTarotRepository>(context);
    tarot->initialize_system_accounts({next(), next(), next(), next()}, 100);
    provision(owner, 100, "provision-owner");
    wagers = std::make_unique<SqliteWagerRepository>(context);
  }

  [[nodiscard]] std::string next() { return uuid(next_id++); }
  [[nodiscard]] WagerIdFactory ids() {
    return [this] { return next(); };
  }

  [[nodiscard]] WagerInvocation call(const DiscordSnowflake user,
                                     std::string key, const std::int64_t now_ms,
                                     const bool test_mode = false) const {
    return {.user_id = user,
            .guild_id = guild,
            .channel_id = channel,
            .interaction_idempotency_key = std::move(key),
            .correlation_id = "wager-test",
            .now_ms = now_ms,
            .owner = user == owner,
            .test_mode = test_mode};
  }

  void provision(const DiscordSnowflake user, const std::int64_t starting_fate,
                 const std::string &key) {
    const auto result = tarot->ensure_account(TarotAccountProvisionRequest{
        .invocation = {.user_id = user,
                       .guild_id = guild,
                       .channel_id = channel,
                       .display_name = user == owner ? "Owner" : "Target",
                       .interaction_idempotency_key = key,
                       .correlation_id = "wager-test",
                       .now_ms = 110},
        .starting_fate = starting_fate,
        .account_id = next(),
        .transaction_id = next(),
        .event_id = next(),
        .mint_posting_id = next(),
        .human_posting_id = next()});
    REQUIRE(result.balance == starting_fate);
  }

  [[nodiscard]] WagerMutationResult
  offer(const std::int64_t stake = 10,
        const WagerVisibility visibility = WagerVisibility::public_offer,
        const WagerResolutionPolicy resolution = WagerResolutionPolicy::mutual,
        const std::optional<DiscordSnowflake> designated_judge = std::nullopt,
        const std::int64_t now_ms = 1'000,
        std::string proposition = "The Blood Angels win the match") {
    auto draft = wagers->create_draft(WagerCreateRequest{
        .invocation = call(owner, "draft:" + std::to_string(next_id), now_ms),
        .target_user_id = target,
        .judge_user_id = designated_judge,
        .visibility = visibility,
        .resolution_policy = resolution,
        .outcome_window_ms = 3'600'000,
        .resolution_grace_ms = 172'800'000,
        .draft_expires_at_ms = now_ms + 900'000,
        .is_test = false,
        .next_id = ids()});
    REQUIRE(draft.status == WagerMutationStatus::applied);
    REQUIRE(draft.wager.has_value());
    REQUIRE(draft.controls.size() == 1);
    const auto form_token =
        token_from(draft.controls.front().custom_id, wager_form_prefix);
    auto preview = wagers->preview(WagerPreviewRequest{
        .invocation =
            call(owner, "preview:" + draft.wager->wager_id, now_ms + 10),
        .token_id = form_token,
        .proposition = std::move(proposition),
        .stake = stake,
        .evidence_instructions = "Record the final score",
        .offer_expiry_ms = 86'400'000,
        .next_id = ids()});
    REQUIRE(preview.status == WagerMutationStatus::applied);
    REQUIRE(preview.controls.size() == 2);
    auto confirmed = wagers->act(WagerActionRequest{
        .invocation =
            call(owner, "confirm:" + draft.wager->wager_id, now_ms + 20),
        .wager_id = draft.wager->wager_id,
        .token_id = std::nullopt,
        .action = WagerAction::confirm,
        .starting_fate = 100,
        .offer_expiry_ms = 86'400'000,
        .resolution_grace_ms = 172'800'000,
        .next_id = ids()});
    REQUIRE(confirmed.status == WagerMutationStatus::applied);
    REQUIRE(confirmed.wager->state == WagerState::offered);
    return confirmed;
  }

  [[nodiscard]] WagerMutationResult accept(const std::string &wager_id,
                                           const std::string &key,
                                           const std::int64_t now_ms,
                                           const std::int64_t starting = 100) {
    return wagers->act(
        WagerActionRequest{.invocation = call(target, key, now_ms),
                           .wager_id = wager_id,
                           .token_id = std::nullopt,
                           .action = WagerAction::accept,
                           .starting_fate = starting,
                           .offer_expiry_ms = 86'400'000,
                           .resolution_grace_ms = 172'800'000,
                           .next_id = ids()});
  }

  void deliver_sealed_offer(const std::string &wager_id,
                            const DiscordSnowflake user = target,
                            const std::string &key = "deliver-sealed-offer",
                            const std::int64_t now_ms = 1'090) {
    const auto token_id = text_scalar(
        *context,
        "SELECT token.token_id FROM tarot_wager_notice link "
        "JOIN interaction_token token ON token.entity_type='pending_notice' "
        "AND token.entity_id=link.notice_id WHERE link.wager_id='" +
            wager_id + "' AND link.purpose='sealed_offer'");
    SqlitePendingNoticeRepository notices{context};
    const auto opened =
        notices.open_by_token({.token_id = token_id,
                               .interaction_kind = InteractionTokenKind::button,
                               .guild_id = guild,
                               .channel_id = channel,
                               .user_id = user,
                               .interaction_idempotency_key = key,
                               .now_ms = now_ms});
    REQUIRE(opened.status == OpenPendingNoticeStatus::opened);
    REQUIRE(opened.notice.has_value());
    REQUIRE(notices.confirm_open_delivery(key, now_ms + 1) ==
            PendingNoticeMutationStatus::applied);
  }

  [[nodiscard]] WagerMutationResult
  action(const DiscordSnowflake user, const std::string &wager_id,
         const WagerAction selected, const std::string &key,
         const std::int64_t now_ms, const bool test_mode = false) {
    return wagers->act(
        WagerActionRequest{.invocation = call(user, key, now_ms, test_mode),
                           .wager_id = wager_id,
                           .token_id = std::nullopt,
                           .action = selected,
                           .starting_fate = 100,
                           .offer_expiry_ms = 86'400'000,
                           .resolution_grace_ms = 172'800'000,
                           .next_id = ids()});
  }

  [[nodiscard]] WagerMutationResult
  outcome(const DiscordSnowflake user, const std::string &wager_id,
          const WagerRole winner, const std::string &key,
          const std::int64_t now_ms, const bool test_mode = false) {
    return wagers->submit_outcome(
        WagerOutcomeRequest{.invocation = call(user, key, now_ms, test_mode),
                            .wager_id = wager_id,
                            .token_id = std::nullopt,
                            .winner = winner,
                            .next_id = ids()});
  }

  [[nodiscard]] std::int64_t balance(const DiscordSnowflake user) const {
    return scalar(
        *context,
        "SELECT CAST(total(posting.amount) AS INTEGER) "
        "FROM tarot_account account LEFT JOIN tarot_posting posting "
        "ON posting.account_id=account.account_id LEFT JOIN "
        "tarot_transaction tx ON tx.transaction_id=posting.transaction_id "
        "WHERE account.user_id='" +
            user.str() + "' AND (tx.state='committed' OR tx.state IS NULL)");
  }

  [[nodiscard]] std::int64_t escrow_balance() const {
    return scalar(
        *context,
        "SELECT CAST(total(posting.amount) AS INTEGER) "
        "FROM tarot_account account LEFT JOIN tarot_posting posting "
        "ON posting.account_id=account.account_id LEFT JOIN "
        "tarot_transaction tx ON tx.transaction_id=posting.transaction_id "
        "WHERE account.account_kind='ESCROW' "
        "AND (tx.state='committed' OR tx.state IS NULL)");
  }

  void restart() {
    wagers.reset();
    tarot.reset();
    context.reset();
    open_runtime();
    tarot = std::make_unique<SqliteTarotRepository>(context);
    wagers = std::make_unique<SqliteWagerRepository>(context);
  }

  void open_runtime() {
    context = std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path()));
  }

  static constexpr DiscordSnowflake guild{10};
  static constexpr DiscordSnowflake channel{20};
  static constexpr DiscordSnowflake owner{30};
  static constexpr DiscordSnowflake target{31};
  static constexpr DiscordSnowflake judge{32};

  test::TemporaryDatabase temporary;
  test::FakeClock clock;
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteTarotRepository> tarot;
  std::unique_ptr<SqliteWagerRepository> wagers;
  std::size_t next_id{1'000};
};

} // namespace

TEST_CASE("peer wager funding and mutual settlement are atomic and replayable",
          "[wager][ledger][idempotency][restart]") {
  WagerFixture fixture;
  const auto offered = fixture.offer();
  const auto wager_id = offered.wager->wager_id;
  const auto funded = fixture.accept(wager_id, "accept", 1'100);
  REQUIRE(funded.status == WagerMutationStatus::applied);
  REQUIRE(funded.wager->state == WagerState::accepted_funded);
  REQUIRE(fixture.balance(WagerFixture::owner) == 90);
  REQUIRE(fixture.balance(WagerFixture::target) == 90);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT event.event_type FROM event_journal event "
                      "JOIN tarot_transaction tx ON tx.event_id=event.event_id "
                      "WHERE tx.transaction_type='STARTING_GRANT' "
                      "AND event.actor_user_id='31'") ==
          "tarot.starting_grant.v1");
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT event.idempotency_key FROM event_journal event "
                      "JOIN tarot_transaction tx ON tx.event_id=event.event_id "
                      "WHERE tx.transaction_type='STARTING_GRANT' "
                      "AND event.actor_user_id='31'") ==
          "tarot.starting_grant:31");
  REQUIRE(fixture.tarot->check_invariants().valid);

  fixture.restart();
  REQUIRE(fixture.tarot->check_invariants().valid);
  const auto first =
      fixture.outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                      "owner-outcome", 1'200);
  REQUIRE(first.wager->state == WagerState::awaiting_resolution);
  const auto settled =
      fixture.outcome(WagerFixture::target, wager_id, WagerRole::creator,
                      "target-outcome", 1'210);
  REQUIRE(settled.status == WagerMutationStatus::applied);
  REQUIRE(settled.wager->state == WagerState::resolved);
  REQUIRE(settled.wager->winner == WagerRole::creator);
  REQUIRE(fixture.balance(WagerFixture::owner) == 110);
  REQUIRE(fixture.balance(WagerFixture::target) == 90);
  REQUIRE(fixture.escrow_balance() == 0);

  const auto replay =
      fixture.outcome(WagerFixture::target, wager_id, WagerRole::creator,
                      "target-outcome", 1'500);
  REQUIRE(replay.status == WagerMutationStatus::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type LIKE 'WAGER_%'") == 2);
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("integration defers later player results across wall-clock rollback",
          "[wager][tarot][integration][ordering][restart][rollback]") {
  WagerFixture fixture;
  const auto settle_creator_win = [&](const std::int64_t base,
                                      const std::string &suffix) {
    const auto wager_id =
        fixture
            .offer(10, WagerVisibility::public_offer,
                   WagerResolutionPolicy::mutual, std::nullopt, base,
                   "Ordered result " + suffix)
            .wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "accept-" + suffix, base + 100).status ==
            WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                         "owner-" + suffix, base + 200)
                .status == WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::target, wager_id, WagerRole::creator,
                         "target-" + suffix, base + 210)
                .wager->state == WagerState::resolved);
    return text_scalar(*fixture.context,
                       "SELECT event_id FROM tarot_wager_resolution WHERE "
                       "wager_id='" +
                           wager_id + "'");
  };
  const auto earlier = settle_creator_win(1'000, "earlier");
  const auto later = settle_creator_win(500, "later");
  fixture.context->connection().execute(
      "UPDATE tarot_integration_observation SET state='failed',attempts=1,"
      "next_attempt_at_ms=100000,last_error='injected' WHERE "
      "source_event_id='" +
      earlier + "'");
  persistence::SqliteTarotIntegrationRepository integration{fixture.context};
  const auto deferred = integration.scan(3'000, 32, fixture.ids());
  REQUIRE(deferred.failed == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_player_event WHERE baseline=0") ==
          0);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT last_error FROM tarot_integration_observation "
                      "WHERE source_event_id='" +
                          later + "'") ==
          "An earlier Tarot player observation is awaiting integration.");

  const auto completed = integration.scan(100'000, 32, fixture.ids());
  REQUIRE(completed.failed == 0);
  REQUIRE(completed.completed == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT wins FROM tarot_player_stats WHERE user_id='30'") ==
          2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT losses FROM tarot_player_stats WHERE user_id='31'") ==
          2);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT last_event_id FROM tarot_player_stats WHERE "
                      "user_id='30'") == later);
}

TEST_CASE("conflicting outcomes disclose evidence only to authorized viewers",
          "[wager][dispute][privacy][judgment]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "creator-outcome", 1'200)
              .status == WagerMutationStatus::applied);
  const auto disputed =
      fixture.outcome(WagerFixture::target, wager_id, WagerRole::target,
                      "target-outcome", 1'210);
  REQUIRE(disputed.wager->state == WagerState::disputed);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(fixture.wagers
              ->add_evidence({.invocation = fixture.call(WagerFixture::target,
                                                         "evidence", 1'220),
                              .wager_id = wager_id,
                              .token_id = std::nullopt,
                              .body = "Private final score screenshot",
                              .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  const auto participant_history = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::owner, "history", 1'230),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(participant_history.evidence.size() == 1);
  REQUIRE(participant_history.evidence_total_count == 1);
  REQUIRE(participant_history.outcomes.size() == 2);
  REQUIRE(participant_history.exact);
  const auto former_judge = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::judge, "judge-history", 1'230),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(former_judge.status == WagerMutationStatus::forbidden);
  REQUIRE(former_judge.evidence.empty());

  const auto judged = fixture.wagers->judge(WagerJudgmentRequest{
      .invocation = fixture.call(WagerFixture::owner, "owner-judgment", 1'240),
      .wager_id = wager_id,
      .judgment = WagerJudgment::target,
      .reason = "The submitted score establishes the target outcome.",
      .next_id = fixture.ids()});
  REQUIRE(judged.wager->state == WagerState::resolved);
  REQUIRE(judged.wager->winner == WagerRole::target);
  REQUIRE(fixture.balance(WagerFixture::owner) == 90);
  REQUIRE(fixture.balance(WagerFixture::target) == 110);
  REQUIRE(fixture.escrow_balance() == 0);
}

TEST_CASE("private evidence reads are bounded while retaining the full count",
          "[wager][history][evidence][bounds][privacy]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "bounded-accept", 1'100).status ==
          WagerMutationStatus::applied);
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::dispute,
                      "bounded-dispute", 1'110)
              .wager->state == WagerState::disputed);
  constexpr std::size_t extra_evidence = 3;
  const auto stored_evidence = wager_history_evidence_limit + extra_evidence;
  for (std::size_t index = 0; index < stored_evidence; ++index) {
    auto body = std::string(999, static_cast<char>('a' + index));
    body += std::to_string(index);
    REQUIRE(fixture.wagers
                ->add_evidence({.invocation = fixture.call(
                                    WagerFixture::owner,
                                    "bounded-evidence-" + std::to_string(index),
                                    1'120 + static_cast<std::int64_t>(index)),
                                .wager_id = wager_id,
                                .token_id = std::nullopt,
                                .body = std::move(body),
                                .next_id = fixture.ids()})
                .status == WagerMutationStatus::applied);
  }
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_evidence WHERE wager_id='" +
                     wager_id + "'") ==
          static_cast<std::int64_t>(stored_evidence));

  const auto history = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "bounded-history", 1'200),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(history.evidence.size() == wager_history_evidence_limit);
  REQUIRE(history.evidence_total_count == stored_evidence);
  REQUIRE(history.evidence.front().starts_with("creator: a"));

  const auto dispute = fixture.wagers->disputes(
      {.invocation =
           fixture.call(WagerFixture::owner, "bounded-details", 1'201),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(dispute.evidence.size() == wager_history_evidence_limit);
  REQUIRE(dispute.evidence_total_count == stored_evidence);
}

TEST_CASE("mutual void refunds each original equal stake exactly once",
          "[wager][void][ledger][idempotency]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  const auto void_request = [&](const DiscordSnowflake user,
                                const std::string &key,
                                const std::int64_t now_ms) {
    return fixture.wagers->act(
        WagerActionRequest{.invocation = fixture.call(user, key, now_ms),
                           .wager_id = wager_id,
                           .token_id = std::nullopt,
                           .action = WagerAction::void_wager,
                           .starting_fate = 100,
                           .offer_expiry_ms = 86'400'000,
                           .resolution_grace_ms = 172'800'000,
                           .next_id = fixture.ids()});
  };
  REQUIRE(void_request(WagerFixture::owner, "void-owner", 1'200).wager->state ==
          WagerState::accepted_funded);
  const auto refunded =
      void_request(WagerFixture::target, "void-target", 1'210);
  REQUIRE(refunded.wager->state == WagerState::void_refunded);
  REQUIRE(fixture.balance(WagerFixture::owner) == 100);
  REQUIRE(fixture.balance(WagerFixture::target) == 100);
  REQUIRE(fixture.escrow_balance() == 0);
  REQUIRE(void_request(WagerFixture::target, "void-target", 2'000).status ==
          WagerMutationStatus::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_REFUND'") == 1);
  persistence::SqliteTarotIntegrationRepository integration{fixture.context};
  const auto report = integration.scan(2'001, 32, fixture.ids());
  REQUIRE(report.failed == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM relationship_event WHERE "
                 "reason_code='tarot.resolved'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM relationship_event WHERE "
                 "reason_code='tarot.honored'") == 0);
}

TEST_CASE("sealed offers keep terms out of public durable payloads",
          "[wager][sealed][privacy][outbox]") {
  WagerFixture fixture;
  REQUIRE(fixture
              .offer(17, WagerVisibility::sealed,
                     WagerResolutionPolicy::designated, WagerFixture::judge)
              .wager->state == WagerState::offered);
  const auto public_payload =
      text_scalar(*fixture.context, "SELECT payload_json FROM outbox_message "
                                    "WHERE aggregate_type='tarot_wager' "
                                    "ORDER BY created_at_ms LIMIT 1");
  REQUIRE(public_payload.find("Blood Angels") == std::string::npos);
  REQUIRE(public_payload.find("17 Fate") == std::string::npos);
  REQUIRE(public_payload.find("final score") == std::string::npos);
  REQUIRE(public_payload.find(WagerFixture::judge.str()) == std::string::npos);
  const auto private_payload =
      text_scalar(*fixture.context, "SELECT payload_json FROM pending_notice "
                                    "WHERE notice_type='tarot_wager'");
  REQUIRE(private_payload.find("Blood Angels") != std::string::npos);
  REQUIRE(private_payload.find("17 Fate") != std::string::npos);
  REQUIRE(private_payload.find("designated judge") != std::string::npos);
  REQUIRE(private_payload.find(WagerFixture::judge.str()) != std::string::npos);
  REQUIRE(private_payload.find("Outcome window") != std::string::npos);
}

TEST_CASE(
    "sealed cards structurally allow arbitrary terms and use neutral states",
    "[wager][sealed][privacy][outbox][state]") {
  WagerFixture fixture;
  const auto offered =
      fixture.offer(7, WagerVisibility::sealed, WagerResolutionPolicy::mutual,
                    std::nullopt, 1'000, "a");
  const auto wager_id = offered.wager->wager_id;
  REQUIRE(
      text_scalar(
          *fixture.context,
          "SELECT json_extract(payload_json,'$.embed.description') FROM "
          "outbox_message WHERE aggregate_id='" +
              wager_id +
              "' AND kind='discord.public.v1' ORDER BY created_at_ms LIMIT "
              "1") ==
      "Status: Offered. The terms remain sealed to the addressed participant.");
  const auto hidden_decline =
      fixture.action(WagerFixture::target, wager_id, WagerAction::decline,
                     "hidden-decline", 1'090);
  REQUIRE(hidden_decline.status == WagerMutationStatus::forbidden);
  REQUIRE(hidden_decline.wager->state == WagerState::offered);
  fixture.deliver_sealed_offer(wager_id, WagerFixture::target,
                               "neutral-delivery", 1'095);
  REQUIRE(fixture
              .action(WagerFixture::target, wager_id, WagerAction::decline,
                      "neutral-decline", 1'100)
              .wager->state == WagerState::declined);
  const auto closed_payload = text_scalar(
      *fixture.context,
      "SELECT payload_json FROM outbox_message WHERE aggregate_id='" +
          wager_id +
          "' AND kind='discord.message_edit.v1' ORDER BY created_at_ms DESC "
          "LIMIT 1");
  REQUIRE(closed_payload.find("Status: Closed.") != std::string::npos);
  REQUIRE(closed_payload.find("declined") == std::string::npos);
  REQUIRE(closed_payload.find("Proposition") == std::string::npos);
  REQUIRE(closed_payload.find("7 Fate") == std::string::npos);
}

TEST_CASE("terminal sealed offers revoke unopened notice access",
          "[wager][sealed][privacy][notice][terminal]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture.offer(13, WagerVisibility::sealed).wager->wager_id;
  const auto token_id = text_scalar(
      *fixture.context,
      "SELECT token.token_id FROM tarot_wager_notice link "
      "JOIN interaction_token token ON token.entity_type='pending_notice' "
      "AND token.entity_id=link.notice_id WHERE link.wager_id='" +
          wager_id + "' AND link.purpose='sealed_offer'");

  const auto cancelled =
      fixture.action(WagerFixture::owner, wager_id, WagerAction::cancel,
                     "cancel-unopened-sealed", 1'100);
  REQUIRE(cancelled.status == WagerMutationStatus::applied);
  REQUIRE(cancelled.wager->state == WagerState::cancelled);
  REQUIRE(
      text_scalar(*fixture.context,
                  "SELECT notice.state FROM pending_notice notice JOIN "
                  "tarot_wager_notice link ON link.notice_id=notice.notice_id "
                  "WHERE link.wager_id='" +
                      wager_id + "' AND link.purpose='sealed_offer'") ==
      "cancelled");
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT state FROM interaction_token WHERE token_id='" +
                          token_id + "'") == "cancelled");

  SqlitePendingNoticeRepository notices{fixture.context};
  const auto stale_button = notices.open_by_token(
      {.token_id = token_id,
       .interaction_kind = InteractionTokenKind::button,
       .guild_id = WagerFixture::guild,
       .channel_id = WagerFixture::channel,
       .user_id = WagerFixture::target,
       .interaction_idempotency_key = "open-cancelled-sealed",
       .now_ms = 1'110});
  REQUIRE(stale_button.status == OpenPendingNoticeStatus::unavailable);
  REQUIRE_FALSE(stale_button.notice.has_value());
  const auto inbox = notices.open_next(
      {.user_id = WagerFixture::target,
       .interaction_idempotency_key = "inbox-after-cancelled-sealed",
       .now_ms = 1'120});
  REQUIRE(inbox.status == OpenPendingNoticeStatus::no_pending_notice);
  REQUIRE_FALSE(inbox.notice.has_value());

  const auto hidden_exact = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "cancelled-sealed-exact", 1'130),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_exact.status == WagerMutationStatus::forbidden);
  REQUIRE(hidden_exact.wagers.empty());
  const auto hidden_list = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "cancelled-sealed-list", 1'140),
       .wager_id = std::nullopt,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_list.wagers.empty());
}

TEST_CASE(
    "previewed offer timing survives config changes and wall-clock rollback",
    "[wager][deadline][clock][terms]") {
  WagerFixture fixture;
  auto draft = fixture.wagers->create_draft(
      {.invocation =
           fixture.call(WagerFixture::owner, "rollback-draft", 50'000),
       .target_user_id = WagerFixture::target,
       .judge_user_id = std::nullopt,
       .visibility = WagerVisibility::public_offer,
       .resolution_policy = WagerResolutionPolicy::mutual,
       .outcome_window_ms = 3'600'000,
       .resolution_grace_ms = 172'800'000,
       .draft_expires_at_ms = 950'000,
       .is_test = false,
       .next_id = fixture.ids()});
  const auto form =
      token_from(draft.controls.front().custom_id, wager_form_prefix);
  const auto preview = fixture.wagers->preview(
      {.invocation =
           fixture.call(WagerFixture::owner, "rollback-preview", 40'000),
       .token_id = form,
       .proposition = "The stored offer duration remains authoritative",
       .stake = 10,
       .evidence_instructions = std::nullopt,
       .offer_expiry_ms = 3'600'000,
       .next_id = fixture.ids()});
  REQUIRE(preview.wager->updated_at_ms == 50'000);
  REQUIRE(preview.wager->offer_duration_ms == 3'600'000);

  const auto confirmed =
      fixture.wagers->act({.invocation = fixture.call(
                               WagerFixture::owner, "rollback-confirm", 30'000),
                           .wager_id = preview.wager->wager_id,
                           .token_id = std::nullopt,
                           .action = WagerAction::confirm,
                           .starting_fate = 100,
                           .offer_expiry_ms = 1,
                           .resolution_grace_ms = 1,
                           .next_id = fixture.ids()});
  REQUIRE(confirmed.status == WagerMutationStatus::applied);
  REQUIRE(confirmed.wager->updated_at_ms == 50'000);
  REQUIRE(confirmed.wager->offer_expires_at_ms == 3'650'000);

  const auto funded =
      fixture.accept(confirmed.wager->wager_id, "rollback-accept", 20'000);
  REQUIRE(funded.status == WagerMutationStatus::applied);
  REQUIRE(funded.wager->updated_at_ms == 50'000);
  REQUIRE(funded.wager->outcome_due_at_ms == 3'650'000);
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("public cards include immutable policy and deadline metadata",
          "[wager][public][rendering][deadline]") {
  WagerFixture fixture;
  const auto offered =
      fixture.offer(17, WagerVisibility::public_offer,
                    WagerResolutionPolicy::designated, WagerFixture::judge);
  const auto payload =
      text_scalar(*fixture.context, "SELECT payload_json FROM outbox_message "
                                    "WHERE aggregate_type='tarot_wager' "
                                    "ORDER BY created_at_ms LIMIT 1");
  REQUIRE(payload.find("Blood Angels") != std::string::npos);
  REQUIRE(payload.find("17 Fate each") != std::string::npos);
  REQUIRE(payload.find("designated judge") != std::string::npos);
  REQUIRE(payload.find("Offer deadline") != std::string::npos);
  REQUIRE(payload.find(WagerFixture::judge.str()) != std::string::npos);
  REQUIRE(offered.wager->offer_expires_at_ms.has_value());
}

TEST_CASE("public wager outbox rows match their safe durable projection",
          "[wager][public][privacy][outbox][database]") {
  WagerFixture fixture;
  const auto offered_id = fixture.offer().wager->wager_id;
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO outbox_message "
      "(outbox_id,kind,aggregate_type,aggregate_id,target_guild_id,"
      "target_channel_id,target_user_id,payload_json,state,attempt_count,"
      "max_attempts,available_at_ms,idempotency_key,provider_nonce,created_at_"
      "ms,updated_at_ms) SELECT '" +
      fixture.next() +
      "',kind,aggregate_type,aggregate_id,target_guild_id,target_channel_id,"
      "target_user_id,json_set(payload_json,'$.embed.description',"
      "'Private evidence must not enter a public offer.'),'pending',0,5,2000,"
      "'forged-public-offer','nonce-forged-public-offer',2000,2000 FROM "
      "outbox_message WHERE aggregate_id='" +
      offered_id + "' AND kind='discord.public.v1' LIMIT 1"));

  REQUIRE(fixture
              .action(WagerFixture::target, offered_id, WagerAction::decline,
                      "public-projection-decline", 1'100)
              .wager->state == WagerState::declined);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO outbox_message "
      "(outbox_id,kind,aggregate_type,aggregate_id,target_guild_id,"
      "target_channel_id,target_user_id,payload_json,state,attempt_count,"
      "max_attempts,available_at_ms,idempotency_key,provider_nonce,created_at_"
      "ms,updated_at_ms) SELECT '" +
      fixture.next() +
      "',kind,aggregate_type,aggregate_id,target_guild_id,target_channel_id,"
      "target_user_id,json_set(payload_json,'$.content',"
      "'Private history must not enter a public edit.'),'pending',0,20,2000,"
      "'forged-public-edit','nonce-forged-public-edit',2000,2000 FROM "
      "outbox_message WHERE aggregate_id='" +
      offered_id + "' AND kind='discord.message_edit.v1' LIMIT 1"));

  const auto funded_id =
      fixture
          .offer(10, WagerVisibility::public_offer,
                 WagerResolutionPolicy::mutual, std::nullopt, 3'000)
          .wager->wager_id;
  REQUIRE(fixture.accept(funded_id, "public-projection-accept", 3'100).status ==
          WagerMutationStatus::applied);
  SqliteDurableWorkRepository work{fixture.context};
  auto reminder =
      work.claim_due_job(1'803'100, 1'813'100, "worker", fixture.next());
  REQUIRE(reminder.has_value());
  REQUIRE(std::get<WagerDeadlineJobPayload>(reminder->payload).phase ==
          "reminder");
  REQUIRE(
      fixture.wagers
          ->handle_deadline(
              {.job = *reminder, .now_ms = 1'803'100, .next_id = fixture.ids()})
          .status == WagerMutationStatus::applied);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO outbox_message "
      "(outbox_id,kind,aggregate_type,aggregate_id,target_guild_id,"
      "target_channel_id,target_user_id,payload_json,state,attempt_count,"
      "max_attempts,available_at_ms,idempotency_key,provider_nonce,created_at_"
      "ms,updated_at_ms) SELECT '" +
      fixture.next() +
      "',kind,aggregate_type,aggregate_id,target_guild_id,target_channel_id,"
      "target_user_id,json_set(payload_json,'$.embed.description',"
      "'A balance must not enter a public reminder.'),'pending',0,5,2000,"
      "'forged-public-reminder','nonce-forged-public-reminder',2000,2000 FROM "
      "outbox_message WHERE aggregate_id='" +
      funded_id +
      "' AND kind='discord.public.v1' AND json_extract(payload_json,"
      "'$.embed.title')='A peer wager awaits resolution' LIMIT 1"));
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("simultaneous acceptance serializes to one escrow transaction",
          "[wager][concurrency][ledger]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  WagerMutationResult first;
  WagerMutationResult second;
  std::thread one{
      [&] { first = fixture.accept(wager_id, "accept-one", 1'100); }};
  std::thread two{
      [&] { second = fixture.accept(wager_id, "accept-two", 1'100); }};
  one.join();
  two.join();
  REQUIRE(((first.status == WagerMutationStatus::applied &&
            second.status == WagerMutationStatus::invalid_state) ||
           (second.status == WagerMutationStatus::applied &&
            first.status == WagerMutationStatus::invalid_state)));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_ESCROW_FUND'") == 1);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("successful component actions replay from their stored action",
          "[wager][control][idempotency][restart]") {
  WagerFixture fixture;
  const auto offered = fixture.offer();
  const auto accept_token = text_scalar(
      *fixture.context,
      "SELECT token_id FROM tarot_wager_control WHERE wager_id='" +
          offered.wager->wager_id + "' AND action='accept' AND state='active'");
  const WagerActionRequest request{
      .invocation =
          fixture.call(WagerFixture::target, "component-accept", 1'100),
      .wager_id = {},
      .token_id = accept_token,
      .action = WagerAction::cancel,
      .starting_fate = 100,
      .offer_expiry_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .next_id = fixture.ids()};
  const auto accepted = fixture.wagers->act(request);
  REQUIRE(accepted.status == WagerMutationStatus::applied);
  REQUIRE(accepted.wager->state == WagerState::accepted_funded);

  fixture.restart();
  const auto replay = fixture.wagers->act(request);
  REQUIRE(replay.status == WagerMutationStatus::unchanged);
  REQUIRE(replay.wager->state == WagerState::accepted_funded);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_ESCROW_FUND'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_receipt WHERE "
                 "idempotency_key='component-accept'") == 1);
  REQUIRE(fixture.escrow_balance() == 20);
}

TEST_CASE("participant history uses target-bound expiring five-item snapshots",
          "[wager][history][privacy][pagination]") {
  WagerFixture fixture;
  for (std::int64_t index = 0; index < 6; ++index)
    REQUIRE(fixture
                .offer(10, WagerVisibility::public_offer,
                       WagerResolutionPolicy::mutual, std::nullopt,
                       1'000 + index * 100)
                .status == WagerMutationStatus::applied);
  const auto first = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::owner, "history-first", 3'000),
       .wager_id = std::nullopt,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(first.wagers.size() == 5);
  REQUIRE(first.next_cursor_id.has_value());
  const auto wrong_user = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::target, "history-wrong", 3'001),
       .wager_id = std::nullopt,
       .cursor_id = first.next_cursor_id,
       .next_id = fixture.ids()});
  REQUIRE(wrong_user.status == WagerMutationStatus::forbidden);
  REQUIRE(wrong_user.wagers.empty());
  const auto second = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::owner, "history-second", 3'001),
       .wager_id = std::nullopt,
       .cursor_id = first.next_cursor_id,
       .next_id = fixture.ids()});
  REQUIRE(second.status == WagerMutationStatus::applied);
  REQUIRE(second.wagers.size() == 1);
  REQUIRE_FALSE(second.next_cursor_id.has_value());
  const auto expired = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "history-expired", 903'000),
       .wager_id = std::nullopt,
       .cursor_id = first.next_cursor_id,
       .next_id = fixture.ids()});
  REQUIRE(expired.status == WagerMutationStatus::expired);
  REQUIRE(expired.wagers.empty());
}

TEST_CASE("exact participant history returns only current role controls",
          "[wager][history][controls][authorization]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  const auto creator_offer = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "creator-details", 1'100),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(creator_offer.controls.size() == 1);
  REQUIRE(creator_offer.controls.front().action == "Cancel");

  const auto target_offer = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "target-details", 1'100),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(target_offer.controls.size() == 2);
  REQUIRE(target_offer.controls[0].action == "Accept and fund");
  REQUIRE(target_offer.controls[1].action == "Decline");

  REQUIRE(fixture.accept(wager_id, "accept", 1'200).status ==
          WagerMutationStatus::applied);
  const auto funded = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::owner, "funded-details", 1'210),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(funded.controls.size() == 4);
  REQUIRE(funded.controls[0].action == "Submit outcome");
  REQUIRE(funded.controls[0].custom_id.starts_with(wager_outcome_prefix));
  REQUIRE(funded.controls[1].action == "Dispute");
  REQUIRE(funded.controls[2].action == "Consent to void");
  REQUIRE(funded.controls[3].action == "Add private evidence");
  REQUIRE(funded.controls[3].custom_id.starts_with(wager_evidence_prefix));

  const auto outsider = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::judge, "outsider-details", 1'210),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(outsider.status == WagerMutationStatus::forbidden);
  REQUIRE(outsider.controls.empty());

  const auto outcome_token =
      token_from(funded.controls[0].custom_id, wager_outcome_prefix);
  const auto wrong_user = fixture.wagers->submit_outcome(
      {.invocation =
           fixture.call(WagerFixture::target, "wrong-outcome-user", 1'211),
       .wager_id = {},
       .token_id = outcome_token,
       .winner = WagerRole::creator,
       .next_id = fixture.ids()});
  REQUIRE(wrong_user.status == WagerMutationStatus::forbidden);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_outcome WHERE wager_id='" +
                     wager_id + "'") == 0);
  const auto submitted = fixture.wagers->submit_outcome(
      {.invocation =
           fixture.call(WagerFixture::owner, "component-outcome", 1'212),
       .wager_id = {},
       .token_id = outcome_token,
       .winner = WagerRole::creator,
       .next_id = fixture.ids()});
  REQUIRE(submitted.status == WagerMutationStatus::applied);
  REQUIRE(submitted.wager->state == WagerState::awaiting_resolution);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT state FROM tarot_wager_control WHERE token_id='" +
                          outcome_token + "'") == "used");
}

TEST_CASE(
    "sealed offer history and funding require successful private delivery",
    "[wager][history][sealed][privacy][ledger]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture.offer(17, WagerVisibility::sealed).wager->wager_id;

  const auto creator = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::owner, "sealed-creator", 1'100),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(creator.status == WagerMutationStatus::applied);
  REQUIRE(creator.wagers.front().stake == 17);

  const auto hidden_exact = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::target, "sealed-hidden", 1'100),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_exact.status == WagerMutationStatus::forbidden);
  REQUIRE(hidden_exact.wagers.empty());
  const auto hidden_list = fixture.wagers->history(
      {.invocation = fixture.call(WagerFixture::target, "sealed-list", 1'100),
       .wager_id = std::nullopt,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_list.wagers.empty());
  const auto blocked_accept =
      fixture.accept(wager_id, "sealed-accept-before-delivery", 1'105);
  REQUIRE(blocked_accept.status == WagerMutationStatus::forbidden);
  REQUIRE(blocked_accept.wager->state == WagerState::offered);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_ESCROW_FUND'") == 0);
  REQUIRE(fixture.escrow_balance() == 0);

  const auto token_id = text_scalar(
      *fixture.context,
      "SELECT token.token_id FROM tarot_wager_notice link "
      "JOIN interaction_token token ON token.entity_type='pending_notice' "
      "AND token.entity_id=link.notice_id WHERE link.wager_id='" +
          wager_id + "' AND link.purpose='sealed_offer'");
  SqlitePendingNoticeRepository notices{fixture.context};
  const auto opened =
      notices.open_by_token({.token_id = token_id,
                             .interaction_kind = InteractionTokenKind::button,
                             .guild_id = WagerFixture::guild,
                             .channel_id = WagerFixture::channel,
                             .user_id = WagerFixture::target,
                             .interaction_idempotency_key = "sealed-open",
                             .now_ms = 1'110});
  REQUIRE(opened.status == OpenPendingNoticeStatus::opened);
  REQUIRE(opened.notice.has_value());

  const auto still_hidden = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "sealed-pre-delivery", 1'111),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(still_hidden.status == WagerMutationStatus::forbidden);
  REQUIRE(notices.confirm_open_delivery("sealed-open", 1'112) ==
          PendingNoticeMutationStatus::applied);

  const auto revealed_exact = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "sealed-revealed", 1'113),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(revealed_exact.status == WagerMutationStatus::applied);
  REQUIRE(revealed_exact.wagers.front().stake == 17);
  REQUIRE(revealed_exact.wagers.front().proposition ==
          "The Blood Angels win the match");
  const auto revealed_list = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::target, "sealed-revealed-list", 1'114),
       .wager_id = std::nullopt,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(revealed_list.wagers.size() == 1);
  REQUIRE(revealed_list.wagers.front().wager_id == wager_id);
  const auto funded =
      fixture.accept(wager_id, "sealed-accept-after-delivery", 1'115);
  REQUIRE(funded.status == WagerMutationStatus::applied);
  REQUIRE(funded.wager->state == WagerState::accepted_funded);
  REQUIRE(fixture.escrow_balance() == 34);
}

TEST_CASE("target acceptance uses the complete confirmed timing terms",
          "[wager][terms][sealed][restart]") {
  WagerFixture fixture;
  const auto offered = fixture.offer(10, WagerVisibility::sealed);
  const auto wager_id = offered.wager->wager_id;
  REQUIRE(offered.wager->resolution_grace_ms == 172'800'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pending_notice WHERE "
                 "source_aggregate_id='" +
                     wager_id + "' AND instr(payload_json,'" + wager_id +
                     "')>0 AND instr(payload_json,'<@30>')>0 "
                     "AND instr(payload_json,'48 hours')>0") == 1);
  fixture.deliver_sealed_offer(wager_id, WagerFixture::target,
                               "timing-terms-delivery", 1'090);
  const auto funded = fixture.wagers->act(WagerActionRequest{
      .invocation = fixture.call(WagerFixture::target,
                                 "accept-after-config-change", 1'100),
      .wager_id = wager_id,
      .token_id = std::nullopt,
      .action = WagerAction::accept,
      .starting_fate = 100,
      .offer_expiry_ms = 86'400'000,
      .resolution_grace_ms = 3'600'000,
      .next_id = fixture.ids()});
  REQUIRE(funded.status == WagerMutationStatus::applied);
  REQUIRE(*funded.wager->resolution_grace_until_ms -
              *funded.wager->outcome_due_at_ms ==
          172'800'000);
  const auto public_offer =
      fixture.offer(10, WagerVisibility::public_offer,
                    WagerResolutionPolicy::mutual, std::nullopt, 2'000);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT count(*) FROM outbox_message WHERE aggregate_id='" +
                 public_offer.wager->wager_id +
                 "' AND instr(payload_json,'Outcome window: 1 hours')>0 "
                 "AND instr(payload_json,'Owner escalation: 48 hours')>0") ==
      1);
}

TEST_CASE("deadline jobs survive restart and never award Fate by timeout",
          "[wager][scheduler][restart][escrow]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  SqliteDurableWorkRepository work{fixture.context};
  auto reminder =
      work.claim_due_job(1'801'100, 1'811'100, "worker", fixture.next());
  REQUIRE(reminder.has_value());
  REQUIRE(std::get<WagerDeadlineJobPayload>(reminder->payload).phase ==
          "reminder");
  REQUIRE(
      fixture.wagers
          ->handle_deadline(
              {.job = *reminder, .now_ms = 1'801'100, .next_id = fixture.ids()})
          .status == WagerMutationStatus::applied);

  auto due = work.claim_due_job(3'601'100, 3'611'100, "worker", fixture.next());
  REQUIRE(due.has_value());
  const auto awaiting = fixture.wagers->handle_deadline(
      {.job = *due, .now_ms = 3'601'100, .next_id = fixture.ids()});
  REQUIRE(awaiting.wager->state == WagerState::awaiting_resolution);
  REQUIRE(fixture.escrow_balance() == 20);
  const auto late_outcome =
      fixture.outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                      "outcome-after-deadline", 3'601'110);
  REQUIRE(late_outcome.status == WagerMutationStatus::applied);
  REQUIRE(late_outcome.wager->state == WagerState::awaiting_resolution);

  fixture.restart();
  SqliteDurableWorkRepository restarted_work{fixture.context};
  auto grace = restarted_work.claim_due_job(176'401'100, 176'411'100,
                                            "restarted-worker", fixture.next());
  REQUIRE(grace.has_value());
  const auto disputed = fixture.wagers->handle_deadline(
      {.job = *grace, .now_ms = 176'401'100, .next_id = fixture.ids()});
  REQUIRE(disputed.wager->state == WagerState::disputed);
  REQUIRE(fixture.balance(WagerFixture::owner) == 90);
  REQUIRE(fixture.balance(WagerFixture::target) == 90);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type IN ('WAGER_PAYOUT','WAGER_REFUND')") == 0);
  REQUIRE(fixture.wagers->check_invariants().valid);
  const auto agreed =
      fixture.outcome(WagerFixture::target, wager_id, WagerRole::creator,
                      "agreement-after-grace", 176'401'110);
  REQUIRE(agreed.wager->state == WagerState::resolved);
  REQUIRE(fixture.escrow_balance() == 0);
}

TEST_CASE("settlement cancels queued resolution reminders",
          "[wager][scheduler][outbox][notice][settlement]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept-reminder", 1'100).status ==
          WagerMutationStatus::applied);
  SqliteDurableWorkRepository work{fixture.context};
  auto reminder =
      work.claim_due_job(1'801'100, 1'811'100, "worker", fixture.next());
  REQUIRE(reminder.has_value());
  REQUIRE(
      fixture.wagers
          ->handle_deadline(
              {.job = *reminder, .now_ms = 1'801'100, .next_id = fixture.ids()})
          .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "reminder-creator-outcome", 1'801'200)
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::target, wager_id, WagerRole::creator,
                       "reminder-target-outcome", 1'801'210)
              .wager->state == WagerState::resolved);

  REQUIRE(
      text_scalar(*fixture.context, "SELECT state FROM outbox_message WHERE "
                                    "idempotency_key='outbox:wager-reminder:" +
                                        wager_id + "'") == "cancelled");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pending_notice notice JOIN "
                 "tarot_wager_notice link ON link.notice_id=notice.notice_id "
                 "WHERE link.wager_id='" +
                     wager_id +
                     "' AND link.purpose='reminder' AND "
                     "notice.state='cancelled'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE aggregate_id='" +
                     wager_id +
                     "' AND kind='discord.message_edit.v1' "
                     "AND state='pending'") >= 1);
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("every durable wager state resumes with its artifacts and replays",
          "[wager][restart][state-machine][idempotency]") {
  WagerFixture fixture;

  SECTION("draft") {
    const auto draft = fixture.wagers->create_draft(WagerCreateRequest{
        .invocation = fixture.call(WagerFixture::owner, "restart-draft", 1'000),
        .target_user_id = WagerFixture::target,
        .judge_user_id = std::nullopt,
        .visibility = WagerVisibility::public_offer,
        .resolution_policy = WagerResolutionPolicy::mutual,
        .outcome_window_ms = 3'600'000,
        .resolution_grace_ms = 172'800'000,
        .draft_expires_at_ms = 901'000,
        .is_test = false,
        .next_id = fixture.ids()});
    const auto form =
        token_from(draft.controls.front().custom_id, wager_form_prefix);
    fixture.restart();
    const auto preview = fixture.wagers->preview(
        {.invocation =
             fixture.call(WagerFixture::owner, "restart-preview", 1'010),
         .token_id = form,
         .proposition = "A draft survives a process restart",
         .stake = 10,
         .evidence_instructions = std::nullopt,
         .offer_expiry_ms = 86'400'000,
         .next_id = fixture.ids()});
    REQUIRE(preview.status == WagerMutationStatus::applied);
    REQUIRE(preview.wager->state == WagerState::draft);
    REQUIRE(preview.controls.size() == 2);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM scheduled_job job JOIN "
                   "tarot_wager_job link ON link.job_id=job.job_id WHERE "
                   "link.wager_id='" +
                       preview.wager->wager_id +
                       "' AND link.phase='draft_expiry' AND "
                       "job.state='pending'") == 1);
  }

  SECTION("offered") {
    const auto offered = fixture.offer();
    const auto wager_id = offered.wager->wager_id;
    fixture.restart();
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM tarot_wager_control WHERE wager_id='" +
                       wager_id + "' AND state='active'") == 3);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM scheduled_job job JOIN "
                   "tarot_wager_job link ON link.job_id=job.job_id WHERE "
                   "link.wager_id='" +
                       wager_id +
                       "' AND link.phase='offer_expiry' AND "
                       "job.state='pending'") == 1);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM tarot_wager_public_card WHERE "
                   "wager_id='" +
                       wager_id + "'") == 1);
    REQUIRE(fixture.accept(wager_id, "restart-offered-accept", 1'100)
                .wager->state == WagerState::accepted_funded);
  }

  SECTION("accepted funded") {
    const auto wager_id = fixture.offer().wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "restart-funded-accept", 1'100).status ==
            WagerMutationStatus::applied);
    fixture.restart();
    REQUIRE(fixture.accept(wager_id, "restart-funded-accept", 1'100).status ==
            WagerMutationStatus::unchanged);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM scheduled_job job JOIN "
                   "tarot_wager_job link ON link.job_id=job.job_id WHERE "
                   "link.wager_id='" +
                       wager_id + "' AND job.state='pending'") == 3);
    REQUIRE(fixture.escrow_balance() == 20);
  }

  SECTION("awaiting resolution") {
    const auto wager_id = fixture.offer().wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "restart-awaiting-accept", 1'100).status ==
            WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                         "restart-awaiting-outcome", 1'200)
                .wager->state == WagerState::awaiting_resolution);
    fixture.restart();
    const auto replay =
        fixture.outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                        "restart-awaiting-outcome", 1'200);
    REQUIRE(replay.status == WagerMutationStatus::unchanged);
    REQUIRE(replay.wager->state == WagerState::awaiting_resolution);
    REQUIRE(fixture.escrow_balance() == 20);
  }

  SECTION("disputed") {
    const auto wager_id = fixture.offer().wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "restart-disputed-accept", 1'100).status ==
            WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                         "restart-disputed-creator", 1'200)
                .status == WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::target, wager_id, WagerRole::target,
                         "restart-disputed-target", 1'210)
                .wager->state == WagerState::disputed);
    fixture.restart();
    const auto history = fixture.wagers->history(
        {.invocation = fixture.call(WagerFixture::owner,
                                    "restart-disputed-history", 1'220),
         .wager_id = wager_id,
         .cursor_id = std::nullopt,
         .next_id = fixture.ids()});
    REQUIRE(history.exact);
    REQUIRE(history.wagers.front().state == WagerState::disputed);
    REQUIRE(history.outcomes.size() == 2);
    const auto judged = fixture.wagers->judge(
        {.invocation =
             fixture.call(WagerFixture::owner, "restart-owner-judgment", 1'230),
         .wager_id = wager_id,
         .judgment = WagerJudgment::creator,
         .reason = "Conflicting submissions require an owner decision.",
         .next_id = fixture.ids()});
    REQUIRE(judged.wager->state == WagerState::resolved);
    REQUIRE(fixture.escrow_balance() == 0);
  }

  SECTION("resolved") {
    const auto wager_id = fixture.offer().wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "restart-resolved-accept", 1'100).status ==
            WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                         "restart-resolved-creator", 1'200)
                .status == WagerMutationStatus::applied);
    REQUIRE(fixture
                .outcome(WagerFixture::target, wager_id, WagerRole::creator,
                         "restart-resolved-target", 1'210)
                .wager->state == WagerState::resolved);
    fixture.restart();
    REQUIRE(fixture
                .outcome(WagerFixture::target, wager_id, WagerRole::creator,
                         "restart-resolved-target", 1'210)
                .status == WagerMutationStatus::unchanged);
    const auto history = fixture.wagers->history(
        {.invocation = fixture.call(WagerFixture::target,
                                    "restart-resolved-history", 1'220),
         .wager_id = wager_id,
         .cursor_id = std::nullopt,
         .next_id = fixture.ids()});
    REQUIRE(history.wagers.front().state == WagerState::resolved);
    REQUIRE(history.controls.empty());
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM tarot_transaction WHERE "
                   "transaction_type='WAGER_PAYOUT'") == 1);
  }

  SECTION("void refunded") {
    const auto wager_id = fixture.offer().wager->wager_id;
    REQUIRE(fixture.accept(wager_id, "restart-void-accept", 1'100).status ==
            WagerMutationStatus::applied);
    REQUIRE(fixture
                .action(WagerFixture::owner, wager_id, WagerAction::void_wager,
                        "restart-void-creator", 1'200)
                .status == WagerMutationStatus::applied);
    REQUIRE(fixture
                .action(WagerFixture::target, wager_id, WagerAction::void_wager,
                        "restart-void-target", 1'210)
                .wager->state == WagerState::void_refunded);
    fixture.restart();
    const auto replay =
        fixture.action(WagerFixture::target, wager_id, WagerAction::void_wager,
                       "restart-void-target", 1'210);
    REQUIRE(replay.status == WagerMutationStatus::unchanged);
    REQUIRE(replay.wager->state == WagerState::void_refunded);
    REQUIRE(fixture.balance(WagerFixture::owner) == 100);
    REQUIRE(fixture.balance(WagerFixture::target) == 100);
    REQUIRE(fixture.escrow_balance() == 0);
  }

  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("insufficient and competing accepts leave no partial funding",
          "[wager][ledger][atomicity][insufficient]") {
  WagerFixture fixture;
  const auto first_id = fixture.offer(60).wager->wager_id;
  const auto rejected = fixture.accept(first_id, "accept-too-poor", 1'100, 50);
  REQUIRE(rejected.status == WagerMutationStatus::insufficient_funds);
  REQUIRE(rejected.wager->state == WagerState::offered);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_account WHERE user_id='31'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_transfer") == 0);
  REQUIRE(fixture.escrow_balance() == 0);
  REQUIRE(fixture.accept(first_id, "accept-retry", 1'110, 100).status ==
          WagerMutationStatus::applied);
  REQUIRE(fixture.balance(WagerFixture::owner) == 40);
  REQUIRE(fixture.balance(WagerFixture::target) == 40);

  const auto second_id =
      fixture
          .offer(50, WagerVisibility::public_offer,
                 WagerResolutionPolicy::mutual, std::nullopt, 2'000)
          .wager->wager_id;
  const auto competing = fixture.accept(second_id, "accept-competing", 2'100);
  REQUIRE(competing.status == WagerMutationStatus::insufficient_funds);
  REQUIRE(competing.wager->state == WagerState::offered);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_ESCROW_FUND'") == 1);
  REQUIRE(fixture.escrow_balance() == 120);
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("a designated judge is private until funding and loses authority on "
          "dispute",
          "[wager][judge][privacy][authorization]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture
          .offer(10, WagerVisibility::sealed, WagerResolutionPolicy::designated,
                 WagerFixture::judge)
          .wager->wager_id;
  const auto preaccept_probe = fixture.wagers->judge(
      {.invocation =
           fixture.call(WagerFixture::judge, "preaccept-probe", 1'050),
       .wager_id = wager_id,
       .judgment = WagerJudgment::creator,
       .reason = "The judge must not see sealed terms before acceptance.",
       .next_id = fixture.ids()});
  REQUIRE(preaccept_probe.status == WagerMutationStatus::forbidden);
  REQUIRE_FALSE(preaccept_probe.wager.has_value());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_notice WHERE "
                 "target_user_id='32'") == 0);
  fixture.deliver_sealed_offer(wager_id, WagerFixture::target,
                               "judge-offer-delivery", 1'090);
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  REQUIRE(
      scalar(
          *fixture.context,
          "SELECT count(*) FROM tarot_wager_notice WHERE purpose='accepted'") ==
      3);
  const auto disputed = fixture.wagers->act(WagerActionRequest{
      .invocation = fixture.call(WagerFixture::target, "dispute", 1'200),
      .wager_id = wager_id,
      .token_id = std::nullopt,
      .action = WagerAction::dispute,
      .starting_fate = 100,
      .offer_expiry_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .next_id = fixture.ids()});
  REQUIRE(disputed.wager->state == WagerState::disputed);
  const auto judge_attempt = fixture.wagers->judge(
      {.invocation = fixture.call(WagerFixture::judge, "late-judge", 1'210),
       .wager_id = wager_id,
       .judgment = WagerJudgment::creator,
       .reason = "This judge should no longer have authority.",
       .next_id = fixture.ids()});
  REQUIRE(judge_attempt.status == WagerMutationStatus::forbidden);
  REQUIRE(fixture.escrow_balance() == 20);
}

TEST_CASE("unfunded wager terminal transitions are authorized and move no Fate",
          "[wager][transition][authorization][deadline]") {
  WagerFixture fixture;

  const auto declined_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture
              .action(WagerFixture::owner, declined_id, WagerAction::decline,
                      "wrong-decline", 1'050)
              .status == WagerMutationStatus::forbidden);
  REQUIRE(fixture
              .action(WagerFixture::target, declined_id, WagerAction::decline,
                      "decline", 1'060)
              .wager->state == WagerState::declined);
  REQUIRE(fixture
              .action(WagerFixture::target, declined_id, WagerAction::accept,
                      "late-accept", 1'070)
              .status == WagerMutationStatus::invalid_state);

  const auto cancelled_id =
      fixture
          .offer(10, WagerVisibility::public_offer,
                 WagerResolutionPolicy::mutual, std::nullopt, 2'000)
          .wager->wager_id;
  REQUIRE(fixture
              .action(WagerFixture::target, cancelled_id, WagerAction::cancel,
                      "wrong-cancel", 2'050)
              .status == WagerMutationStatus::forbidden);
  REQUIRE(fixture
              .action(WagerFixture::owner, cancelled_id, WagerAction::cancel,
                      "cancel", 2'060)
              .wager->state == WagerState::cancelled);

  const auto expiring =
      fixture.offer(10, WagerVisibility::public_offer,
                    WagerResolutionPolicy::mutual, std::nullopt, 3'000);
  REQUIRE(expiring.wager->offer_expires_at_ms.has_value());
  const auto expired = fixture.action(
      WagerFixture::target, expiring.wager->wager_id, WagerAction::accept,
      "accept-at-deadline", *expiring.wager->offer_expires_at_ms);
  REQUIRE(expired.status == WagerMutationStatus::expired);
  REQUIRE(expired.wager->state == WagerState::expired);

  const auto component_expiring =
      fixture.offer(10, WagerVisibility::public_offer,
                    WagerResolutionPolicy::mutual, std::nullopt, 3'250);
  const auto accept_token =
      text_scalar(*fixture.context,
                  "SELECT token_id FROM tarot_wager_control WHERE wager_id='" +
                      component_expiring.wager->wager_id +
                      "' AND action='accept' AND state='active'");
  const auto component_expired =
      fixture.wagers->act({.invocation = fixture.call(
                               WagerFixture::target, "component-at-deadline",
                               *component_expiring.wager->offer_expires_at_ms),
                           .wager_id = {},
                           .token_id = accept_token,
                           .action = WagerAction::cancel,
                           .starting_fate = 100,
                           .offer_expiry_ms = 86'400'000,
                           .resolution_grace_ms = 172'800'000,
                           .next_id = fixture.ids()});
  REQUIRE(component_expired.status == WagerMutationStatus::expired);
  REQUIRE(component_expired.wager->state == WagerState::expired);

  const auto sealed =
      fixture.offer(10, WagerVisibility::sealed, WagerResolutionPolicy::mutual,
                    std::nullopt, 3'500);
  const auto outsider = fixture.action(
      WagerFixture::judge, sealed.wager->wager_id, WagerAction::accept,
      "outsider-at-deadline", *sealed.wager->offer_expires_at_ms);
  REQUIRE(outsider.status == WagerMutationStatus::forbidden);
  REQUIRE(outsider.wager->state == WagerState::offered);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_action WHERE wager_id='" +
                     sealed.wager->wager_id + "' AND action='expired'") == 0);

  auto draft = fixture.wagers->create_draft(WagerCreateRequest{
      .invocation = fixture.call(WagerFixture::owner, "draft-discard", 4'000),
      .target_user_id = WagerFixture::target,
      .judge_user_id = std::nullopt,
      .visibility = WagerVisibility::public_offer,
      .resolution_policy = WagerResolutionPolicy::mutual,
      .outcome_window_ms = 3'600'000,
      .resolution_grace_ms = 172'800'000,
      .draft_expires_at_ms = 904'000,
      .is_test = false,
      .next_id = fixture.ids()});
  const auto overdue_details = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "overdue-draft", 904'000),
       .wager_id = draft.wager->wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(overdue_details.controls.empty());
  REQUIRE(fixture
              .action(WagerFixture::owner, draft.wager->wager_id,
                      WagerAction::discard, "discard", 904'000)
              .wager->state == WagerState::expired);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type LIKE 'WAGER_%'") == 0);
  REQUIRE(fixture.escrow_balance() == 0);
}

TEST_CASE(
    "designated judgment is reasoned and cannot be bypassed before dispute",
    "[wager][judge][transition][ledger]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture
          .offer(12, WagerVisibility::public_offer,
                 WagerResolutionPolicy::designated, WagerFixture::judge)
          .wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "mutual-bypass", 1'150)
              .status == WagerMutationStatus::invalid_state);
  const auto judged = fixture.wagers->judge(
      {.invocation = fixture.call(WagerFixture::judge, "judge", 1'200),
       .wager_id = wager_id,
       .judgment = WagerJudgment::target,
       .reason = "The agreed score report establishes the target result.",
       .next_id = fixture.ids()});
  REQUIRE(judged.wager->state == WagerState::resolved);
  REQUIRE(judged.wager->winner == WagerRole::target);
  REQUIRE(fixture.balance(WagerFixture::owner) == 88);
  REQUIRE(fixture.balance(WagerFixture::target) == 112);
  REQUIRE(fixture.escrow_balance() == 0);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT authority FROM tarot_wager_resolution") ==
          "judge");
  REQUIRE(
      fixture.wagers
          ->judge(
              {.invocation = fixture.call(WagerFixture::judge, "judge", 1'500),
               .wager_id = wager_id,
               .judgment = WagerJudgment::target,
               .reason =
                   "The agreed score report establishes the target result.",
               .next_id = fixture.ids()})
          .status == WagerMutationStatus::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_PAYOUT'") == 1);
}

TEST_CASE("designated judgment authority ends exactly at the grace boundary",
          "[wager][judge][deadline][authorization]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture
          .offer(10, WagerVisibility::sealed, WagerResolutionPolicy::designated,
                 WagerFixture::judge)
          .wager->wager_id;
  fixture.deliver_sealed_offer(wager_id, WagerFixture::target,
                               "boundary-offer-delivery", 1'090);
  const auto funded = fixture.accept(wager_id, "accept-boundary", 1'100);
  REQUIRE(funded.wager->resolution_grace_until_ms.has_value());
  const auto judgment = fixture.wagers->judge(
      {.invocation = fixture.call(WagerFixture::judge, "judge-at-grace",
                                  *funded.wager->resolution_grace_until_ms),
       .wager_id = wager_id,
       .judgment = WagerJudgment::creator,
       .reason = "This arrives exactly at the owner escalation boundary.",
       .next_id = fixture.ids()});
  REQUIRE(judgment.status == WagerMutationStatus::forbidden);
  REQUIRE_FALSE(judgment.wager.has_value());
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type IN ('WAGER_PAYOUT','WAGER_REFUND')") == 0);
}

TEST_CASE("a participant can adopt the other submission after dispute",
          "[wager][dispute][agree][ledger]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  SqliteDurableWorkRepository work{fixture.context};
  auto reminder =
      work.claim_due_job(1'801'100, 1'811'100, "worker", fixture.next());
  REQUIRE(reminder.has_value());
  REQUIRE(
      fixture.wagers
          ->handle_deadline(
              {.job = *reminder, .now_ms = 1'801'100, .next_id = fixture.ids()})
          .status == WagerMutationStatus::applied);
  REQUIRE(
      text_scalar(*fixture.context, "SELECT state FROM outbox_message WHERE "
                                    "idempotency_key='outbox:wager-reminder:" +
                                        wager_id + "'") == "pending");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pending_notice notice JOIN "
                 "tarot_wager_notice link ON link.notice_id=notice.notice_id "
                 "WHERE link.wager_id='" +
                     wager_id +
                     "' AND link.purpose='reminder' AND "
                     "notice.state='pending'") == 2);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "creator-outcome", 1'801'200)
              .wager->state == WagerState::awaiting_resolution);
  REQUIRE(fixture
              .action(WagerFixture::target, wager_id, WagerAction::dispute,
                      "explicit-dispute", 1'801'210)
              .wager->state == WagerState::disputed);
  REQUIRE(
      text_scalar(*fixture.context, "SELECT state FROM outbox_message WHERE "
                                    "idempotency_key='outbox:wager-reminder:" +
                                        wager_id + "'") == "cancelled");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pending_notice notice JOIN "
                 "tarot_wager_notice link ON link.notice_id=notice.notice_id "
                 "WHERE link.wager_id='" +
                     wager_id +
                     "' AND link.purpose='reminder' AND "
                     "notice.state='cancelled'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token token JOIN "
                 "tarot_wager_notice link ON link.notice_id=token.entity_id "
                 "WHERE link.wager_id='" +
                     wager_id +
                     "' AND link.purpose='reminder' AND "
                     "token.state='cancelled'") == 2);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT count(*) FROM scheduled_job job JOIN tarot_wager_job link "
             "ON link.job_id=job.job_id WHERE link.wager_id='" +
                 wager_id + "' AND job.state='pending'") == 0);
  const auto agreed = fixture.action(WagerFixture::target, wager_id,
                                     WagerAction::agree, "adopt", 1'801'220);
  REQUIRE(agreed.wager->state == WagerState::resolved);
  REQUIRE(agreed.wager->winner == WagerRole::creator);
  REQUIRE(fixture.balance(WagerFixture::owner) == 110);
  REQUIRE(fixture.balance(WagerFixture::target) == 90);
  REQUIRE(fixture.escrow_balance() == 0);
}

TEST_CASE(
    "owner self simulation combines role postings and cleanup reverses them",
    "[wager][test-mode][cleanup][ledger]") {
  WagerFixture fixture;
  REQUIRE_THROWS_AS(
      fixture.wagers->create_draft(WagerCreateRequest{
          .invocation = fixture.call(WagerFixture::owner, "external-test-judge",
                                     900, true),
          .target_user_id = WagerFixture::owner,
          .judge_user_id = WagerFixture::judge,
          .visibility = WagerVisibility::public_offer,
          .resolution_policy = WagerResolutionPolicy::designated,
          .outcome_window_ms = 3'600'000,
          .resolution_grace_ms = 172'800'000,
          .draft_expires_at_ms = 900'900,
          .is_test = true,
          .next_id = fixture.ids()}),
      std::invalid_argument);
  auto draft = fixture.wagers->create_draft(
      WagerCreateRequest{.invocation = fixture.call(WagerFixture::owner,
                                                    "test-draft", 1'000, true),
                         .target_user_id = WagerFixture::owner,
                         .judge_user_id = std::nullopt,
                         .visibility = WagerVisibility::sealed,
                         .resolution_policy = WagerResolutionPolicy::mutual,
                         .outcome_window_ms = 3'600'000,
                         .resolution_grace_ms = 172'800'000,
                         .draft_expires_at_ms = 901'000,
                         .is_test = true,
                         .next_id = fixture.ids()});
  const auto token =
      token_from(draft.controls.front().custom_id, wager_form_prefix);
  auto preview = fixture.wagers->preview(
      {.invocation =
           fixture.call(WagerFixture::owner, "test-preview", 1'010, true),
       .token_id = token,
       .proposition = "Owner-only simulated wager",
       .stake = 10,
       .evidence_instructions = std::nullopt,
       .offer_expiry_ms = 86'400'000,
       .next_id = fixture.ids()});
  REQUIRE(preview.status == WagerMutationStatus::applied);
  const auto wager_id = preview.wager->wager_id;
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::confirm,
                      "test-confirm", 1'020, true)
              .wager->state == WagerState::offered);
  REQUIRE(
      fixture.wagers
          ->set_test_role({.invocation = fixture.call(
                               WagerFixture::owner, "role-target", 1'030, true),
                           .wager_id = wager_id,
                           .role = WagerRole::target,
                           .next_id = fixture.ids()})
          .status == WagerMutationStatus::applied);
  fixture.deliver_sealed_offer(wager_id, WagerFixture::owner,
                               "self-offer-delivery", 1'035);
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::accept,
                      "test-accept", 1'040, true)
              .wager->state == WagerState::accepted_funded);
  REQUIRE(fixture.balance(WagerFixture::owner) == 80);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(scalar(*fixture.context,
                 "SELECT expected_posting_count FROM tarot_transaction WHERE "
                 "transaction_type='WAGER_ESCROW_FUND'") == 2);
  const auto fund_transaction = text_scalar(
      *fixture.context, "SELECT transaction_id FROM tarot_transaction WHERE "
                        "transaction_type='WAGER_ESCROW_FUND'");
  const auto generic_reversal = fixture.tarot->reverse(TarotReversalRequest{
      .invocation = {.user_id = WagerFixture::owner,
                     .guild_id = WagerFixture::guild,
                     .channel_id = WagerFixture::channel,
                     .display_name = "Owner",
                     .interaction_idempotency_key = "generic-wager-reversal",
                     .correlation_id = "wager-test",
                     .now_ms = 1'045},
      .original_transaction_id = fund_transaction,
      .reason = "Generic reversal must not detach escrow from its wager.",
      .transaction_id = fixture.next(),
      .event_id = fixture.next(),
      .first_posting_id = fixture.next(),
      .second_posting_id = fixture.next()});
  REQUIRE(generic_reversal.status == TarotMutationStatus::forbidden);
  REQUIRE(fixture.escrow_balance() == 20);
  REQUIRE(fixture.wagers->check_invariants().valid);

  REQUIRE(fixture.wagers
              ->force_test_deadline(
                  {.invocation = fixture.call(WagerFixture::owner,
                                              "force-reminder", 1'046, true),
                   .wager_id = wager_id,
                   .phase = WagerDeadlinePhase::reminder,
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture.wagers
              ->force_test_deadline({.invocation = fixture.call(
                                         WagerFixture::owner,
                                         "force-reminder-again", 1'047, true),
                                     .wager_id = wager_id,
                                     .phase = WagerDeadlinePhase::reminder,
                                     .next_id = fixture.ids()})
              .status == WagerMutationStatus::unchanged);
  REQUIRE(text_scalar(*fixture.context,
                      "SELECT job.state FROM scheduled_job job JOIN "
                      "tarot_wager_job link ON link.job_id=job.job_id WHERE "
                      "link.wager_id='" +
                          wager_id + "' AND link.phase='reminder'") ==
          "cancelled");

  REQUIRE(fixture.wagers
              ->set_test_role(
                  {.invocation = fixture.call(WagerFixture::owner,
                                              "role-creator", 1'050, true),
                   .wager_id = wager_id,
                   .role = WagerRole::creator,
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "test-creator-outcome", 1'060, true)
              .wager->state == WagerState::awaiting_resolution);
  REQUIRE(fixture.wagers
              ->set_test_role(
                  {.invocation = fixture.call(WagerFixture::owner,
                                              "role-target-2", 1'070, true),
                   .wager_id = wager_id,
                   .role = WagerRole::target,
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "test-target-outcome", 1'080, true)
              .wager->state == WagerState::resolved);
  REQUIRE(fixture.balance(WagerFixture::owner) == 100);
  REQUIRE(fixture.wagers
              ->cleanup_test_wager(
                  {.invocation = fixture.call(WagerFixture::owner, "cleanup",
                                              1'100, true),
                   .wager_id = wager_id,
                   .reason =
                       "Remove the simulated Fate effects after live testing.",
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture.balance(WagerFixture::owner) == 100);
  REQUIRE(fixture.escrow_balance() == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='TEST_REVERSAL'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_test_cleanup") == 2);
  REQUIRE(fixture.tarot->check_invariants().valid);
  REQUIRE(fixture.wagers->check_invariants().valid);
  REQUIRE(fixture.wagers
              ->cleanup_test_wager(
                  {.invocation = fixture.call(WagerFixture::owner,
                                              "cleanup-again", 1'200, true),
                   .wager_id = wager_id,
                   .reason =
                       "Remove the simulated Fate effects after live testing.",
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::unchanged);
}

TEST_CASE("database guards retain immutable private and scoped wager records",
          "[wager][database][privacy][invariant]") {
  WagerFixture fixture;
  const std::string malformed_uuid = "000000000000004000080000000000000000";
  REQUIRE(malformed_uuid.size() == 36);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager "
      "(wager_id,state,revision,guild_id,channel_id,creator_user_id,"
      "target_user_id,judge_user_id,visibility,resolution_policy,proposition,"
      "stake,evidence_instructions,outcome_window_ms,resolution_grace_ms,"
      "offer_duration_ms,offer_expires_at_ms,outcome_due_at_ms,"
      "resolution_grace_until_ms,winner_role,terminal_reason,"
      "judged_by_user_id,fund_transaction_id,settlement_transaction_id,"
      "is_test,created_at_ms,updated_at_ms,terminal_at_ms) VALUES ('" +
      malformed_uuid +
      "','draft',1,'10','20','30','31',NULL,'public','mutual',NULL,NULL,"
      "NULL,3600000,172800000,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,0,"
      "1000,1000,NULL)"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_history_cursor "
      "(cursor_id,user_id,item_count,next_cursor_id,created_at_ms,"
      "expires_at_ms) VALUES ('" +
      malformed_uuid + ",'30',0,NULL,1000,2000)"));
  const auto external_test_judge_id = fixture.next();
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager "
      "(wager_id,state,revision,guild_id,channel_id,creator_user_id,"
      "target_user_id,judge_user_id,visibility,resolution_policy,proposition,"
      "stake,evidence_instructions,outcome_window_ms,resolution_grace_ms,"
      "offer_duration_ms,offer_expires_at_ms,outcome_due_at_ms,"
      "resolution_grace_until_ms,winner_role,terminal_reason,"
      "judged_by_user_id,fund_transaction_id,settlement_transaction_id,"
      "is_test,created_at_ms,updated_at_ms,terminal_at_ms) VALUES ('" +
      external_test_judge_id +
      "','draft',1,'10','20','30','30','32','public','designated',NULL,NULL,"
      "NULL,3600000,172800000,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,1,"
      "1000,1000,NULL)"));
  const auto forged_offer_id = fixture.next();
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager "
      "(wager_id,state,revision,guild_id,channel_id,creator_user_id,"
      "target_user_id,judge_user_id,visibility,resolution_policy,proposition,"
      "stake,evidence_instructions,outcome_window_ms,resolution_grace_ms,"
      "offer_duration_ms,offer_expires_at_ms,outcome_due_at_ms,"
      "resolution_grace_until_ms,winner_role,terminal_reason,"
      "judged_by_user_id,fund_transaction_id,settlement_transaction_id,"
      "is_test,created_at_ms,updated_at_ms,terminal_at_ms) VALUES ('" +
      forged_offer_id +
      "','offered',1,'10','20','30','31',NULL,'public','mutual',"
      "'Forged offer',10,NULL,3600000,172800000,86400000,87400000,NULL,"
      "NULL,NULL,NULL,NULL,NULL,NULL,0,1000,1000,NULL)"));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager WHERE wager_id='" +
                     forged_offer_id + "'") == 0);

  const auto wager =
      fixture
          .offer(19, WagerVisibility::sealed, WagerResolutionPolicy::mutual,
                 std::nullopt, 1'000, "The \"quoted\" result remains sealed")
          .wager.value();
  const auto starting_grant = text_scalar(
      *fixture.context,
      "SELECT transaction_id FROM tarot_transaction WHERE "
      "transaction_type='STARTING_GRANT' ORDER BY ledger_sequence LIMIT 1");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_transfer "
      "(transfer_id,wager_id,transfer_kind,transaction_id,created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager.wager_id + "','fund','" + starting_grant +
      "',2000)"));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_transfer WHERE wager_id='" +
                     wager.wager_id + "'") == 0);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager_job SET expected_revision=expected_revision+1 "
      "WHERE wager_id='" +
      wager.wager_id + "' AND phase='offer_expiry'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_wager_job WHERE wager_id='" + wager.wager_id +
      "' AND phase='offer_expiry'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager SET proposition='tampered', revision=revision+1 "
      "WHERE wager_id='" +
      wager.wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager_action SET action='cancelled' WHERE wager_id='" +
      wager.wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_evidence "
      "(evidence_id,wager_id,actor_user_id,actor_role,body,idempotency_key,"
      "created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager.wager_id +
      "','32','target','private leak','direct-evidence',2000)"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_evidence "
      "(evidence_id,wager_id,actor_user_id,actor_role,body,idempotency_key,"
      "created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager.wager_id +
      "','30','creator','premature evidence','direct-valid-evidence',2000)"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_outcome "
      "(submission_id,wager_id,actor_user_id,actor_role,winner_role,"
      "idempotency_key,created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager.wager_id +
      "','30','creator','creator','direct-premature-outcome',2000)"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_void_consent "
      "(consent_id,wager_id,actor_user_id,actor_role,idempotency_key,created_"
      "at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager.wager_id +
      "','30','creator','direct-premature-void',2000)"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_wager_action "
      "(action_id,wager_id,wager_revision,actor_user_id,actor_role,action,"
      "event_id,"
      "reason,idempotency_key,occurred_at_ms) SELECT '" +
      fixture.next() + "','" + wager.wager_id +
      "',revision,'30','creator','cancelled',"
      "(SELECT event_id FROM event_journal WHERE "
      "aggregate_type='tarot_account' LIMIT 1),"
      "NULL,'direct-action',2000 FROM tarot_wager WHERE wager_id='" +
      wager.wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO outbox_message "
      "(outbox_id,kind,aggregate_type,aggregate_id,target_guild_id,"
      "target_channel_id,target_user_id,payload_json,state,attempt_count,"
      "max_attempts,available_at_ms,idempotency_key,provider_nonce,created_at_"
      "ms,updated_at_ms) "
      "SELECT '" +
      fixture.next() + "','discord.public.v1','tarot_wager','" +
      wager.wager_id +
      "','10','20',NULL,json_object('secret','The \"quoted\" result remains "
      "sealed'),"
      "'pending',0,5,2000,'direct-leak','nonce-direct-leak',2000,2000"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager SET state='cancelled', revision=revision+1, "
      "updated_at_ms=2100, terminal_at_ms=2100 WHERE wager_id='" +
      wager.wager_id + "'"));
  fixture.deliver_sealed_offer(wager.wager_id, WagerFixture::target,
                               "guard-offer-delivery", 2'090);
  REQUIRE(fixture.accept(wager.wager_id, "accept-guarded", 2'100).status ==
          WagerMutationStatus::applied);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager SET state='disputed', revision=revision+1, "
      "updated_at_ms=updated_at_ms+1 WHERE wager_id='" +
      wager.wager_id + "'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_wager SET outcome_due_at_ms=outcome_due_at_ms+1, "
      "revision=revision+1 WHERE wager_id='" +
      wager.wager_id + "'"));
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("wager invariants detect a committed draft missing its deadline link",
          "[wager][database][scheduler][invariant]") {
  WagerFixture fixture;
  const auto wager_id = fixture.next();
  fixture.context->connection().execute(
      "INSERT INTO tarot_wager "
      "(wager_id,state,revision,guild_id,channel_id,creator_user_id,"
      "target_user_id,judge_user_id,visibility,resolution_policy,proposition,"
      "stake,evidence_instructions,outcome_window_ms,resolution_grace_ms,"
      "offer_duration_ms,offer_expires_at_ms,outcome_due_at_ms,"
      "resolution_grace_until_ms,winner_role,terminal_reason,"
      "judged_by_user_id,fund_transaction_id,settlement_transaction_id,"
      "is_test,created_at_ms,updated_at_ms,terminal_at_ms) VALUES ('" +
      wager_id +
      "','draft',1,'10','20','30','31',NULL,'public','mutual',NULL,NULL,"
      "NULL,3600000,172800000,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,"
      "0,1000,1000,NULL)");
  const auto report = fixture.wagers->check_invariants();
  REQUIRE_FALSE(report.valid);
  REQUIRE(report.invalid_deadline_action_link_count == 1);
}

TEST_CASE("wager invariants detect a preexisting malformed transfer link",
          "[wager][database][ledger][invariant]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  const auto starting_grant = text_scalar(
      *fixture.context,
      "SELECT transaction_id FROM tarot_transaction WHERE "
      "transaction_type='STARTING_GRANT' ORDER BY ledger_sequence LIMIT 1");
  fixture.context->connection().execute(
      "DROP TRIGGER tarot_wager_transfer_guard");
  fixture.context->connection().execute(
      "INSERT INTO tarot_wager_transfer "
      "(transfer_id,wager_id,transfer_kind,transaction_id,created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager_id + "','fund','" + starting_grant +
      "',2000)");
  const auto report = fixture.wagers->check_invariants();
  REQUIRE_FALSE(report.valid);
  REQUIRE(report.malformed_transfer_count >= 1);
}

TEST_CASE("simulated-role controls fail closed when test mode is disabled",
          "[wager][test-mode][control][restart]") {
  WagerFixture fixture;
  auto draft = fixture.wagers->create_draft(WagerCreateRequest{
      .invocation =
          fixture.call(WagerFixture::owner, "control-draft", 1'000, true),
      .target_user_id = WagerFixture::owner,
      .judge_user_id = std::nullopt,
      .visibility = WagerVisibility::sealed,
      .resolution_policy = WagerResolutionPolicy::mutual,
      .outcome_window_ms = 3'600'000,
      .resolution_grace_ms = 172'800'000,
      .draft_expires_at_ms = 901'000,
      .is_test = true,
      .next_id = fixture.ids()});
  const auto form =
      token_from(draft.controls.front().custom_id, wager_form_prefix);
  const auto disabled_preview = fixture.wagers->preview(
      {.invocation =
           fixture.call(WagerFixture::owner, "disabled-preview", 1'005, false),
       .token_id = form,
       .proposition = "A disabled simulated preview must fail closed",
       .stake = 10,
       .evidence_instructions = std::nullopt,
       .offer_expiry_ms = 86'400'000,
       .next_id = fixture.ids()});
  REQUIRE(disabled_preview.status == WagerMutationStatus::forbidden);
  REQUIRE(disabled_preview.wager->revision == 1);
  const auto preview = fixture.wagers->preview(
      {.invocation =
           fixture.call(WagerFixture::owner, "control-preview", 1'010, true),
       .token_id = form,
       .proposition = "A simulated control must require active test mode",
       .stake = 10,
       .evidence_instructions = std::nullopt,
       .offer_expiry_ms = 86'400'000,
       .next_id = fixture.ids()});
  const auto wager_id = preview.wager->wager_id;
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::confirm,
                      "control-confirm", 1'020, true)
              .status == WagerMutationStatus::applied);
  const auto initial_card = text_scalar(
      *fixture.context,
      "SELECT json_extract(payload_json,'$.content') FROM outbox_message "
      "WHERE aggregate_id='" +
          wager_id +
          "' AND kind='discord.public.v1' ORDER BY created_at_ms LIMIT 1");
  REQUIRE(initial_card.starts_with("[TEST] <@30>"));

  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::accept,
                      "disabled-slash", 1'025, false)
              .status == WagerMutationStatus::forbidden);
  const auto hidden_exact = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "disabled-exact", 1'026, false),
       .wager_id = wager_id,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_exact.status == WagerMutationStatus::forbidden);
  REQUIRE(hidden_exact.wagers.empty());
  const auto hidden_list = fixture.wagers->history(
      {.invocation =
           fixture.call(WagerFixture::owner, "disabled-list", 1'027, false),
       .wager_id = std::nullopt,
       .cursor_id = std::nullopt,
       .next_id = fixture.ids()});
  REQUIRE(hidden_list.wagers.empty());
  fixture.deliver_sealed_offer(wager_id, WagerFixture::owner,
                               "control-offer-delivery", 1'035);
  const auto accept_token =
      text_scalar(*fixture.context,
                  "SELECT token_id FROM tarot_wager_control WHERE wager_id='" +
                      wager_id + "' AND action='accept' AND state='active'");
  const auto disabled = fixture.wagers->act(WagerActionRequest{
      .invocation =
          fixture.call(WagerFixture::owner, "disabled-control", 1'030, false),
      .wager_id = {},
      .token_id = accept_token,
      .action = WagerAction::cancel,
      .starting_fate = 100,
      .offer_expiry_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .next_id = fixture.ids()});
  REQUIRE(disabled.status == WagerMutationStatus::forbidden);
  REQUIRE(disabled.wager->state == WagerState::offered);
  REQUIRE(fixture.escrow_balance() == 0);
  const auto enabled = fixture.wagers->act(WagerActionRequest{
      .invocation =
          fixture.call(WagerFixture::owner, "enabled-control", 1'040, true),
      .wager_id = {},
      .token_id = accept_token,
      .action = WagerAction::cancel,
      .starting_fate = 100,
      .offer_expiry_ms = 86'400'000,
      .resolution_grace_ms = 172'800'000,
      .next_id = fixture.ids()});
  REQUIRE(enabled.status == WagerMutationStatus::applied);
  REQUIRE(enabled.wager->state == WagerState::accepted_funded);

  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "disabled-outcome", 1'050, false)
              .status == WagerMutationStatus::forbidden);
  REQUIRE(fixture.wagers
              ->add_evidence(
                  {.invocation = fixture.call(
                       WagerFixture::owner, "disabled-evidence", 1'051, false),
                   .wager_id = wager_id,
                   .token_id = std::nullopt,
                   .body = "This must not be persisted while test mode is off.",
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::forbidden);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_evidence WHERE wager_id='" +
                     wager_id + "'") == 0);
  REQUIRE(fixture.wagers
              ->set_test_role(
                  {.invocation = fixture.call(WagerFixture::owner,
                                              "role-creator-off", 1'052, true),
                   .wager_id = wager_id,
                   .role = WagerRole::creator,
                   .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::dispute,
                      "enabled-dispute", 1'053, true)
              .wager->state == WagerState::disputed);
  const auto disabled_judgment = fixture.wagers->judge(
      {.invocation =
           fixture.call(WagerFixture::owner, "disabled-judgment", 1'054, false),
       .wager_id = wager_id,
       .judgment = WagerJudgment::creator,
       .reason = "Owner authority must also fail closed with test mode off.",
       .next_id = fixture.ids()});
  REQUIRE(disabled_judgment.status == WagerMutationStatus::forbidden);
  REQUIRE(fixture.escrow_balance() == 20);
}

TEST_CASE("wager settlement sealing rejects a balanced nonparticipant payout",
          "[wager][database][ledger][shape]") {
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);
  fixture.provision(WagerFixture::judge, 100, "provision-judge");

  const auto event_id = fixture.next();
  SqliteDurableWorkRepository work{fixture.context};
  REQUIRE(work.append_event({
      .event_id = event_id,
      .event_type = "tarot.wager_resolved.v1",
      .aggregate_type = "tarot_wager",
      .aggregate_id = wager_id,
      .actor_user_id = WagerFixture::owner,
      .guild_id = WagerFixture::guild,
      .channel_id = WagerFixture::channel,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 1'200,
      .recorded_at_ms = 1'200,
      .correlation_id = "malformed-payout",
      .causation_id = std::nullopt,
      .idempotency_key = "event:malformed-payout",
      .payload_json = "{}",
  }));
  const auto transaction_id = fixture.next();
  fixture.context->connection().execute(
      "INSERT INTO tarot_transaction "
      "(transaction_id,transaction_type,state,expected_posting_count,event_id,"
      "idempotency_key,actor_user_id,reason,is_test,reversal_of_transaction_id,"
      "created_at_ms,committed_at_ms) VALUES ('" +
      transaction_id + "','WAGER_PAYOUT','prepared',2,'" + event_id +
      "','tx:malformed-payout','30',NULL,0,NULL,1200,NULL)");
  fixture.context->connection().execute(
      "INSERT INTO tarot_wager_transfer "
      "(transfer_id,wager_id,transfer_kind,transaction_id,created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager_id + "','payout','" + transaction_id +
      "',1200)");
  const auto escrow = text_scalar(
      *fixture.context,
      "SELECT account_id FROM tarot_account WHERE account_kind='ESCROW'");
  const auto judge_account =
      text_scalar(*fixture.context,
                  "SELECT account_id FROM tarot_account WHERE user_id='32'");
  fixture.context->connection().execute(
      "INSERT INTO tarot_posting "
      "(posting_id,transaction_id,account_id,amount,created_at_ms) VALUES ('" +
      fixture.next() + "','" + transaction_id + "','" + escrow + "',-20,1200)");
  fixture.context->connection().execute(
      "INSERT INTO tarot_posting "
      "(posting_id,transaction_id,account_id,amount,created_at_ms) VALUES ('" +
      fixture.next() + "','" + transaction_id + "','" + judge_account +
      "',20,1200)");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=1200 "
      "WHERE transaction_id='" +
      transaction_id + "'"));
  REQUIRE(
      text_scalar(*fixture.context,
                  "SELECT state FROM tarot_transaction WHERE transaction_id='" +
                      transaction_id + "'") == "prepared");
}

TEST_CASE("terminal wager metadata must match the immutable resolution audit",
          "[wager][database][resolution][audit]") {
  WagerFixture fixture;
  const auto wager_id =
      fixture
          .offer(10, WagerVisibility::public_offer,
                 WagerResolutionPolicy::designated, WagerFixture::judge)
          .wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept-terminal-audit", 1'100).status ==
          WagerMutationStatus::applied);

  auto &connection = fixture.context->connection();
  connection.execute("SAVEPOINT terminal_audit_mismatch");
  const auto event_id = fixture.next();
  connection.execute(
      "INSERT INTO event_journal "
      "(event_id,event_type,aggregate_type,aggregate_id,actor_user_id,guild_id,"
      "channel_id,source_message_id,occurred_at_ms,recorded_at_ms,"
      "correlation_id,causation_id,idempotency_key,payload_json) VALUES ('" +
      event_id + "','tarot.wager_resolved.v1','tarot_wager','" + wager_id +
      "','32','10','20',NULL,1200,1200,'terminal-audit-mismatch',NULL,"
      "'event:terminal-audit-mismatch','{}')");
  connection.execute(
      "INSERT INTO tarot_wager_action "
      "(action_id,wager_id,wager_revision,actor_user_id,actor_role,action,"
      "event_id,reason,idempotency_key,occurred_at_ms) SELECT '" +
      fixture.next() + "','" + wager_id + "',revision,'32','judge','judged','" +
      event_id +
      "','Canonical judgment reason.','action:terminal-audit-mismatch',1200 "
      "FROM tarot_wager WHERE wager_id='" +
      wager_id + "'");
  const auto transaction_id = fixture.next();
  connection.execute(
      "INSERT INTO tarot_transaction "
      "(transaction_id,transaction_type,state,expected_posting_count,event_id,"
      "idempotency_key,actor_user_id,reason,is_test,reversal_of_transaction_id,"
      "created_at_ms,committed_at_ms) VALUES ('" +
      transaction_id + "','WAGER_PAYOUT','prepared',2,'" + event_id +
      "','tx:terminal-audit-mismatch','32','Canonical judgment reason.',0,NULL,"
      "1200,NULL)");
  connection.execute(
      "INSERT INTO tarot_wager_transfer "
      "(transfer_id,wager_id,transfer_kind,transaction_id,created_at_ms) "
      "VALUES ('" +
      fixture.next() + "','" + wager_id + "','payout','" + transaction_id +
      "',1200)");
  const auto escrow = text_scalar(
      *fixture.context,
      "SELECT account_id FROM tarot_account WHERE account_kind='ESCROW'");
  const auto creator_account =
      text_scalar(*fixture.context,
                  "SELECT account_id FROM tarot_account WHERE user_id='30'");
  connection.execute(
      "INSERT INTO tarot_posting "
      "(posting_id,transaction_id,account_id,amount,created_at_ms) VALUES ('" +
      fixture.next() + "','" + transaction_id + "','" + escrow + "',-20,1200)");
  connection.execute(
      "INSERT INTO tarot_posting "
      "(posting_id,transaction_id,account_id,amount,created_at_ms) VALUES ('" +
      fixture.next() + "','" + transaction_id + "','" + creator_account +
      "',20,1200)");
  connection.execute(
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=1200 "
      "WHERE transaction_id='" +
      transaction_id + "'");
  connection.execute(
      "INSERT INTO tarot_wager_resolution "
      "(resolution_id,wager_id,result,authority,actor_user_id,reason,"
      "transaction_id,event_id,created_at_ms) VALUES ('" +
      fixture.next() + "','" + wager_id +
      "','creator','judge','32','Canonical judgment reason.','" +
      transaction_id + "','" + event_id + "',1200)");

  REQUIRE_THROWS(connection.execute(
      "UPDATE tarot_wager SET state='resolved',revision=revision+1,"
      "winner_role='creator',terminal_reason='Forged different reason.',"
      "judged_by_user_id='32',settlement_transaction_id='" +
      transaction_id +
      "',updated_at_ms=1200,terminal_at_ms=1200 WHERE wager_id='" + wager_id +
      "'"));
  REQUIRE_THROWS(connection.execute(
      "UPDATE tarot_wager SET state='resolved',revision=revision+1,"
      "winner_role='creator',terminal_reason='Canonical judgment reason.',"
      "judged_by_user_id='30',settlement_transaction_id='" +
      transaction_id +
      "',updated_at_ms=1200,terminal_at_ms=1200 WHERE wager_id='" + wager_id +
      "'"));
  connection.execute("ROLLBACK TO terminal_audit_mismatch");
  connection.execute("RELEASE terminal_audit_mismatch");
  REQUIRE(fixture.wagers->check_invariants().valid);
}

TEST_CASE("maximum private wager inputs use bounded complete fingerprints",
          "[wager][idempotency][bounds][privacy]") {
  WagerFixture fixture;
  auto draft = fixture.wagers->create_draft(WagerCreateRequest{
      .invocation = fixture.call(WagerFixture::owner, "max-draft", 1'000),
      .target_user_id = WagerFixture::target,
      .judge_user_id = std::nullopt,
      .visibility = WagerVisibility::sealed,
      .resolution_policy = WagerResolutionPolicy::mutual,
      .outcome_window_ms = 3'600'000,
      .resolution_grace_ms = 172'800'000,
      .draft_expires_at_ms = 901'000,
      .is_test = false,
      .next_id = fixture.ids()});
  const auto token =
      token_from(draft.controls.front().custom_id, wager_form_prefix);
  const std::string proposition(500, 'p');
  const std::string instructions(500, 'i');
  const auto preview = fixture.wagers->preview(
      {.invocation = fixture.call(WagerFixture::owner, "max-preview", 1'010),
       .token_id = token,
       .proposition = proposition,
       .stake = 10,
       .evidence_instructions = instructions,
       .offer_expiry_ms = 86'400'000,
       .next_id = fixture.ids()});
  REQUIRE(preview.status == WagerMutationStatus::applied);
  REQUIRE_THROWS(fixture.wagers->preview(
      {.invocation = fixture.call(WagerFixture::owner, "max-preview", 1'011),
       .token_id = token,
       .proposition = proposition,
       .stake = 10,
       .evidence_instructions = std::string(500, 'x'),
       .offer_expiry_ms = 86'400'000,
       .next_id = fixture.ids()}));
  const auto wager_id = preview.wager->wager_id;
  REQUIRE(fixture
              .action(WagerFixture::owner, wager_id, WagerAction::confirm,
                      "max-confirm", 1'020)
              .status == WagerMutationStatus::applied);
  fixture.deliver_sealed_offer(wager_id, WagerFixture::target,
                               "max-offer-delivery", 1'025);
  REQUIRE(fixture.accept(wager_id, "max-accept", 1'030).status ==
          WagerMutationStatus::applied);
  const std::string evidence(1'000, 'e');
  REQUIRE(fixture.wagers
              ->add_evidence({.invocation = fixture.call(WagerFixture::owner,
                                                         "max-evidence", 1'040),
                              .wager_id = wager_id,
                              .token_id = std::nullopt,
                              .body = evidence,
                              .next_id = fixture.ids()})
              .status == WagerMutationStatus::applied);
  REQUIRE(fixture
              .outcome(WagerFixture::owner, wager_id, WagerRole::creator,
                       "max-outcome", 1'050)
              .status == WagerMutationStatus::applied);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_wager_receipt WHERE wager_id='" +
                     wager_id + "' AND length(request_fingerprint)=35") == 6);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT count(*) FROM event_journal WHERE aggregate_id='" +
                 wager_id + "' AND (instr(payload_json,'" + proposition +
                 "')>0 OR instr(payload_json,'" + evidence +
                 "')>0 OR json_type(payload_json,'$.stake') IS NOT NULL "
                 "OR json_type(payload_json,'$.stake_each') IS NOT NULL "
                 "OR json_type(payload_json,'$.winner_role') IS NOT NULL)") ==
      0);
}

TEST_CASE(
    "wager card edits wait for a future-dated source after clock rollback",
    "[wager][outbox][edit][clock][ordering]") {
  WagerFixture fixture;
  const auto offered =
      fixture.offer(10, WagerVisibility::public_offer,
                    WagerResolutionPolicy::mutual, std::nullopt, 50'000);
  REQUIRE(
      fixture.accept(offered.wager->wager_id, "rollback-edit-accept", 20'000)
          .status == WagerMutationStatus::applied);

  SqliteDurableWorkRepository work{fixture.context};
  REQUIRE_FALSE(work.claim_due_outbox(20'001, 21'001, "rollback-edit-blocked",
                                      fixture.next(), true)
                    .has_value());
  REQUIRE(scalar(*fixture.context,
                 "SELECT attempt_count FROM outbox_message WHERE "
                 "aggregate_id='" +
                     offered.wager->wager_id +
                     "' AND kind='discord.message_edit.v1'") == 0);

  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{51}});
  const auto source = work.claim_due_outbox(
      50'020, 51'020, "rollback-edit-source", fixture.next(), true);
  REQUIRE(source.has_value());
  REQUIRE(source->kind == public_discord_outbox_kind);
  REQUIRE(work.mark_public_outbox_submitted(
              *source,
              {.wall_time_ms = 50'020,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              51'020) == WorkMutationStatus::applied);
  REQUIRE(work.complete_public_outbox(*source, DiscordId{2'000}, 50'020) ==
          WorkMutationStatus::applied);

  const auto edit = work.claim_due_outbox(50'021, 51'021, "rollback-edit-ready",
                                          fixture.next(), true);
  REQUIRE(edit.has_value());
  REQUIRE(edit->kind == wager_public_edit_outbox_kind);
  REQUIRE(edit->attempt_count == 1);
  REQUIRE(work.release_outbox(*edit, 50'021) == WorkMutationStatus::applied);
}

TEST_CASE("revisioned wager cards retry exact replacement edits safely",
          "[wager][outbox][edit][idempotency]") {
  using namespace std::chrono_literals;
  WagerFixture fixture;
  const auto wager_id = fixture.offer().wager->wager_id;
  REQUIRE(fixture.accept(wager_id, "accept", 1'100).status ==
          WagerMutationStatus::applied);

  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{2}});
  SqliteDurableWorkRepository work{fixture.context};
  const auto source =
      work.claim_due_outbox(2'000, 3'000, "source-setup", fixture.next(), true);
  REQUIRE(source.has_value());
  REQUIRE(source->kind == public_discord_outbox_kind);
  REQUIRE(work.mark_public_outbox_submitted(
              *source,
              {.wall_time_ms = 2'000,
               .elapsed_realtime_ms = fixture.clock.elapsed_realtime_ms(),
               .boot_session_id = std::string{fixture.clock.boot_session_id()}},
              3'000) == WorkMutationStatus::applied);
  REQUIRE(work.complete_public_outbox(*source, DiscordId{1'000}, 2'000) ==
          WorkMutationStatus::applied);

  test::FakeDiscord discord;
  discord.set_public_edit_result(DeliveryResult::unknown_outcome);
  discord.start({}, {}, {.version = 9, .commands = {}});
  test::FakePersistentIdGenerator lease_ids;
  test::FakeDiagnostics diagnostics;
  OutboxService outbox{work,
                       fixture.clock,
                       lease_ids,
                       diagnostics,
                       discord,
                       discord,
                       {.guild_id = WagerFixture::guild,
                        .primary_channel_id = WagerFixture::channel,
                        .owner_user_id = WagerFixture::owner},
                       "wager-edit-test",
                       16,
                       100ms};
  outbox.start();
  outbox.wake();
  REQUIRE(discord.wait_for_public_edit_count(1, 2s));
  REQUIRE(discord.public_edits().front().message_id == DiscordId{1'000});
  REQUIRE(discord.public_edits().front().message.buttons.empty());

  bool retry_pending{};
  for (std::size_t attempt = 0; attempt < 200 && !retry_pending; ++attempt) {
    retry_pending =
        scalar(*fixture.context,
               "SELECT count(*) FROM outbox_message WHERE "
               "aggregate_id='" +
                   wager_id +
                   "' AND kind='discord.message_edit.v1' AND state='pending' "
                   "AND last_error_code='discord_edit_unknown'") == 1;
    if (!retry_pending)
      std::this_thread::sleep_for(10ms);
  }
  REQUIRE(retry_pending);

  discord.set_public_edit_result(DeliveryResult::success);
  fixture.clock.set(std::chrono::sys_seconds{std::chrono::seconds{7}});
  outbox.wake();
  REQUIRE(discord.wait_for_public_edit_count(2, 2s));
  const auto edits = discord.public_edits();
  REQUIRE(edits[0].message_id == edits[1].message_id);
  REQUIRE(edits[0].message.content == edits[1].message.content);
  REQUIRE(edits[0].message.embed->description ==
          edits[1].message.embed->description);
  bool delivered{};
  for (std::size_t attempt = 0; attempt < 200 && !delivered; ++attempt) {
    delivered =
        scalar(*fixture.context,
               "SELECT count(*) FROM outbox_message WHERE "
               "aggregate_type='tarot_wager' AND state='delivered'") == 2;
    if (!delivered)
      std::this_thread::sleep_for(10ms);
  }
  REQUIRE(delivered);
  outbox.stop();
  discord.shutdown();
}
