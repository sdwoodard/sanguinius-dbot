#include "sanguinius/chronicle.hpp"

#include "support/fake_chronicle_repository.hpp"
#include "support/fake_clock.hpp"
#include "support/fake_discord.hpp"
#include "support/fake_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

TEST_CASE("Chronicle text tags and opaque controls are bounded",
          "[chronicle][unit][validation]") {
  REQUIRE(sanguinius::valid_chronicle_text("A valid memory\nwith a line.", 64));
  REQUIRE(sanguinius::valid_chronicle_text("Glory \xE2\x9C\xA8", 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text("bad\x01control", 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text("bad\x7F"
                                                 "control",
                                                 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text("bad\xC2\x85"
                                                 "control",
                                                 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text("\xC0\x80", 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text(" \t\r\n", 64));
  REQUIRE_FALSE(sanguinius::valid_chronicle_text("12345", 4));
  REQUIRE(sanguinius::valid_chronicle_snapshot_text(" \t\n", 64));
  REQUIRE_FALSE(
      sanguinius::valid_chronicle_snapshot_text("bad\x01control", 64));

  REQUIRE(sanguinius::parse_chronicle_tags("").empty());
  REQUIRE(sanguinius::parse_chronicle_tags("   ").empty());
  REQUIRE(sanguinius::parse_chronicle_tags("Deed, blood-angels") ==
          std::vector<std::string>{"deed", "blood-angels"});
  REQUIRE_THROWS(sanguinius::parse_chronicle_tags("same,same"));
  REQUIRE_THROWS(sanguinius::parse_chronicle_tags("not valid"));

  const std::string token{"00000000-0000-4000-8000-000000000001"};
  const auto component = sanguinius::make_chronicle_component(
      sanguinius::chronicle_component_prefix, token);
  REQUIRE(sanguinius::parse_chronicle_component(
              component, sanguinius::chronicle_component_prefix) == token);
  REQUIRE_FALSE(sanguinius::parse_chronicle_component(
      component, sanguinius::chronicle_modal_prefix));
}

TEST_CASE("Chronicle source-derived bodies preserve UTF-8 boundaries",
          "[chronicle][unit][validation][utf8]") {
  sanguinius::test::FakeChronicleRepository repository;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{1}}};
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::ChronicleService service{repository, clock, ids,  {10, 20, 30},
                                       {},         [] {}, [] {}};
  auto responder =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto interaction = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::message_context_command, 60, 10,
      20, 31);
  interaction.context_message = sanguinius::ContextMessageSnapshot{
      .reference = {.message_id = 61, .guild_id = 10, .channel_id = 20},
      .author = {.user_id = 32, .username = "source", .display_name = "Source"},
      .content = std::string(999, 'a') + "\xE2\x9C\xA8",
      .occurred_at_ms = 500};

  const auto result = service.canonize_message(interaction);
  REQUIRE(result.entry.has_value());
  REQUIRE(result.entry->body.size() == 999);
  REQUIRE(sanguinius::valid_chronicle_text(
      result.entry->body, sanguinius::maximum_chronicle_body_size));
}

TEST_CASE("Chronicle accepts only verifier-backed delivered bot appearances",
          "[chronicle][unit][appearance][privacy]") {
  sanguinius::test::FakeChronicleRepository repository;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{1}}};
  sanguinius::test::FakePersistentIdGenerator ids;
  auto responder =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto interaction = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::message_context_command, 60, 10,
      20, 31);
  interaction.context_message = sanguinius::ContextMessageSnapshot{
      .reference = {.message_id = 61, .guild_id = 10, .channel_id = 20},
      .author = {.user_id = 42,
                 .username = "sanguinius",
                 .display_name = "Sanguinius",
                 .is_bot = true},
      .content = "The exact delivered public appearance.",
      .occurred_at_ms = 500};

  sanguinius::ChronicleService unverified{repository, clock, ids,  {10, 20, 30},
                                          {},         [] {}, [] {}};
  REQUIRE(unverified.canonize_message(interaction).code ==
          sanguinius::ChronicleResultCode::unauthorized);
  REQUIRE(repository.proposal_count() == 0);

  const std::string decision_id{"00000000-0000-4000-8000-000000009901"};
  sanguinius::ChronicleService verified{
      repository,
      clock,
      ids,
      {10, 20, 30},
      {},
      [] {},
      [] {},
      64,
      [] {},
      [&](const sanguinius::ContextMessageSnapshot &message) {
        REQUIRE(message.content == interaction.context_message->content);
        return std::optional<std::pair<std::string, bool>>{
            std::pair{decision_id, false}};
      }};
  const auto result = verified.canonize_message(interaction);
  REQUIRE(result.code == sanguinius::ChronicleResultCode::created);
  REQUIRE(result.entry);
  REQUIRE(result.entry->body == interaction.context_message->content);
  REQUIRE(result.entry->participants.empty());
  const auto request = repository.latest_proposal();
  REQUIRE(request);
  REQUIRE(request->appearance_decision_id == decision_id);
  REQUIRE_FALSE(request->owner_test);
}

