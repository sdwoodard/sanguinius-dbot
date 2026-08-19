#include "sanguinius/chronicle.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_chronicle_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <barrier>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using sanguinius::ApplyApprovalRequest;
using sanguinius::ApprovalRenewalDispatch;
using sanguinius::ChronicleEntryStatus;
using sanguinius::ChronicleResultCode;
using sanguinius::ConfirmMemoryRequest;
using sanguinius::ContextAttachmentSnapshot;
using sanguinius::ContextMessageSnapshot;
using sanguinius::ContextUserSnapshot;
using sanguinius::CreateProposalRequest;
using sanguinius::EditProposalRequest;
using sanguinius::MemoryDraft;
using sanguinius::MemorySensitivity;
using sanguinius::MemoryVisibility;
using sanguinius::ProposalActionIds;
using sanguinius::ProposalControlMode;
using sanguinius::SubmitProposalRequest;
using sanguinius::persistence::Database;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SqliteChronicleRepository;
using sanguinius::persistence::SqliteCoreIdentityRepository;
using sanguinius::persistence::SqliteDurableWorkRepository;
using sanguinius::persistence::SqliteRepositoryContext;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

class ChronicleFixture {
public:
  ChronicleFixture() {
    {
      auto database = Database::open_migration(temporary.path(), 25ms);
      const Migrator migrator{sanguinius::persistence::production_migrations(),
                              {"test", "revision"},
                              clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 5);
    }
    context = std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path(), 25ms));
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({30, "Owner", "owner", false, 100});
    identities.ensure_user({31, "Proposer", "proposer", false, 100});
    identities.ensure_user({32, "Source", "source", false, 100});
    context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=1");
    chronicle = std::make_unique<SqliteChronicleRepository>(context);
    durable = std::make_unique<SqliteDurableWorkRepository>(context);
  }

  [[nodiscard]] CreateProposalRequest proposal(const std::size_t base = 100) {
    return CreateProposalRequest{
        .entry_id = uuid(base),
        .event_id = uuid(base + 1),
        .actions =
            ProposalActionIds{uuid(base + 2), uuid(base + 3), uuid(base + 4)},
        .source =
            ContextMessageSnapshot{
                .reference = {.message_id = 40,
                              .guild_id = 10,
                              .channel_id = 20},
                .author = ContextUserSnapshot{.user_id = 32,
                                              .username = "source",
                                              .display_name = "Source"},
                .content = "A bounded source statement.",
                .occurred_at_ms = 90,
                .mentioned_users = {},
                .attachments = {ContextAttachmentSnapshot{
                    .attachment_id = 41,
                    .filename = "proof.png",
                    .content_type = "image/png",
                    .byte_size = 123,
                    .width = 10,
                    .height = 20}}},
        .proposer_user_id = 31,
        .owner_user_id = 30,
        .title = "A useful heading",
        .body = "A concise Chronicle body.",
        .correlation_id = "correlation",
        .idempotency_key = "chronicle:proposal:40",
        .now_ms = 100,
        .action_expires_at_ms = 1'000'000};
  }

  [[nodiscard]] SubmitProposalRequest
  submission(const CreateProposalRequest &proposal,
             const std::size_t base = 200) {
    return SubmitProposalRequest{
        .token_id = proposal.actions.submit_token_id,
        .guild_id = 10,
        .channel_id = 20,
        .actor_user_id = proposal.proposer_user_id,
        .owner_user_id = 30,
        .proposer_approval_id = uuid(base),
        .submit_event_id = uuid(base + 1),
        .immediate_canon_event_id = uuid(base + 2),
        .reviewer_dispatches = {SubmitProposalRequest::ReviewerDispatch{
            .approval_id = uuid(base + 3),
            .notice_id = uuid(base + 4),
            .notice_open_token_id = uuid(base + 5),
            .approve_token_id = uuid(base + 6),
            .decline_token_id = uuid(base + 7),
            .notice_event_id = uuid(base + 8),
            .notice_outbox_id = uuid(base + 9)}},
        .correlation_id = "correlation",
        .interaction_idempotency_key = "chronicle:submit:50",
        .now_ms = 200,
        .notice_expires_at_ms = 604'800'200};
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteChronicleRepository> chronicle;
  std::unique_ptr<SqliteDurableWorkRepository> durable;
};

[[nodiscard]] std::int64_t scalar(SqliteRepositoryContext &context,
                                  const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_int64(0);
}

} // namespace

TEST_CASE("Chronicle source identity is permanent across canon and retraction",
          "[chronicle][repository][idempotency]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal();
  const auto created = fixture.chronicle->create_or_get_proposal(proposal);
  REQUIRE(created.code == ChronicleResultCode::created);
  REQUIRE(created.entry->attachments.size() == 1);
  REQUIRE(created.entry->attachments[0].filename == "proof.png");

  const auto submission = fixture.submission(proposal);
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.code == ChronicleResultCode::updated);
  REQUIRE(submitted.wake_outbox);
  REQUIRE(submitted.entry->status == ChronicleEntryStatus::proposed);

  sanguinius::persistence::SqlitePendingNoticeRepository notices{
      fixture.context};
  const auto reveal_key = std::string{"chronicle:notice:open"};
  const auto revealed =
      notices.open_next({.user_id = 32,
                         .interaction_idempotency_key = reveal_key,
                         .now_ms = 250});
  REQUIRE(revealed.status == sanguinius::OpenPendingNoticeStatus::opened);
  REQUIRE(revealed.notice->content.actions.size() == 2);
  REQUIRE(revealed.notice->content.body.find("A concise Chronicle body.") !=
          std::string::npos);
  REQUIRE(revealed.notice->content.body.find("Source author: `32`") !=
          std::string::npos);
  REQUIRE(revealed.notice->content.body.find("message `40`") !=
          std::string::npos);
  REQUIRE(revealed.notice->content.body.find("proof.png") != std::string::npos);
  REQUIRE(revealed.notice->content.body.find("image/png") != std::string::npos);
  REQUIRE(revealed.notice->content.body.find("123 bytes") != std::string::npos);
  REQUIRE(notices.confirm_open_delivery(reveal_key, 251) ==
          sanguinius::PendingNoticeMutationStatus::applied);

  const auto approved = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = submission.reviewer_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(300),
      .canon_event_id = uuid(301),
      .public_outbox_id = uuid(302),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:approve:51",
      .now_ms = 300});
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == ChronicleEntryStatus::canon);
  const auto recalled_by_source =
      fixture.chronicle->recall(31, "bounded source", 350, 5);
  REQUIRE(recalled_by_source.entries.size() == 1);
  REQUIRE(recalled_by_source.entries[0].entry_id == proposal.entry_id);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 2);

  fixture.chronicle =
      std::make_unique<SqliteChronicleRepository>(fixture.context);

  auto duplicate_request = fixture.proposal(400);
  duplicate_request.idempotency_key = "chronicle:proposal:duplicate";
  const auto duplicate =
      fixture.chronicle->create_or_get_proposal(duplicate_request);
  REQUIRE(duplicate.code == ChronicleResultCode::existing);
  REQUIRE(duplicate.entry->entry_id == proposal.entry_id);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM chronicle_entry") ==
          1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 2);

  const auto retracted = fixture.chronicle->retract_entry(
      {.entity_id = proposal.entry_id,
       .expected_revision = approved.entry->revision,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 31,
       .owner_user_id = 30,
       .event_id = uuid(500),
       .public_outbox_id = uuid(501),
       .correlation_id = "correlation",
       .interaction_idempotency_key = "chronicle:retract:52",
       .now_ms = 400});
  REQUIRE(retracted.entry->status == ChronicleEntryStatus::retracted);
  REQUIRE(retracted.wake_outbox);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 3);
  REQUIRE(fixture.chronicle->recall(31, "", 500, 5).entries.empty());
  REQUIRE(fixture.chronicle->timeline(std::nullopt, 500, 5).empty());

  const auto after_retraction =
      fixture.chronicle->create_or_get_proposal(duplicate_request);
  REQUIRE(after_retraction.code == ChronicleResultCode::existing);
  REQUIRE(after_retraction.entry->status == ChronicleEntryStatus::retracted);
}

