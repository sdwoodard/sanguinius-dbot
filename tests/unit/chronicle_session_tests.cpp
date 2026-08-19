#include "sanguinius/chronicle.hpp"
#include "sanguinius/chronicle_sessions.hpp"

#include "support/fake_ai_client.hpp"
#include "support/fake_chronicle_session_repository.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_diagnostics.hpp"
#include "support/fake_durable_work_repository.hpp"
#include "support/fake_id_generator.hpp"
#include "support/fake_repositories.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

using namespace std::chrono_literals;

TEST_CASE(
    "Chronicle session summary and title transitions reject invalid edges",
    "[chronicle][session][transition]") {
  using namespace sanguinius;
  REQUIRE(transition_session(ChronicleSessionState::open,
                             SessionAction::close_with_entries) ==
          ChronicleSessionState::closing);
  REQUIRE(transition_session(ChronicleSessionState::open,
                             SessionAction::close_empty) ==
          ChronicleSessionState::abandoned);
  REQUIRE(transition_session(ChronicleSessionState::closing,
                             SessionAction::finish_summary) ==
          ChronicleSessionState::closed);
  REQUIRE_FALSE(transition_session(ChronicleSessionState::closed,
                                   SessionAction::finish_summary));
  REQUIRE(transition_summary(ChronicleSummaryState::pending,
                             SummaryAction::approve) ==
          ChronicleSummaryState::approved);
  REQUIRE_FALSE(
      transition_summary(ChronicleSummaryState::approved, SummaryAction::edit));
  REQUIRE(
      transition_title(ChronicleTitleState::proposed, TitleAction::approve) ==
      ChronicleTitleState::active);
  REQUIRE(transition_title(ChronicleTitleState::active, TitleAction::revoke) ==
          ChronicleTitleState::revoked);
  REQUIRE_FALSE(
      transition_title(ChronicleTitleState::revoked, TitleAction::feature));
}

TEST_CASE("Chronicle summary validation fences identifiers and copied context",
          "[chronicle][session][ai][privacy]") {
  using namespace sanguinius;
  const std::string entry{"00000000-0000-4000-8000-000000000101"};
  const ChronicleSummaryValidationContext context{
      .opted_in_participants = {30, 31},
      .shared_entry_ids = {entry},
      .shared_entry_context = {"Approved canon."},
      .transient_context = {std::string(80, 'x')},
  };
  ChronicleSummaryCandidate candidate{
      .chapter_title = "A chapter",
      .summary = "A short original summary.",
      .highlighted_entry_ids = {entry},
      .proposed_titles = {{.recipient_user_id = 30,
                           .title = "The Steadfast",
                           .description = "For constancy.",
                           .supporting_entry_id = entry}},
  };
  REQUIRE(validate_summary_candidate(candidate, context) ==
          SummaryValidationCode::valid);
  candidate.highlighted_entry_ids = {"00000000-0000-4000-8000-000000000999"};
  REQUIRE(validate_summary_candidate(candidate, context) ==
          SummaryValidationCode::unknown_entry);
  candidate.highlighted_entry_ids = {entry};
  candidate.proposed_titles.push_back(candidate.proposed_titles.front());
  REQUIRE(validate_summary_candidate(candidate, context) ==
          SummaryValidationCode::duplicate_recipient);
  candidate.proposed_titles.resize(1);
  candidate.summary = std::string(64, 'x');
  REQUIRE(validate_summary_candidate(candidate, context) ==
          SummaryValidationCode::copied_context);
  std::string unaligned_context;
  for (std::size_t index = 0; index < 80; ++index)
    unaligned_context += static_cast<char>('a' + index % 26);
  auto unaligned = context;
  unaligned.transient_context = {unaligned_context};
  candidate.summary = unaligned_context.substr(1, 64);
  REQUIRE(validate_summary_candidate(candidate, unaligned) ==
          SummaryValidationCode::copied_context);
}