TEST_CASE(
    "Chronicle proposal rendering exposes bounded provenance and test data",
    "[chronicle][unit][privacy][rendering]") {
  sanguinius::ChronicleEntry entry;
  entry.entry_id = "00000000-0000-4000-8000-000000000001";
  entry.title = "A test entry";
  entry.body = "Only private proposal prose";
  entry.source_author_user_id = 32;
  entry.source_guild_id = 10;
  entry.source_channel_id = 20;
  entry.source_message_id = 61;
  entry.source_text = "The captured source statement";
  entry.source_text_truncated = true;
  entry.tags = {"owner-test"};
  entry.attachments = {{.attachment_id = 71,
                        .filename = "relic.png",
                        .content_type = "image/png",
                        .byte_size = 4'096,
                        .width = 640,
                        .height = 480,
                        .ephemeral = true,
                        .spoiler = true}};
  const sanguinius::ProposalResult proposal{
      .code = sanguinius::ChronicleResultCode::created,
      .entry = entry,
      .actions = sanguinius::ProposalActionIds{
          .edit_token_id = "00000000-0000-4000-8000-000000000002",
          .submit_token_id = "00000000-0000-4000-8000-000000000003",
          .retract_token_id = "00000000-0000-4000-8000-000000000004"}};

  const auto rendered = sanguinius::render_chronicle_proposal(proposal);
  REQUIRE(rendered.content.find("TEST DATA") != std::string::npos);
  REQUIRE(rendered.embed.has_value());
  REQUIRE(rendered.buttons.size() == 3);
  REQUIRE(rendered.buttons[0].style == sanguinius::ButtonStyle::secondary);
  REQUIRE(rendered.buttons[1].style == sanguinius::ButtonStyle::primary);
  REQUIRE(rendered.buttons[2].style == sanguinius::ButtonStyle::danger);
  const auto &provenance = rendered.embed->description;
  REQUIRE(provenance.find("author: `32`") != std::string::npos);
  REQUIRE(provenance.find("message `61`") != std::string::npos);
  REQUIRE(provenance.find("relic.png") != std::string::npos);
  REQUIRE(provenance.find("image/png") != std::string::npos);
  REQUIRE(provenance.find("4096 bytes") != std::string::npos);
  REQUIRE(provenance.find("640x480") != std::string::npos);
  REQUIRE(provenance.find("ephemeral") != std::string::npos);
  REQUIRE(provenance.find("spoiler") != std::string::npos);
  REQUIRE(provenance.find("captured source statement") != std::string::npos);
  REQUIRE(provenance.find("snapshot truncated") != std::string::npos);
  REQUIRE(sanguinius::render_chronicle_provenance(entry, 64).size() <= 64);

  const auto awaiting = sanguinius::render_chronicle_proposal(
      {.code = sanguinius::ChronicleResultCode::existing,
       .entry = entry,
       .control_mode =
           sanguinius::ProposalControlMode::awaiting_confirmations});
  REQUIRE(awaiting.buttons.empty());
  REQUIRE(awaiting.content.find("still pending") != std::string::npos);
  const auto reissued = sanguinius::render_chronicle_proposal(
      {.code = sanguinius::ChronicleResultCode::existing,
       .entry = entry,
       .control_mode =
           sanguinius::ProposalControlMode::confirmations_reissued});
  REQUIRE(reissued.buttons.empty());
  REQUIRE(reissued.content.find("were reissued") != std::string::npos);
}