TEST_CASE("proposal mutations replay only matching immutable requests",
          "[chronicle][repository][idempotency][token]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(1'400);
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);

  const EditProposalRequest edit{
      .token_id = proposal.actions.edit_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 31,
      .title = "Edited heading",
      .body = "Edited bounded body.",
      .type = sanguinius::ChronicleEntryType::deed,
      .visibility = sanguinius::ChronicleVisibility::shared,
      .tags = {"deed"},
      .event_id = uuid(1'500),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:edit:immutable",
      .now_ms = 150};
  REQUIRE(fixture.chronicle->edit_proposal(edit).code ==
          ChronicleResultCode::updated);
  REQUIRE(fixture.chronicle->edit_proposal(edit).code ==
          ChronicleResultCode::unchanged);
  auto conflicting_edit = edit;
  conflicting_edit.body = "Conflicting body.";
  REQUIRE_THROWS_AS(fixture.chronicle->edit_proposal(conflicting_edit),
                    sanguinius::persistence::DatabaseError);

  auto submission = fixture.submission(proposal, 1'600);
  submission.interaction_idempotency_key = "chronicle:submit:immutable";
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::updated);
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::unchanged);
  auto conflicting_submission = submission;
  conflicting_submission.interaction_idempotency_key =
      "chronicle:submit:different-delivery";
  REQUIRE(fixture.chronicle->submit_proposal(conflicting_submission).code ==
          ChronicleResultCode::invalid_token);
}