TEST_CASE("Chronicle title responses expose full references in bounded pages",
          "[chronicle][title][interaction][bounds]") {
  using namespace sanguinius;
  test::FakeChronicleSessionRepository repository;
  const std::string grant_id{"00000000-0000-4000-8000-000000000101"};
  repository.proposed_title =
      ChronicleTitleGrant{.grant_id = grant_id,
                          .recipient_user_id = 31,
                          .title = "Keeper of the Hour",
                          .description = "For constancy.",
                          .provenance = ChronicleTitleProvenance::owner_curated,
                          .state = ChronicleTitleState::proposed,
                          .featured = false,
                          .revision = 1};
  repository.title_page.total = 6;
  for (std::size_t index = 0; index < chronicle_title_page_size; ++index) {
    auto current_id = grant_id;
    current_id.replace(current_id.size() - 3, 3, std::to_string(101 + index));
    repository.title_page.grants.push_back(
        {.grant_id = std::move(current_id),
         .recipient_user_id = 31,
         .title = std::string(maximum_chronicle_title_size, 'T'),
         .description = "Retained title.",
         .provenance = ChronicleTitleProvenance::owner_curated,
         .state = ChronicleTitleState::proposed,
         .featured = false,
         .revision = 1});
  }
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [] {},
                                  [] {},
                                  "America/New_York"};
  IncomingInteraction proposal;
  proposal.correlation_id = "title";
  proposal.interaction_id = 500;
  proposal.guild_id = 10;
  proposal.channel_id = 20;
  proposal.user_id = 30;
  proposal.command_options = {{"recipient", DiscordId{31}},
                              {"title", std::string{"Keeper of the Hour"}},
                              {"description", std::string{"For constancy."}}};
  const auto proposed = service.propose_title(proposal);
  REQUIRE(proposed.content.find(grant_id) != std::string::npos);
  REQUIRE(proposed.content.find("revision 1") != std::string::npos);

  IncomingInteraction listing;
  listing.correlation_id = "list";
  listing.interaction_id = 501;
  listing.guild_id = 10;
  listing.channel_id = 20;
  listing.user_id = 31;
  const auto listed = service.list_titles(listing);
  REQUIRE(listed.content.find(grant_id) != std::string::npos);
  REQUIRE(listed.content.find("page 1 of 2") != std::string::npos);
  REQUIRE(listed.content.size() < 2'000);
}

TEST_CASE("closing Chronicle sessions do not expose fallback review controls",
          "[chronicle][session][summary][privacy]") {
  using namespace sanguinius;
  test::FakeChronicleSessionRepository repository;
  const std::string session_id{"00000000-0000-4000-8000-000000000181"};
  const std::string draft_id{"00000000-0000-4000-8000-000000000182"};
  repository.status_result =
      ChronicleSession{.session_id = session_id,
                       .guild_id = 10,
                       .channel_id = 20,
                       .opened_by_user_id = 31,
                       .state = ChronicleSessionState::closing,
                       .opened_at_ms = 100,
                       .closing_at_ms = 200,
                       .closed_at_ms = std::nullopt,
                       .revision = 2,
                       .participants = {31},
                       .linked_shared_canon_entries = 1,
                       .draft_id = draft_id,
                       .draft_state = ChronicleSummaryState::pending,
                       .draft_revision = 1};
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [] {},
                                  [] {},
                                  "America/New_York"};
  IncomingInteraction interaction;
  interaction.correlation_id = "status";
  interaction.interaction_id = 700;
  interaction.guild_id = 10;
  interaction.channel_id = 20;
  interaction.user_id = 30;

  const auto closing = service.status(interaction);
  REQUIRE(closing.content.find("preparing summary") != std::string::npos);
  REQUIRE(closing.content.find(draft_id) == std::string::npos);
  REQUIRE(closing.content.find("edit, approve, or reject") ==
          std::string::npos);

  repository.status_result->state = ChronicleSessionState::closed;
  repository.status_result->closed_at_ms = 220;
  repository.status_result->revision = 3;
  const auto reviewable = service.status(interaction);
  REQUIRE(reviewable.content.find(draft_id) != std::string::npos);
  REQUIRE(reviewable.content.find("edit, approve, or reject") !=
          std::string::npos);
}