TEST_CASE("volatile Chronicle actions are scoped replayable and group bounded",
          "[chronicle][unit][privacy]") {
  sanguinius::VolatileChronicleActions actions{2};
  actions.put("00000000-0000-4000-8000-000000000001",
              {.kind = sanguinius::VolatileActionKind::cancel_memory,
               .guild_id = 10,
               .channel_id = 20,
               .expected_user_id = 30,
               .expires_at_ms = 1'000,
               .group_id = "group"});
  auto responder =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto wrong = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::button, 40, 10, 20, 31);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000001", wrong, 100)
              .status == sanguinius::VolatileClaimStatus::unavailable);
  auto correct = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::button, 41, 10, 20, 30);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000001", correct, 100)
              .status == sanguinius::VolatileClaimStatus::claimed);
  sanguinius::ChronicleEntry private_entry;
  private_entry.entry_id = "00000000-0000-4000-8000-000000000010";
  private_entry.title = "Private title";
  private_entry.body = "Private proposal body";
  private_entry.source_text = "Private source snapshot";
  private_entry.status = sanguinius::ChronicleEntryStatus::retracted;
  private_entry.revision = 4;
  private_entry.participants = {30, 31};
  private_entry.tags = {"private"};
  private_entry.attachments = {sanguinius::ChronicleAttachment{
      .attachment_id = 70, .filename = "private.png"}};
  actions.finish("00000000-0000-4000-8000-000000000001",
                 {.code = sanguinius::ChronicleResultCode::updated,
                  .entry = std::move(private_entry)});
  const auto replay =
      actions.claim("00000000-0000-4000-8000-000000000001", correct, 100);
  REQUIRE(replay.status == sanguinius::VolatileClaimStatus::completed);
  REQUIRE(replay.result->code == sanguinius::ChronicleResultCode::updated);
  REQUIRE(replay.result->entry.has_value());
  REQUIRE(replay.result->entry->status ==
          sanguinius::ChronicleEntryStatus::retracted);
  REQUIRE(replay.result->entry->revision == 4);
  REQUIRE(replay.result->entry->title.empty());
  REQUIRE(replay.result->entry->body.empty());
  REQUIRE(replay.result->entry->source_text.empty());
  REQUIRE(replay.result->entry->participants.empty());
  REQUIRE(replay.result->entry->tags.empty());
  REQUIRE(replay.result->entry->attachments.empty());
  actions.cancel_group("group");

  actions.put("00000000-0000-4000-8000-000000000002",
              {.kind = sanguinius::VolatileActionKind::cancel_memory,
               .guild_id = 10,
               .channel_id = 20,
               .expected_user_id = 30,
               .expires_at_ms = 200,
               .group_id = "old"});
  actions.put_group({
      {"00000000-0000-4000-8000-000000000003",
       {.kind = sanguinius::VolatileActionKind::cancel_memory,
        .guild_id = 10,
        .channel_id = 20,
        .expected_user_id = 30,
        .expires_at_ms = 300,
        .group_id = "new"}},
      {"00000000-0000-4000-8000-000000000004",
       {.kind = sanguinius::VolatileActionKind::cancel_memory,
        .guild_id = 10,
        .channel_id = 20,
        .expected_user_id = 30,
        .expires_at_ms = 400,
        .group_id = "new"}},
  });
  REQUIRE(actions.size() == 2);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000002", correct, 100)
              .status == sanguinius::VolatileClaimStatus::unavailable);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000003", correct, 100)
              .status == sanguinius::VolatileClaimStatus::claimed);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000004", correct, 100)
              .status == sanguinius::VolatileClaimStatus::busy);
  REQUIRE_THROWS_AS(
      actions.put("00000000-0000-4000-8000-000000000005",
                  {.kind = sanguinius::VolatileActionKind::cancel_memory,
                   .guild_id = 10,
                   .channel_id = 20,
                   .expected_user_id = 30,
                   .expires_at_ms = 500,
                   .group_id = "outsider"}),
      std::overflow_error);
  actions.release("00000000-0000-4000-8000-000000000003");
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000004", correct, 100)
              .status == sanguinius::VolatileClaimStatus::claimed);
  actions.release("00000000-0000-4000-8000-000000000004");
  actions.cancel_group("new");
  REQUIRE(actions.size() == 0);
}