TEST_CASE("Chronicle consent is revalidated at submission and approval",
          "[chronicle][repository][consent][privacy]") {
  SECTION("shared source opts out before submission") {
    ChronicleFixture fixture;
    const auto proposal = fixture.proposal(4'500);
    REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
            ChronicleResultCode::created);
    fixture.context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='32'");

    const auto result =
        fixture.chronicle->submit_proposal(fixture.submission(proposal, 4'600));
    REQUIRE(result.code == ChronicleResultCode::opted_out);
    REQUIRE_FALSE(result.entry->submitted_at_ms.has_value());
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_approval") == 0);
    REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice") ==
            0);
  }

  SECTION("participant opts out before submission") {
    ChronicleFixture fixture;
    auto proposal = fixture.proposal(4'700);
    proposal.visibility = sanguinius::ChronicleVisibility::participant_only;
    proposal.source.mentioned_users.push_back(ContextUserSnapshot{
        .user_id = 30, .username = "owner", .display_name = "Owner"});
    REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
            ChronicleResultCode::created);
    fixture.context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='30'");

    auto submission = fixture.submission(proposal, 4'800);
    submission.reviewer_dispatches.push_back(
        SubmitProposalRequest::ReviewerDispatch{
            .approval_id = uuid(4'810),
            .notice_id = uuid(4'811),
            .notice_open_token_id = uuid(4'812),
            .approve_token_id = uuid(4'813),
            .decline_token_id = uuid(4'814),
            .notice_event_id = uuid(4'815),
            .notice_outbox_id = uuid(4'816)});
    const auto result = fixture.chronicle->submit_proposal(submission);
    REQUIRE(result.code == ChronicleResultCode::opted_out);
    REQUIRE_FALSE(result.entry->submitted_at_ms.has_value());
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_approval") == 0);
  }

  SECTION("source opts out before reviewer approval") {
    ChronicleFixture fixture;
    const auto proposal = fixture.proposal(4'900);
    REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
            ChronicleResultCode::created);
    const auto submission = fixture.submission(proposal, 5'000);
    REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
            ChronicleResultCode::updated);
    fixture.context->connection().execute(
        "UPDATE user_preference SET chronicle_opt_in=0 WHERE user_id='32'");

    const auto result = fixture.chronicle->apply_approval(ApplyApprovalRequest{
        .token_id = submission.reviewer_dispatches[0].approve_token_id,
        .guild_id = 10,
        .channel_id = 20,
        .actor_user_id = 32,
        .owner_user_id = 30,
        .action_event_id = uuid(5'100),
        .canon_event_id = uuid(5'101),
        .public_outbox_id = uuid(5'102),
        .correlation_id = "correlation",
        .interaction_idempotency_key = "chronicle:approve:opted-out",
        .now_ms = 300});
    REQUIRE(result.code == ChronicleResultCode::opted_out);
    REQUIRE(result.entry->status == ChronicleEntryStatus::proposed);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_approval WHERE "
                   "state='pending'") == 1);
  }
}

TEST_CASE(
    "expired durable Chronicle controls are distinguished from invalid tokens",
    "[chronicle][repository][token][expiry]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(3'700);
  proposal.action_expires_at_ms = 150;
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);

  REQUIRE(fixture.chronicle
              ->edit_proposal(EditProposalRequest{
                  .token_id = proposal.actions.edit_token_id,
                  .guild_id = 10,
                  .channel_id = 20,
                  .actor_user_id = 31,
                  .title = "Too late",
                  .body = "This edit arrived after expiry.",
                  .event_id = uuid(3'800),
                  .correlation_id = "correlation",
                  .interaction_idempotency_key = "chronicle:edit:expired",
                  .now_ms = 150})
              .code == ChronicleResultCode::expired);

  auto submission = fixture.submission(proposal, 3'810);
  submission.now_ms = 150;
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::expired);

  REQUIRE(fixture.chronicle
              ->apply_approval(ApplyApprovalRequest{
                  .token_id = proposal.actions.retract_token_id,
                  .guild_id = 10,
                  .channel_id = 20,
                  .actor_user_id = 31,
                  .owner_user_id = 30,
                  .action_event_id = uuid(3'830),
                  .canon_event_id = uuid(3'831),
                  .public_outbox_id = uuid(3'832),
                  .correlation_id = "correlation",
                  .interaction_idempotency_key = "chronicle:retract:expired",
                  .now_ms = 150})
              .code == ChronicleResultCode::expired);
}

TEST_CASE("explicit memory privacy expiry and journals stay bounded",
          "[chronicle][memory][privacy][restart]") {
  ChronicleFixture fixture;
  const auto memory_id = uuid(600);
  const auto job_id = uuid(602);
  const auto confirmed = fixture.chronicle->confirm_memory(ConfirmMemoryRequest{
      .memory_id = memory_id,
      .event_id = uuid(601),
      .expiry_job_id = job_id,
      .draft = MemoryDraft{.text = "A private remembered detail.",
                           .visibility = MemoryVisibility::self_only,
                           .sensitivity = MemorySensitivity::personal,
                           .expires_at_ms = 1'000,
                           .guild_id = 10,
                           .channel_id = 20,
                           .user_id = 31},
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:memory:60",
      .now_ms = 500});
  REQUIRE(confirmed.code == ChronicleResultCode::created);
  REQUIRE(confirmed.wake_scheduler);
  REQUIRE(fixture.chronicle->recall(31, "private", 600, 5).memories.size() ==
          1);
  REQUIRE(fixture.chronicle->recall(32, "private", 600, 5).memories.empty());
  const auto owner_items = fixture.chronicle->manageable(30, 30, "", 600, 5);
  REQUIRE(owner_items.size() == 1);
  REQUIRE(owner_items[0].entity_id == memory_id);
  REQUIRE(owner_items[0].summary == "Private memory (content hidden)");
  const auto creator_items = fixture.chronicle->manageable(31, 30, "", 600, 5);
  REQUIRE(creator_items.size() == 1);
  REQUIRE(creator_items[0].summary == "A private remembered detail.");

  auto public_payloads = fixture.context->connection().prepare(
      "SELECT count(*) FROM outbox_message WHERE payload_json LIKE "
      "'%private%' ");
  REQUIRE(public_payloads.step());
  REQUIRE(public_payloads.column_int64(0) == 0);
  auto event_payloads = fixture.context->connection().prepare(
      "SELECT count(*) FROM event_journal WHERE payload_json LIKE "
      "'%private%' ");
  REQUIRE(event_payloads.step());
  REQUIRE(event_payloads.column_int64(0) == 0);

  const auto claimed =
      fixture.durable->claim_due_job(1'000, 2'000, "test-instance", uuid(603));
  REQUIRE(claimed.has_value());
  fixture.chronicle =
      std::make_unique<SqliteChronicleRepository>(fixture.context);
  const auto early = fixture.chronicle->expire_memory(*claimed, uuid(604), 999);
  REQUIRE(early.code == ChronicleResultCode::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM scheduled_job WHERE state='pending'") ==
          1);
  const auto reclaimed =
      fixture.durable->claim_due_job(1'000, 2'000, "test-instance", uuid(605));
  REQUIRE(reclaimed.has_value());
  const auto expired =
      fixture.chronicle->expire_memory(*reclaimed, uuid(606), 1'000);
  REQUIRE(expired.code == ChronicleResultCode::updated);
  REQUIRE(fixture.chronicle->recall(31, "", 1'001, 5).memories.empty());
  REQUIRE(
      scalar(*fixture.context,
             "SELECT count(*) FROM scheduled_job WHERE state='completed'") ==
      1);

  const auto replayed =
      fixture.chronicle->expire_memory(*reclaimed, uuid(607), 1'001);
  REQUIRE(replayed.code == ChronicleResultCode::stale_revision);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE "
                 "event_type='chronicle.memory_expired.v1'") == 1);
}

TEST_CASE("owner test mode requires the second explicit self confirmation",
          "[chronicle][approval][owner-test]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(700);
  proposal.proposer_user_id = 30;
  proposal.source.author.user_id = 30;
  proposal.source.author.username = "owner";
  proposal.source.author.display_name = "Owner";
  proposal.owner_test = true;
  proposal.idempotency_key = "chronicle:proposal:owner-test";
  const auto created = fixture.chronicle->create_or_get_proposal(proposal);
  REQUIRE(created.entry->status == ChronicleEntryStatus::proposed);
  const auto edit_request = EditProposalRequest{
      .token_id = proposal.actions.edit_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .title = "Edited owner test",
      .body = "The marker remains automatic.",
      .tags = {"owner-test"},
      .event_id = uuid(750),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:edit:owner-test",
      .now_ms = 150};
  const auto edited = fixture.chronicle->edit_proposal(edit_request);
  REQUIRE(std::find(edited.entry->tags.begin(), edited.entry->tags.end(),
                    "owner-test") != edited.entry->tags.end());
  REQUIRE(fixture.chronicle->edit_proposal(edit_request).code ==
          ChronicleResultCode::unchanged);

  auto submission = fixture.submission(proposal, 800);
  submission.interaction_idempotency_key = "chronicle:submit:owner-test";
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.entry->status == ChronicleEntryStatus::proposed);
  REQUIRE_FALSE(submitted.became_canon);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM pending_notice WHERE payload_json "
                 "LIKE '%TEST DATA%'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE payload_json "
                 "LIKE '%TEST DATA%'") == 1);

  fixture.chronicle =
      std::make_unique<SqliteChronicleRepository>(fixture.context);

  auto renewal = fixture.proposal(850);
  renewal.proposer_user_id = 30;
  renewal.source.author.user_id = 30;
  renewal.source.author.username = "owner";
  renewal.source.author.display_name = "Owner";
  renewal.now_ms = submission.notice_expires_at_ms + 1;
  renewal.action_expires_at_ms = renewal.now_ms + 15 * 60 * 1'000;
  renewal.notice_expires_at_ms = renewal.now_ms + 7LL * 24 * 60 * 60 * 1'000;
  renewal.renewal_dispatches = {
      ApprovalRenewalDispatch{.notice_id = uuid(860),
                              .notice_open_token_id = uuid(861),
                              .approve_token_id = uuid(862),
                              .decline_token_id = uuid(863),
                              .notice_event_id = uuid(864),
                              .notice_outbox_id = uuid(865)}};
  renewal.idempotency_key = "chronicle:proposal:owner-test-renewal";
  const auto renewed = fixture.chronicle->create_or_get_proposal(renewal);
  REQUIRE(renewed.control_mode == ProposalControlMode::confirmations_reissued);
  REQUIRE(renewed.wake_outbox);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE "
                 "event_type='chronicle.notice_renewed.v1'") == 1);

  const auto approved = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = renewal.renewal_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .action_event_id = uuid(900),
      .canon_event_id = uuid(901),
      .public_outbox_id = uuid(902),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:approve:owner-test",
      .now_ms = renewal.now_ms + 1});
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == ChronicleEntryStatus::canon);
  REQUIRE(std::find(approved.entry->tags.begin(), approved.entry->tags.end(),
                    "owner-test") != approved.entry->tags.end());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE idempotency_key "
                 "LIKE 'outbox:chronicle:canon:%' AND payload_json LIKE "
                 "'%TEST DATA%'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE idempotency_key "
                 "LIKE 'outbox:chronicle:notice:%' AND state='cancelled'") ==
          2);
}

TEST_CASE(
    "exact memory references replay retraction without relisting terminals",
    "[chronicle][memory][retraction][idempotency]") {
  ChronicleFixture fixture;
  const auto memory_id = uuid(950);
  REQUIRE(
      fixture.chronicle
          ->confirm_memory(ConfirmMemoryRequest{
              .memory_id = memory_id,
              .event_id = uuid(951),
              .draft =
                  MemoryDraft{
                      .text = "A memory removed through the slash fallback.",
                      .visibility = MemoryVisibility::shared,
                      .sensitivity = MemorySensitivity::ordinary,
                      .guild_id = 10,
                      .channel_id = 20,
                      .user_id = 31},
              .correlation_id = "correlation",
              .interaction_idempotency_key = "chronicle:memory:slash-fallback",
              .now_ms = 500})
          .code == ChronicleResultCode::created);
  const auto active = fixture.chronicle->manageable(31, 30, memory_id, 600, 2);
  REQUIRE(active.size() == 1);
  REQUIRE(active[0].revision == 1);

  const auto retracted = fixture.chronicle->retract_memory(
      {.entity_id = memory_id,
       .expected_revision = active[0].revision,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 31,
       .owner_user_id = 30,
       .event_id = uuid(952),
       .public_outbox_id = uuid(953),
       .correlation_id = "correlation",
       .interaction_idempotency_key = "chronicle:retract-command:950",
       .now_ms = 600});
  REQUIRE(retracted.code == ChronicleResultCode::updated);
  REQUIRE(fixture.chronicle->manageable(31, 30, "", 601, 5).empty());

  const auto terminal = fixture.chronicle->manageable(
      31, 30, std::string_view{memory_id}.substr(0, 8), 601, 2);
  REQUIRE(terminal.size() == 1);
  REQUIRE(terminal[0].revision == 2);
  REQUIRE(fixture.chronicle
              ->retract_memory({.entity_id = memory_id,
                                .expected_revision = terminal[0].revision,
                                .guild_id = 10,
                                .channel_id = 20,
                                .actor_user_id = 31,
                                .owner_user_id = 30,
                                .event_id = uuid(954),
                                .public_outbox_id = uuid(955),
                                .correlation_id = "correlation",
                                .interaction_idempotency_key =
                                    "chronicle:retract-command:951",
                                .now_ms = 601})
              .code == ChronicleResultCode::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE "
                 "event_type='chronicle.memory_retracted.v1'") == 1);
}

TEST_CASE("owner can explicitly resolve a stale shared proposal",
          "[chronicle][approval][stale][owner]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(1'000);
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);
  const auto submission = fixture.submission(proposal, 1'100);
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::updated);

  auto owner_resolution = fixture.proposal(1'200);
  owner_resolution.proposer_user_id = 30;
  owner_resolution.owner_user_id = 30;
  owner_resolution.now_ms = submission.now_ms + 7LL * 24 * 60 * 60 * 1'000 + 1;
  owner_resolution.action_expires_at_ms =
      owner_resolution.now_ms + 15 * 60 * 1'000;
  owner_resolution.idempotency_key = "chronicle:proposal:owner-stale";
  const auto stale =
      fixture.chronicle->create_or_get_proposal(owner_resolution);
  REQUIRE(stale.code == ChronicleResultCode::existing);
  REQUIRE(stale.control_mode == ProposalControlMode::owner_stale_resolution);
  REQUIRE(stale.actions.has_value());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE idempotency_key "
                 "LIKE 'outbox:chronicle:notice:%' AND state='cancelled'") ==
          1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token WHERE state='active' "
                 "AND ((entity_type='chronicle_approval' AND entity_id='" +
                     submission.reviewer_dispatches[0].approval_id +
                     "') OR (entity_type='pending_notice' AND entity_id='" +
                     submission.reviewer_dispatches[0].notice_id + "'))") == 0);

  auto refreshed_request = fixture.proposal(1'250);
  refreshed_request.proposer_user_id = 30;
  refreshed_request.owner_user_id = 30;
  refreshed_request.now_ms = owner_resolution.action_expires_at_ms + 1;
  refreshed_request.action_expires_at_ms =
      refreshed_request.now_ms + 15 * 60 * 1'000;
  refreshed_request.idempotency_key = "chronicle:proposal:owner-stale-refresh";
  const auto refreshed =
      fixture.chronicle->create_or_get_proposal(refreshed_request);
  REQUIRE(refreshed.code == ChronicleResultCode::existing);
  REQUIRE(refreshed.control_mode ==
          ProposalControlMode::owner_stale_resolution);
  REQUIRE(refreshed.actions.has_value());
  REQUIRE(refreshed.actions->submit_token_id != stale.actions->submit_token_id);

  REQUIRE(fixture.chronicle
              ->apply_approval(ApplyApprovalRequest{
                  .token_id = stale.actions->submit_token_id,
                  .guild_id = 10,
                  .channel_id = 20,
                  .actor_user_id = 30,
                  .owner_user_id = 30,
                  .action_event_id = uuid(1'290),
                  .canon_event_id = uuid(1'291),
                  .public_outbox_id = uuid(1'292),
                  .correlation_id = "correlation",
                  .interaction_idempotency_key =
                      "chronicle:approve:old-owner-stale",
                  .now_ms = refreshed_request.now_ms})
              .code == ChronicleResultCode::invalid_token);

  const auto approved = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = refreshed.actions->submit_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .action_event_id = uuid(1'300),
      .canon_event_id = uuid(1'301),
      .public_outbox_id = uuid(1'302),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:approve:owner-stale",
      .now_ms = refreshed_request.now_ms + 1});
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == ChronicleEntryStatus::canon);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "approval_role='owner_stale' AND state='approved'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 2);
}

TEST_CASE("stale owner proposal reissues the distinct reviewer confirmation",
          "[chronicle][approval][stale][authorization]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(1'400);
  proposal.proposer_user_id = 30;
  proposal.idempotency_key = "chronicle:proposal:owner-proposer";
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);
  auto submission = fixture.submission(proposal, 1'500);
  submission.interaction_idempotency_key = "chronicle:submit:owner-proposer";
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::updated);

  auto stale_attempt = fixture.proposal(1'600);
  stale_attempt.proposer_user_id = 30;
  stale_attempt.owner_user_id = 30;
  stale_attempt.now_ms = submission.now_ms + 7LL * 24 * 60 * 60 * 1'000 + 1;
  stale_attempt.action_expires_at_ms = stale_attempt.now_ms + 15 * 60 * 1'000;
  stale_attempt.notice_expires_at_ms =
      stale_attempt.now_ms + 7LL * 24 * 60 * 60 * 1'000;
  stale_attempt.renewal_dispatches = {
      ApprovalRenewalDispatch{.notice_id = uuid(1'610),
                              .notice_open_token_id = uuid(1'611),
                              .approve_token_id = uuid(1'612),
                              .decline_token_id = uuid(1'613),
                              .notice_event_id = uuid(1'614),
                              .notice_outbox_id = uuid(1'615)}};
  stale_attempt.idempotency_key = "chronicle:proposal:owner-self-stale-attempt";
  const auto stale = fixture.chronicle->create_or_get_proposal(stale_attempt);
  REQUIRE(stale.code == ChronicleResultCode::existing);
  REQUIRE(stale.control_mode == ProposalControlMode::confirmations_reissued);
  REQUIRE_FALSE(stale.actions.has_value());
  REQUIRE(stale.wake_outbox);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "approval_role='owner_stale'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "state='pending' AND reviewer_user_id='32'") == 1);
}

TEST_CASE("participant-only owner tests require explicit self confirmation",
          "[chronicle][approval][participant][owner-test]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(1'700);
  proposal.proposer_user_id = 30;
  proposal.source.author.user_id = 30;
  proposal.source.author.username = "owner";
  proposal.source.author.display_name = "Owner";
  proposal.visibility = sanguinius::ChronicleVisibility::participant_only;
  proposal.owner_test = true;
  proposal.idempotency_key = "chronicle:proposal:participant-owner-test";
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);

  auto submission = fixture.submission(proposal, 1'800);
  submission.interaction_idempotency_key =
      "chronicle:submit:participant-owner-test";
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.code == ChronicleResultCode::updated);
  REQUIRE_FALSE(submitted.became_canon);
  REQUIRE(submitted.entry->status == ChronicleEntryStatus::proposed);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "approval_role='owner_test' AND reviewer_user_id='30' AND "
                 "state='pending'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);

  const auto approved = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = submission.reviewer_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .action_event_id = uuid(1'900),
      .canon_event_id = uuid(1'901),
      .public_outbox_id = uuid(1'902),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:approve:participant-owner-test",
      .now_ms = 300});
  REQUIRE(approved.became_canon);
  REQUIRE(approved.entry->status == ChronicleEntryStatus::canon);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("participant-only canon requires every participant and stays private",
          "[chronicle][approval][participant][privacy]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(1'700);
  proposal.visibility = sanguinius::ChronicleVisibility::participant_only;
  proposal.source.mentioned_users.push_back(ContextUserSnapshot{
      .user_id = 30, .username = "owner", .display_name = "Owner"});
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);
  auto submission = fixture.submission(proposal, 1'800);
  submission.reviewer_dispatches.push_back(
      SubmitProposalRequest::ReviewerDispatch{.approval_id = uuid(1'810),
                                              .notice_id = uuid(1'811),
                                              .notice_open_token_id =
                                                  uuid(1'812),
                                              .approve_token_id = uuid(1'813),
                                              .decline_token_id = uuid(1'814),
                                              .notice_event_id = uuid(1'815),
                                              .notice_outbox_id = uuid(1'816)});
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.entry->status == ChronicleEntryStatus::proposed);
  REQUIRE_FALSE(submitted.wake_outbox);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);

  const auto owner_approval =
      fixture.chronicle->apply_approval(ApplyApprovalRequest{
          .token_id = submission.reviewer_dispatches[0].approve_token_id,
          .guild_id = 10,
          .channel_id = 20,
          .actor_user_id = 30,
          .owner_user_id = 30,
          .action_event_id = uuid(1'820),
          .canon_event_id = uuid(1'821),
          .public_outbox_id = uuid(1'822),
          .correlation_id = "correlation",
          .interaction_idempotency_key = "chronicle:participant:owner",
          .now_ms = 300});
  REQUIRE_FALSE(owner_approval.became_canon);
  REQUIRE(owner_approval.entry->status == ChronicleEntryStatus::proposed);
  auto wrong_participant = ApplyApprovalRequest{
      .token_id = submission.reviewer_dispatches[1].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 30,
      .owner_user_id = 30,
      .action_event_id = uuid(1'823),
      .canon_event_id = uuid(1'824),
      .public_outbox_id = uuid(1'825),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:participant:wrong",
      .now_ms = 301};
  REQUIRE(fixture.chronicle->apply_approval(wrong_participant).code ==
          ChronicleResultCode::invalid_token);

  wrong_participant.actor_user_id = 32;
  wrong_participant.interaction_idempotency_key =
      "chronicle:participant:source";
  const auto canon = fixture.chronicle->apply_approval(wrong_participant);
  REQUIRE(canon.became_canon);
  REQUIRE(canon.entry->status == ChronicleEntryStatus::canon);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("expired participant confirmation is reissued across restart",
          "[chronicle][approval][participant][expiry][restart]") {
  ChronicleFixture fixture;
  auto proposal = fixture.proposal(2'900);
  proposal.visibility = sanguinius::ChronicleVisibility::participant_only;
  proposal.source.mentioned_users.push_back(ContextUserSnapshot{
      .user_id = 30, .username = "owner", .display_name = "Owner"});
  proposal.idempotency_key = "chronicle:proposal:participant-renewal";
  REQUIRE(fixture.chronicle->create_or_get_proposal(proposal).code ==
          ChronicleResultCode::created);

  auto submission = fixture.submission(proposal, 3'000);
  submission.interaction_idempotency_key =
      "chronicle:submit:participant-renewal";
  submission.reviewer_dispatches.push_back(
      SubmitProposalRequest::ReviewerDispatch{.approval_id = uuid(3'010),
                                              .notice_id = uuid(3'011),
                                              .notice_open_token_id =
                                                  uuid(3'012),
                                              .approve_token_id = uuid(3'013),
                                              .decline_token_id = uuid(3'014),
                                              .notice_event_id = uuid(3'015),
                                              .notice_outbox_id = uuid(3'016)});
  REQUIRE(fixture.chronicle->submit_proposal(submission).code ==
          ChronicleResultCode::updated);

  const auto owner_approval =
      fixture.chronicle->apply_approval(ApplyApprovalRequest{
          .token_id = submission.reviewer_dispatches[0].approve_token_id,
          .guild_id = 10,
          .channel_id = 20,
          .actor_user_id = 30,
          .owner_user_id = 30,
          .action_event_id = uuid(3'020),
          .canon_event_id = uuid(3'021),
          .public_outbox_id = uuid(3'022),
          .correlation_id = "correlation",
          .interaction_idempotency_key = "chronicle:participant-renewal:owner",
          .now_ms = 300});
  REQUIRE_FALSE(owner_approval.became_canon);

  fixture.chronicle =
      std::make_unique<SqliteChronicleRepository>(fixture.context);
  auto renewal = fixture.proposal(3'100);
  renewal.visibility = sanguinius::ChronicleVisibility::participant_only;
  renewal.proposer_user_id = proposal.proposer_user_id;
  renewal.now_ms = submission.notice_expires_at_ms + 1;
  renewal.action_expires_at_ms = renewal.now_ms + 15 * 60 * 1'000;
  renewal.notice_expires_at_ms = renewal.now_ms + 7LL * 24 * 60 * 60 * 1'000;
  renewal.renewal_dispatches = {
      ApprovalRenewalDispatch{.notice_id = uuid(3'110),
                              .notice_open_token_id = uuid(3'111),
                              .approve_token_id = uuid(3'112),
                              .decline_token_id = uuid(3'113),
                              .notice_event_id = uuid(3'114),
                              .notice_outbox_id = uuid(3'115)}};
  renewal.idempotency_key = "chronicle:proposal:participant-renewal-reopen";
  const auto renewed = fixture.chronicle->create_or_get_proposal(renewal);
  REQUIRE(renewed.code == ChronicleResultCode::existing);
  REQUIRE(renewed.control_mode == ProposalControlMode::confirmations_reissued);
  REQUIRE_FALSE(renewed.actions.has_value());
  REQUIRE_FALSE(renewed.wake_outbox);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE "
                 "event_type='chronicle.notice_renewed.v1'") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
  REQUIRE(
      scalar(*fixture.context,
             "SELECT count(*) FROM pending_notice WHERE state='cancelled'") >=
      1);

  const auto expired_old =
      fixture.chronicle->apply_approval(ApplyApprovalRequest{
          .token_id = submission.reviewer_dispatches[1].approve_token_id,
          .guild_id = 10,
          .channel_id = 20,
          .actor_user_id = 32,
          .owner_user_id = 30,
          .action_event_id = uuid(3'120),
          .canon_event_id = uuid(3'121),
          .public_outbox_id = uuid(3'122),
          .correlation_id = "correlation",
          .interaction_idempotency_key =
              "chronicle:participant-renewal:expired-old",
          .now_ms = renewal.now_ms + 1});
  REQUIRE(expired_old.code == ChronicleResultCode::invalid_token);

  const auto canon = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = renewal.renewal_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(3'130),
      .canon_event_id = uuid(3'131),
      .public_outbox_id = uuid(3'132),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:participant-renewal:source",
      .now_ms = renewal.now_ms + 1});
  REQUIRE(canon.became_canon);
  REQUIRE(canon.entry->status == ChronicleEntryStatus::canon);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") == 0);
}

TEST_CASE("a required reviewer decline retracts the proposal once",
          "[chronicle][approval][decline][idempotency]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(1'900);
  static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
  const auto submission = fixture.submission(proposal, 2'000);
  static_cast<void>(fixture.chronicle->submit_proposal(submission));
  const ApplyApprovalRequest decline{
      .token_id = submission.reviewer_dispatches[0].decline_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(2'100),
      .canon_event_id = uuid(2'101),
      .public_outbox_id = uuid(2'102),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:decline:required",
      .now_ms = 300};
  const auto retracted = fixture.chronicle->apply_approval(decline);
  REQUIRE(retracted.entry->status == ChronicleEntryStatus::retracted);
  REQUIRE_FALSE(retracted.became_canon);
  REQUIRE(fixture.chronicle->apply_approval(decline).code ==
          ChronicleResultCode::unchanged);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE "
                 "event_type='chronicle.entry_retracted.v1'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "state='pending'") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice WHERE "
                                   "state IN ('pending','opened')") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token WHERE state='active' "
                 "AND entity_type IN ('chronicle_approval',"
                 "'pending_notice')") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "idempotency_key LIKE 'outbox:chronicle:notice:%' AND "
                 "state <> 'cancelled'") == 0);
}

TEST_CASE("retracting a submitted proposal revokes approval artifacts",
          "[chronicle][approval][retraction][privacy]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(5'200);
  static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
  const auto submission = fixture.submission(proposal, 5'300);
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.code == ChronicleResultCode::updated);

  const auto retracted = fixture.chronicle->retract_entry(
      {.entity_id = proposal.entry_id,
       .expected_revision = submitted.entry->revision,
       .guild_id = 10,
       .channel_id = 20,
       .actor_user_id = 31,
       .owner_user_id = 30,
       .event_id = uuid(5'400),
       .public_outbox_id = uuid(5'401),
       .correlation_id = "correlation",
       .interaction_idempotency_key = "chronicle:retract:submitted",
       .now_ms = 300});
  REQUIRE(retracted.code == ChronicleResultCode::updated);
  REQUIRE(retracted.entry->status == ChronicleEntryStatus::retracted);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM chronicle_approval WHERE "
                 "state='pending'") == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice WHERE "
                                   "state IN ('pending','opened')") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token WHERE state='active' "
                 "AND entity_type IN ('chronicle_entry',"
                 "'chronicle_approval','pending_notice')") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "idempotency_key LIKE 'outbox:chronicle:notice:%' AND "
                 "state <> 'cancelled'") == 0);

  REQUIRE(
      fixture.chronicle
          ->apply_approval(ApplyApprovalRequest{
              .token_id = submission.reviewer_dispatches[0].approve_token_id,
              .guild_id = 10,
              .channel_id = 20,
              .actor_user_id = 32,
              .owner_user_id = 30,
              .action_event_id = uuid(5'410),
              .canon_event_id = uuid(5'411),
              .public_outbox_id = uuid(5'412),
              .correlation_id = "correlation",
              .interaction_idempotency_key =
                  "chronicle:approve:after-retraction",
              .now_ms = 301})
          .code == ChronicleResultCode::invalid_token);
}

TEST_CASE("concurrent approval and retraction produce one terminal outcome",
          "[chronicle][approval][retraction][concurrency]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(3'300);
  static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
  const auto submission = fixture.submission(proposal, 3'400);
  const auto submitted = fixture.chronicle->submit_proposal(submission);
  REQUIRE(submitted.entry.has_value());

  const ApplyApprovalRequest approval{
      .token_id = submission.reviewer_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(3'500),
      .canon_event_id = uuid(3'501),
      .public_outbox_id = uuid(3'502),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:race:approve",
      .now_ms = 300};
  const sanguinius::RetractItemRequest retraction{
      .entity_id = proposal.entry_id,
      .expected_revision = submitted.entry->revision,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 31,
      .owner_user_id = 30,
      .event_id = uuid(3'510),
      .public_outbox_id = uuid(3'511),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:race:retract",
      .now_ms = 300};

  std::barrier gate{3};
  std::optional<sanguinius::ChronicleMutationResult> approval_result;
  std::optional<sanguinius::ChronicleMutationResult> retraction_result;
  std::exception_ptr approval_error;
  std::exception_ptr retraction_error;
  std::thread approve_thread{[&] {
    gate.arrive_and_wait();
    try {
      approval_result = fixture.chronicle->apply_approval(approval);
    } catch (...) {
      approval_error = std::current_exception();
    }
  }};
  std::thread retract_thread{[&] {
    gate.arrive_and_wait();
    try {
      retraction_result = fixture.chronicle->retract_entry(retraction);
    } catch (...) {
      retraction_error = std::current_exception();
    }
  }};
  gate.arrive_and_wait();
  approve_thread.join();
  retract_thread.join();

  REQUIRE_FALSE(approval_error);
  REQUIRE_FALSE(retraction_error);
  REQUIRE(approval_result.has_value());
  REQUIRE(retraction_result.has_value());

  auto duplicate = fixture.proposal(3'600);
  duplicate.idempotency_key = "chronicle:race:inspect";
  const auto final = fixture.chronicle->create_or_get_proposal(duplicate);
  REQUIRE(final.entry.has_value());
  REQUIRE((final.entry->status == ChronicleEntryStatus::canon ||
           final.entry->status == ChronicleEntryStatus::retracted));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type IN ("
                 "'chronicle.entry_canonized.v1',"
                 "'chronicle.entry_retracted.v1')") == 1);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") <= 2);
}

TEST_CASE(
    "recall globally orders entries and memories before applying its limit",
    "[chronicle][repository][recall][ordering]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(4'000);
  static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
  const auto submission = fixture.submission(proposal, 4'100);
  static_cast<void>(fixture.chronicle->submit_proposal(submission));
  const auto canon = fixture.chronicle->apply_approval(ApplyApprovalRequest{
      .token_id = submission.reviewer_dispatches[0].approve_token_id,
      .guild_id = 10,
      .channel_id = 20,
      .actor_user_id = 32,
      .owner_user_id = 30,
      .action_event_id = uuid(4'200),
      .canon_event_id = uuid(4'201),
      .public_outbox_id = uuid(4'202),
      .correlation_id = "correlation",
      .interaction_idempotency_key = "chronicle:approve:recall-order",
      .now_ms = 300});
  REQUIRE(canon.became_canon);

  const auto memory_id = uuid(4'300);
  REQUIRE(
      fixture.chronicle
          ->confirm_memory(ConfirmMemoryRequest{
              .memory_id = memory_id,
              .event_id = uuid(4'301),
              .draft = MemoryDraft{.text = "The newer explicit memory.",
                                   .visibility = MemoryVisibility::shared,
                                   .sensitivity = MemorySensitivity::ordinary,
                                   .guild_id = 10,
                                   .channel_id = 20,
                                   .user_id = 31},
              .correlation_id = "correlation",
              .interaction_idempotency_key = "chronicle:memory:recall-order",
              .now_ms = 500})
          .code == ChronicleResultCode::created);

  const auto recalled = fixture.chronicle->recall(31, "", 600, 1);
  REQUIRE(recalled.entries.empty());
  REQUIRE(recalled.memories.size() == 1);
  REQUIRE(recalled.memories[0].memory_id == memory_id);
  REQUIRE(recalled.ordered_items.size() == 1);
  REQUIRE(std::holds_alternative<sanguinius::ExplicitMemory>(
      recalled.ordered_items[0]));
}

TEST_CASE("Chronicle compound mutations roll back every durable side effect",
          "[chronicle][repository][transaction][rollback]") {
  SECTION("proposal event failure") {
    ChronicleFixture fixture;
    fixture.context->connection().execute(
        "CREATE TRIGGER fail_chronicle_event BEFORE INSERT ON event_journal "
        "WHEN NEW.event_type='chronicle.proposal_created.v1' BEGIN "
        "SELECT RAISE(ABORT,'injected proposal event failure'); END");
    REQUIRE_THROWS_AS(
        fixture.chronicle->create_or_get_proposal(fixture.proposal(2'200)),
        sanguinius::persistence::DatabaseError);
    REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM chronicle_entry") ==
            0);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM interaction_token") == 0);
  }

  SECTION("approval notice failure") {
    ChronicleFixture fixture;
    const auto proposal = fixture.proposal(2'300);
    static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
    fixture.context->connection().execute(
        "CREATE TRIGGER fail_chronicle_notice BEFORE INSERT ON pending_notice "
        "BEGIN SELECT RAISE(ABORT,'injected notice failure'); END");
    REQUIRE_THROWS_AS(
        fixture.chronicle->submit_proposal(fixture.submission(proposal, 2'400)),
        sanguinius::persistence::DatabaseError);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_entry WHERE "
                   "submitted_at_ms IS NOT NULL") == 0);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_approval") == 0);
    REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM pending_notice") ==
            0);
    REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM outbox_message") ==
            0);
  }

  SECTION("memory expiry job failure") {
    ChronicleFixture fixture;
    fixture.context->connection().execute(
        "CREATE TRIGGER fail_memory_job BEFORE INSERT ON scheduled_job "
        "WHEN NEW.job_type='chronicle.memory-expire.v1' BEGIN "
        "SELECT RAISE(ABORT,'injected job failure'); END");
    REQUIRE_THROWS_AS(
        fixture.chronicle->confirm_memory(ConfirmMemoryRequest{
            .memory_id = uuid(2'500),
            .event_id = uuid(2'501),
            .expiry_job_id = uuid(2'502),
            .draft = MemoryDraft{.text = "Expiring atomically.",
                                 .expires_at_ms = 1'000,
                                 .guild_id = 10,
                                 .channel_id = 20,
                                 .user_id = 31},
            .correlation_id = "correlation",
            .interaction_idempotency_key = "chronicle:memory:rollback",
            .now_ms = 500}),
        sanguinius::persistence::DatabaseError);
    REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM memory") == 0);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM event_journal WHERE "
                   "event_type='chronicle.memory_confirmed.v1'") == 0);
  }

  SECTION("canon public outbox failure") {
    ChronicleFixture fixture;
    const auto proposal = fixture.proposal(2'600);
    static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
    const auto submission = fixture.submission(proposal, 2'700);
    static_cast<void>(fixture.chronicle->submit_proposal(submission));
    fixture.context->connection().execute(
        "CREATE TRIGGER fail_canon_outbox BEFORE INSERT ON outbox_message "
        "WHEN NEW.idempotency_key LIKE 'outbox:chronicle:canon:%' BEGIN "
        "SELECT RAISE(ABORT,'injected canon outbox failure'); END");
    REQUIRE_THROWS_AS(
        fixture.chronicle->apply_approval(ApplyApprovalRequest{
            .token_id = submission.reviewer_dispatches[0].approve_token_id,
            .guild_id = 10,
            .channel_id = 20,
            .actor_user_id = 32,
            .owner_user_id = 30,
            .action_event_id = uuid(2'800),
            .canon_event_id = uuid(2'801),
            .public_outbox_id = uuid(2'802),
            .correlation_id = "correlation",
            .interaction_idempotency_key = "chronicle:canon:rollback",
            .now_ms = 300}),
        sanguinius::persistence::DatabaseError);
    REQUIRE(
        scalar(*fixture.context,
               "SELECT count(*) FROM chronicle_entry WHERE status='canon'") ==
        0);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM chronicle_approval WHERE "
                   "state='approved' AND approval_role<>'proposer'") == 0);
    REQUIRE(scalar(*fixture.context,
                   "SELECT count(*) FROM event_journal WHERE "
                   "event_type='chronicle.entry_canonized.v1'") == 0);
  }
}

TEST_CASE("schema enforces Chronicle byte and collection bounds",
          "[chronicle][repository][bounds][schema]") {
  ChronicleFixture fixture;
  const auto proposal = fixture.proposal(2'900);
  static_cast<void>(fixture.chronicle->create_or_get_proposal(proposal));
  for (const auto *tag : {"one", "two", "three", "four", "five"}) {
    auto insert = fixture.context->connection().prepare(
        "INSERT INTO chronicle_tag (entry_id,tag) VALUES (?,?)");
    insert.bind(1, proposal.entry_id);
    insert.bind(2, tag);
    insert.execute();
  }
  REQUIRE_THROWS_AS(fixture.context->connection().execute(
                        "INSERT INTO chronicle_tag (entry_id,tag) VALUES ('" +
                        proposal.entry_id + "','six')"),
                    sanguinius::persistence::DatabaseError);
  const auto oversized_utf8 = std::string(51, '\xC3') + std::string(51, '\xA9');
  auto update = fixture.context->connection().prepare(
      "UPDATE chronicle_entry SET title=? WHERE entry_id=?");
  update.bind(1, oversized_utf8);
  update.bind(2, proposal.entry_id);
  REQUIRE_THROWS_AS(update.execute(), sanguinius::persistence::DatabaseError);
  auto blank_title = fixture.context->connection().prepare(
      "UPDATE chronicle_entry SET title=? WHERE entry_id=?");
  blank_title.bind(1, " \t\r\n");
  blank_title.bind(2, proposal.entry_id);
  REQUIRE_THROWS_AS(blank_title.execute(),
                    sanguinius::persistence::DatabaseError);

  const auto memory_id = uuid(3'100);
  REQUIRE(
      fixture.chronicle
          ->confirm_memory(ConfirmMemoryRequest{
              .memory_id = memory_id,
              .event_id = uuid(3'101),
              .draft = MemoryDraft{.text = "A valid memory.",
                                   .guild_id = 10,
                                   .channel_id = 20,
                                   .user_id = 31},
              .correlation_id = "correlation",
              .interaction_idempotency_key = "chronicle:memory:schema-bounds",
              .now_ms = 500})
          .code == ChronicleResultCode::created);
  auto blank_memory = fixture.context->connection().prepare(
      "UPDATE memory SET text=? WHERE memory_id=?");
  blank_memory.bind(1, " \t\r\n");
  blank_memory.bind(2, memory_id);
  REQUIRE_THROWS_AS(blank_memory.execute(),
                    sanguinius::persistence::DatabaseError);

  std::vector<std::string> attachment_columns;
  auto columns = fixture.context->connection().prepare(
      "PRAGMA table_info(chronicle_attachment)");
  while (columns.step())
    attachment_columns.push_back(columns.column_text(1));
  REQUIRE(std::find(attachment_columns.begin(), attachment_columns.end(),
                    "url") == attachment_columns.end());
  REQUIRE(std::find(attachment_columns.begin(), attachment_columns.end(),
                    "proxy_url") == attachment_columns.end());
  REQUIRE(std::find(attachment_columns.begin(), attachment_columns.end(),
                    "attachment_bytes") == attachment_columns.end());
}