TEST_CASE("empty revalidated search pages retain their next control",
          "[chronicle][search][pagination][privacy]") {
  using namespace sanguinius;
  test::FakeChronicleSessionRepository repository;
  repository.search_result = {
      .cursor_id = "00000000-0000-4000-8000-000000000201",
      .page = 1,
      .total = 15,
      .items = {},
      .navigation_token_id = "00000000-0000-4000-8000-000000000202",
      .presentation = "recall"};
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [] {},
                                  [] {},
                                  "America/New_York"};
  IncomingInteraction interaction;
  interaction.correlation_id = "search";
  interaction.interaction_id = 600;
  interaction.guild_id = 10;
  interaction.channel_id = 20;
  interaction.user_id = 31;
  interaction.kind = InteractionKind::button;
  interaction.custom_id = std::string{chronicle_search_component_prefix} +
                          "00000000-0000-4000-8000-000000000200";
  const auto message = service.advance_search(interaction);
  REQUIRE(message.content == "No results on this page remain visible.");
  REQUIRE(message.buttons.size() == 1);
  REQUIRE(message.buttons.front().custom_id ==
          std::string{chronicle_search_component_prefix} +
              "00000000-0000-4000-8000-000000000202");
}

TEST_CASE("structured Chronicle summaries use an exact strict schema",
          "[chronicle][session][ai]") {
  const auto &format = sanguinius::chronicle_summary_json_schema();
  REQUIRE(format.strict);
  REQUIRE(format.name == "chronicle_session_summary");
  const auto schema = nlohmann::json::parse(format.schema);
  REQUIRE(schema.at("additionalProperties") == false);
  REQUIRE(schema.at("required").size() == 4);
  REQUIRE(schema.at("properties")
              .at("proposed_titles")
              .at("items")
              .at("additionalProperties") == false);
}

TEST_CASE("literal FTS conversion never exposes user operators",
          "[chronicle][search]") {
  REQUIRE(sanguinius::literal_fts_query("alpha OR beta*") ==
          "\"alpha\" AND \"OR\" AND \"beta*\"");
  REQUIRE(sanguinius::literal_fts_query("a\"b") == "\"a\"\"b\"");
  REQUIRE(sanguinius::literal_fts_query("   ").empty());
}

TEST_CASE("anniversary scheduling follows New York local time across DST",
          "[chronicle][anniversary][timezone]") {
  using namespace std::chrono;
  const auto *zone = locate_zone("America/New_York");
  const auto before_fall_back =
      zone->to_sys(local_days{2024y / November / 2} + 11h);
  const auto due_ms = sanguinius::next_anniversary_scan_ms(
      duration_cast<milliseconds>(before_fall_back.time_since_epoch()).count(),
      "America/New_York");
  const zoned_time due{zone, sys_time<milliseconds>{milliseconds{due_ms}}};
  REQUIRE(due.get_local_time() ==
          local_time<milliseconds>{local_days{2024y / November / 3} + 10h});
}

TEST_CASE("an unavailable AI queue completes the deterministic fallback",
          "[chronicle][session][ai][queue]") {
  using namespace sanguinius;
  test::FakeChronicleSessionRepository repository;
  repository.summary_context_result.opted_in_participants = {31};
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  bool outbox_woken = false;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [] {},
                                  [&outbox_woken] { outbox_woken = true; },
                                  "America/New_York",
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr};
  const ClaimedScheduledJob job{
      .job_id = "00000000-0000-4000-8000-000000000101",
      .job_type = std::string{session_summary_job_type},
      .lease_owner = "instance",
      .lease_token = "00000000-0000-4000-8000-000000000102",
      .attempt_count = 1,
      .max_attempts = 5,
      .due_at_ms = 100'000,
      .payload =
          SessionSummaryJobPayload{
              .session_id = "00000000-0000-4000-8000-000000000103",
              .draft_id = "00000000-0000-4000-8000-000000000104",
              .expected_session_revision = 2,
              .expected_draft_revision = 1},
      .correlation_id = "fallback-test",
      .causation_event_id = std::nullopt,
  };
  REQUIRE(service.submit_summary_job(job) == SubmitResult::accepted);
  REQUIRE(repository.last_summary_completion.has_value());
  REQUIRE(repository.last_summary_completion->generation_context ==
          repository.summary_context_result);
  REQUIRE_FALSE(repository.last_summary_completion->candidate.has_value());
  REQUIRE(repository.last_summary_completion->failure_category ==
          "queue_unavailable");
  REQUIRE(repository.last_summary_completion->relationship_event_ids.size() ==
          1);
  REQUIRE(outbox_woken);
}