TEST_CASE("volatile Chronicle actions expire during deterministic cleanup",
          "[chronicle][unit][privacy][expiry]") {
  sanguinius::VolatileChronicleActions actions{2};
  actions.put("00000000-0000-4000-8000-000000000001",
              {.kind = sanguinius::VolatileActionKind::confirm_memory,
               .guild_id = 10,
               .channel_id = 20,
               .expected_user_id = 30,
               .expires_at_ms = 10,
               .memory =
                   sanguinius::MemoryDraft{
                       .text = "Sensitive draft that must not linger",
                       .visibility = sanguinius::MemoryVisibility::self_only,
                       .sensitivity = sanguinius::MemorySensitivity::sensitive,
                       .guild_id = 10,
                       .channel_id = 20,
                       .user_id = 30},
               .group_id = "abandoned-private-draft"});
  REQUIRE(actions.size() == 1);

  actions.purge_expired(10);
  REQUIRE(actions.size() == 0);
}

TEST_CASE("volatile Chronicle actions serialize concurrent token consumption",
          "[chronicle][unit][privacy][concurrency]") {
  constexpr std::size_t contender_count = 8;
  const std::string token{"00000000-0000-4000-8000-000000000010"};
  sanguinius::VolatileChronicleActions actions{16};
  actions.put(token, {.kind = sanguinius::VolatileActionKind::confirm_memory,
                      .guild_id = 10,
                      .channel_id = 20,
                      .expected_user_id = 30,
                      .expires_at_ms = 1'000,
                      .group_id = "concurrent"});

  std::barrier gate{static_cast<std::ptrdiff_t>(contender_count + 1)};
  std::atomic<std::size_t> claimed{};
  std::atomic<std::size_t> busy{};
  std::vector<std::thread> contenders;
  contenders.reserve(contender_count);
  for (std::size_t index = 0; index < contender_count; ++index) {
    contenders.emplace_back([&, index] {
      auto responder =
          std::make_shared<sanguinius::test::FakeInteractionResponder>();
      auto interaction = sanguinius::test::interaction(
          responder, sanguinius::InteractionKind::button,
          static_cast<std::uint64_t>(100 + index), 10, 20, 30);
      gate.arrive_and_wait();
      const auto result = actions.claim(token, interaction, 100);
      if (result.status == sanguinius::VolatileClaimStatus::claimed)
        claimed.fetch_add(1, std::memory_order_relaxed);
      if (result.status == sanguinius::VolatileClaimStatus::busy)
        busy.fetch_add(1, std::memory_order_relaxed);
    });
  }
  gate.arrive_and_wait();
  for (auto &contender : contenders)
    contender.join();

  REQUIRE(claimed.load(std::memory_order_relaxed) == 1);
  REQUIRE(busy.load(std::memory_order_relaxed) == contender_count - 1);
  REQUIRE(actions.size() == 1);
  auto unknown = sanguinius::test::interaction(
      std::make_shared<sanguinius::test::FakeInteractionResponder>(),
      sanguinius::InteractionKind::button, 199, 10, 20, 30);
  REQUIRE(actions.claim("00000000-0000-4000-8000-000000000099", unknown, 1'000)
              .status == sanguinius::VolatileClaimStatus::unavailable);
  REQUIRE(actions.size() == 1);
  actions.finish(token, {.code = sanguinius::ChronicleResultCode::created});
  auto duplicate = sanguinius::test::interaction(
      std::make_shared<sanguinius::test::FakeInteractionResponder>(),
      sanguinius::InteractionKind::button, 200, 10, 20, 30);
  const auto replay = actions.claim(token, duplicate, 100);
  REQUIRE(replay.status == sanguinius::VolatileClaimStatus::completed);
  REQUIRE(replay.result->code == sanguinius::ChronicleResultCode::created);
}

TEST_CASE("memory preview coerces sensitivity and is lost across restart",
          "[chronicle][unit][memory]") {
  sanguinius::test::FakeChronicleRepository repository;
  sanguinius::test::FakeClock clock{
      std::chrono::sys_seconds{std::chrono::seconds{1}}};
  sanguinius::test::FakePersistentIdGenerator ids;
  sanguinius::ChronicleService service{repository, clock, ids,  {10, 20, 30},
                                       {},         [] {}, [] {}};
  auto responder =
      std::make_shared<sanguinius::test::FakeInteractionResponder>();
  auto modal = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::modal_submit, 50, 10, 20, 31);
  modal.custom_id = "chronicle.remember:1";
  modal.modal_fields = {{"text", "A personal detail"},
                        {"visibility", "shared"},
                        {"sensitivity", "personal"},
                        {"expiry", "30d"}};
  const auto preview = service.begin_memory_preview(modal);
  REQUIRE(preview.content.find("self_only") != std::string::npos);
  REQUIRE(preview.buttons.size() == 2);

  auto confirm = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::button, 51, 10, 20, 31);
  confirm.custom_id = preview.buttons[0].custom_id;
  sanguinius::ChronicleService restarted{repository, clock, ids,  {10, 20, 30},
                                         {},         [] {}, [] {}};
  REQUIRE(restarted.apply_component(confirm).code ==
          sanguinius::ChronicleResultCode::invalid_token);
  REQUIRE(repository.confirmation_count() == 0);
  REQUIRE(service.apply_component(confirm).code ==
          sanguinius::ChronicleResultCode::created);
  REQUIRE(repository.confirmation_count() == 1);
  REQUIRE(service.apply_component(confirm).code ==
          sanguinius::ChronicleResultCode::created);
  REQUIRE(repository.confirmation_count() == 1);

  modal.interaction_id = 52;
  modal.modal_fields[0].second = "A retryable personal detail";
  const auto retry_preview = service.begin_memory_preview(modal);
  auto retry = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::button, 53, 10, 20, 31);
  retry.custom_id = retry_preview.buttons[0].custom_id;
  repository.fail_next_confirmation();
  REQUIRE_THROWS_AS(service.apply_component(retry), std::runtime_error);
  REQUIRE(service.apply_component(retry).code ==
          sanguinius::ChronicleResultCode::created);
  REQUIRE(repository.confirmation_count() == 2);

  modal.interaction_id = 54;
  modal.modal_fields[0].second = "A draft that must be discarded";
  const auto cancel_preview = service.begin_memory_preview(modal);
  auto cancel = sanguinius::test::interaction(
      responder, sanguinius::InteractionKind::button, 55, 10, 20, 31);
  cancel.custom_id = cancel_preview.buttons[1].custom_id;
  const auto cancelled = service.apply_component(cancel);
  REQUIRE(cancelled.draft_cancelled);
  REQUIRE(sanguinius::render_chronicle_mutation(cancelled).content.find(
              "discarded") != std::string::npos);
  REQUIRE(repository.confirmation_count() == 2);
  confirm.interaction_id = 56;
  confirm.custom_id = cancel_preview.buttons[0].custom_id;
  REQUIRE(service.apply_component(confirm).code ==
          sanguinius::ChronicleResultCode::invalid_token);
}

TEST_CASE("Chronicle modals use fixed bounded fields",
          "[chronicle][unit][modal]") {
  const auto remember = sanguinius::ChronicleService::remember_modal();
  REQUIRE(remember.fields.size() == 5);
  REQUIRE(remember.fields[0].style ==
          sanguinius::ModalFieldPayload::Style::paragraph);
  REQUIRE(remember.fields[0].maximum_length ==
          sanguinius::maximum_memory_text_size);
  REQUIRE(remember.fields[1].custom_id == "tags");
  REQUIRE_FALSE(remember.fields[1].required);
  const auto edit = sanguinius::ChronicleService::edit_entry_modal(
      "00000000-0000-4000-8000-000000000001");
  REQUIRE(edit.fields.size() == 5);
  REQUIRE(edit.fields[1].style ==
          sanguinius::ModalFieldPayload::Style::paragraph);
}

TEST_CASE("Chronicle state machines make terminal transitions explicit",
          "[chronicle][unit][state-machine]") {
  using sanguinius::ChronicleEntryAction;
  using sanguinius::ChronicleEntryStatus;
  REQUIRE(sanguinius::transition_chronicle_entry(
              ChronicleEntryStatus::proposed, false, ChronicleEntryAction::edit)
              ->status == ChronicleEntryStatus::proposed);
  REQUIRE(sanguinius::transition_chronicle_entry(ChronicleEntryStatus::proposed,
                                                 false,
                                                 ChronicleEntryAction::submit)
              ->submitted);
  REQUIRE_FALSE(sanguinius::transition_chronicle_entry(
      ChronicleEntryStatus::proposed, true, ChronicleEntryAction::edit));
  REQUIRE(sanguinius::transition_chronicle_entry(
              ChronicleEntryStatus::proposed, true,
              ChronicleEntryAction::approval_completed, true)
              ->status == ChronicleEntryStatus::proposed);
  REQUIRE(sanguinius::transition_chronicle_entry(
              ChronicleEntryStatus::proposed, true,
              ChronicleEntryAction::approval_completed, false)
              ->status == ChronicleEntryStatus::canon);
  REQUIRE(sanguinius::transition_chronicle_entry(ChronicleEntryStatus::proposed,
                                                 true,
                                                 ChronicleEntryAction::decline)
              ->status == ChronicleEntryStatus::retracted);
  REQUIRE(sanguinius::transition_chronicle_entry(
              ChronicleEntryStatus::canon, true, ChronicleEntryAction::retract)
              ->status == ChronicleEntryStatus::retracted);
  REQUIRE_FALSE(sanguinius::transition_chronicle_entry(
      ChronicleEntryStatus::canon, true, ChronicleEntryAction::edit));
  REQUIRE_FALSE(sanguinius::transition_chronicle_entry(
      ChronicleEntryStatus::retracted, true, ChronicleEntryAction::retract));

  REQUIRE(sanguinius::transition_memory(sanguinius::MemoryStatus::confirmed,
                                        sanguinius::MemoryAction::retract) ==
          sanguinius::MemoryStatus::retracted);
  REQUIRE(sanguinius::transition_memory(sanguinius::MemoryStatus::confirmed,
                                        sanguinius::MemoryAction::expire) ==
          sanguinius::MemoryStatus::expired);
  REQUIRE_FALSE(sanguinius::transition_memory(
      sanguinius::MemoryStatus::retracted, sanguinius::MemoryAction::expire));
  REQUIRE_FALSE(sanguinius::transition_memory(
      sanguinius::MemoryStatus::expired, sanguinius::MemoryAction::retract));
}