TEST_CASE("changed summary context releases fallback work for immediate retry",
          "[chronicle][session][ai][privacy][retry]") {
  using namespace sanguinius;
  test::FakePendingNoticeRepository notices;
  test::FakeDurableWorkRepository durable{notices};
  durable.seed_job({.job_id = "00000000-0000-4000-8000-000000000201",
                    .job_type = std::string{session_summary_job_type},
                    .aggregate_type = "chronicle_session",
                    .aggregate_id =
                        "00000000-0000-4000-8000-000000000202",
                    .due_at_ms = 100'000,
                    .max_attempts = 5,
                    .idempotency_key = "job:changed-summary-context",
                    .created_at_ms = 100'000},
                   SessionSummaryJobPayload{
                       .session_id =
                           "00000000-0000-4000-8000-000000000202",
                       .draft_id = "00000000-0000-4000-8000-000000000203",
                       .expected_session_revision = 2,
                       .expected_draft_revision = 1});
  const auto claimed = durable.claim_due_job(
      100'000, 220'000, "instance", "00000000-0000-4000-8000-000000000204");
  REQUIRE(claimed);

  test::FakeChronicleSessionRepository repository;
  repository.summary_context_result.opted_in_participants = {31};
  repository.summary_completion_result = WorkMutationStatus::invalid_state;
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  bool scheduler_woken = false;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [&scheduler_woken] { scheduler_woken = true; },
                                  [] {},
                                  "America/New_York",
                                  nullptr,
                                  nullptr,
                                  &durable,
                                  nullptr};
  REQUIRE(service.submit_summary_job(*claimed) == SubmitResult::accepted);
  REQUIRE(scheduler_woken);
  const auto reclaimed = durable.claim_due_job(
      100'000, 220'000, "competitor",
      "00000000-0000-4000-8000-000000000205");
  REQUIRE(reclaimed);
  REQUIRE(reclaimed->attempt_count == 1);
}

TEST_CASE("queued summary work extends and releases its durable claim",
          "[chronicle][session][ai][lease][shutdown]") {
  using namespace sanguinius;
  test::FakePendingNoticeRepository notices;
  test::FakeDurableWorkRepository durable{notices};
  durable.seed_job({.job_id = "00000000-0000-4000-8000-000000000301",
                    .job_type = std::string{session_summary_job_type},
                    .aggregate_type = "chronicle_session",
                    .aggregate_id = "00000000-0000-4000-8000-000000000302",
                    .due_at_ms = 100'000,
                    .max_attempts = 5,
                    .idempotency_key = "job:queued-summary",
                    .created_at_ms = 100'000},
                   SessionSummaryJobPayload{
                       .session_id = "00000000-0000-4000-8000-000000000302",
                       .draft_id = "00000000-0000-4000-8000-000000000303",
                       .expected_session_revision = 2,
                       .expected_draft_revision = 1});
  const auto claimed = durable.claim_due_job(
      100'000, 220'000, "instance", "00000000-0000-4000-8000-000000000304");
  REQUIRE(claimed);

  AiWorkService work{2, 1};
  work.start();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  REQUIRE(work.submit([&](const std::stop_token stop_token) {
    entered.set_value();
    std::unique_lock lock{gate_mutex};
    std::stop_callback wake_on_stop{stop_token,
                                    [&] { gate_changed.notify_all(); }};
    gate_changed.wait(lock,
                      [stop_token] { return stop_token.stop_requested(); });
  }) == SubmitResult::accepted);
  REQUIRE(entered_future.wait_for(2s) == std::future_status::ready);

  test::FakeChronicleSessionRepository repository;
  test::FakeClock clock{std::chrono::sys_seconds{100s}};
  test::FakePersistentIdGenerator ids;
  test::FakeAiClient ai;
  test::FakeDiagnostics diagnostics;
  ChronicleSessionService service{repository,
                                  clock,
                                  ids,
                                  ServerScopeConfiguration{10, 20, 30},
                                  ControlConfiguration{},
                                  [] {},
                                  [] {},
                                  "America/New_York",
                                  &ai,
                                  &work,
                                  &durable,
                                  &diagnostics};
  REQUIRE(service.submit_summary_job(*claimed) == SubmitResult::accepted);

  clock.set(std::chrono::sys_seconds{250s});
  REQUIRE_FALSE(durable.claim_due_job(250'000, 300'000, "competitor",
                                      "00000000-0000-4000-8000-000000000305"));
  work.stop();
  const auto reclaimed = durable.claim_due_job(
      250'000, 370'000, "competitor", "00000000-0000-4000-8000-000000000306");
  REQUIRE(reclaimed);
  REQUIRE(reclaimed->attempt_count == 1);
}
