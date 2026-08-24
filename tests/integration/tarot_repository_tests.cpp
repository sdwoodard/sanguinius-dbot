#include "sanguinius/durable_work.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include "support/fake_clock.hpp"
#include "support/temp_database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using sanguinius::TarotMutationStatus;
using sanguinius::TarotRecoveryKind;
using sanguinius::TarotRecoveryStatus;
using sanguinius::TarotTrialDraw;
using sanguinius::TarotVisibility;
using sanguinius::persistence::Database;
using sanguinius::persistence::Migrator;
using sanguinius::persistence::SqliteCoreIdentityRepository;
using sanguinius::persistence::SqliteRepositoryContext;
using sanguinius::persistence::SqliteTarotRepository;

[[nodiscard]] std::string uuid(const std::size_t value) {
  auto suffix = std::to_string(value);
  suffix.insert(suffix.begin(), 12 - suffix.size(), '0');
  return "00000000-0000-4000-8000-" + suffix;
}

[[nodiscard]] std::int64_t scalar(SqliteRepositoryContext &context,
                                  const std::string_view sql) {
  auto query = context.connection().prepare(sql);
  REQUIRE(query.step());
  return query.column_int64(0);
}

void prepare_unlinked_grace(SqliteRepositoryContext &context) {
  context.connection().execute_script(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000005901',"
      "'tarot.grace_completed.v1','tarot_recovery_claim',"
      "'00000000-0000-4000-8000-000000005900','30','10','20',300,300,"
      "'test','tarot:unlinked-grace-event','{}');"
      "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
      "expected_posting_count,event_id,idempotency_key,actor_user_id,is_test,"
      "created_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000005902','GRACE','prepared',2,"
      "'00000000-0000-4000-8000-000000005901',"
      "'tarot:unlinked-grace-transaction','30',1,300);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000005903',"
      "'00000000-0000-4000-8000-000000005902',"
      "'00000000-0000-4000-8000-000000000001',-1,300);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000005904',"
      "'00000000-0000-4000-8000-000000005902',"
      "'00000000-0000-4000-8000-000000000010',1,300);");
}

class TarotFixture {
public:
  TarotFixture() {
    {
      auto database = Database::open_migration(temporary.path());
      const Migrator migrator{sanguinius::persistence::production_migrations(),
                              {"test", "revision"},
                              clock};
      REQUIRE(migrator.apply(database.connection()).current_version == 10);
    }
    context = std::make_shared<SqliteRepositoryContext>(
        Database::open_runtime(temporary.path()));
    SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 100);
    identities.ensure_user({9, "Earlier", "earlier", false, 100});
    identities.ensure_user({31, "Member", "member", false, 100});
    repository = std::make_unique<SqliteTarotRepository>(context);
    repository->initialize_system_accounts({uuid(1), uuid(2), uuid(3), uuid(4)},
                                           100);
  }

  [[nodiscard]] sanguinius::TarotInvocation
  call(const std::string &key, const std::int64_t now_ms,
       const sanguinius::DiscordSnowflake user_id = 30) const {
    return {.user_id = user_id,
            .guild_id = 10,
            .channel_id = 20,
            .display_name = user_id == 30 ? "Owner" : "Member",
            .interaction_idempotency_key = key,
            .correlation_id = "tarot-test",
            .now_ms = now_ms};
  }

  [[nodiscard]] sanguinius::TarotAccountProvisionResult
  provision(const std::size_t base,
            const sanguinius::DiscordSnowflake user_id = 30) {
    return repository->ensure_account(
        {.invocation =
             call("provision:" + std::to_string(user_id.value()), 200, user_id),
         .starting_fate = 100,
         .account_id = uuid(base),
         .transaction_id = uuid(base + 1),
         .event_id = uuid(base + 2),
         .mint_posting_id = uuid(base + 3),
         .human_posting_id = uuid(base + 4)});
  }

  sanguinius::test::TemporaryDatabase temporary;
  sanguinius::test::FakeClock clock;
  std::shared_ptr<SqliteRepositoryContext> context;
  std::unique_ptr<SqliteTarotRepository> repository;
};

} // namespace

TEST_CASE("Tarot starting grants are balanced immutable and idempotent",
          "[tarot][ledger][idempotency]") {
  TarotFixture fixture;
  const auto created = fixture.provision(10);
  REQUIRE(created.created);
  REQUIRE(created.balance == 100);
  const auto replay = fixture.provision(20);
  REQUIRE_FALSE(replay.created);
  REQUIRE(replay.account_id == created.account_id);
  REQUIRE(replay.balance == 100);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='STARTING_GRANT'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT CAST(total(amount) AS INTEGER) FROM tarot_posting") ==
          0);

  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_posting SET amount=101 WHERE amount=100"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_transaction WHERE transaction_type='STARTING_GRANT'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_account SET created_at_ms=201 WHERE account_kind='HUMAN'"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_account WHERE account_kind='HUMAN'"));

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000004990',"
        "'tarot.starting_grant_created.v1','tarot_account',"
        "'00000000-0000-4000-8000-000000000010','30','10','20',300,300,"
        "'test','tarot:test-starting-grant','{}')");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,is_test,"
        "created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000004991','STARTING_GRANT','prepared',"
        "2,'00000000-0000-4000-8000-000000004990',"
        "'tarot:test-starting-grant:transaction','30',1,300)"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000005000','tarot.admin_adjusted.v1',"
        "'tarot_transaction','direct-committed','30','10','20',300,300,"
        "'test','tarot:direct-committed','{}')");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms,committed_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000005001','TEST_ADJUSTMENT','committed',"
        "2,'00000000-0000-4000-8000-000000005000','tarot:direct-tx','30',"
        "'must be rejected',1,300,300)"));
  }
  REQUIRE_THROWS(fixture.context->connection().execute(
      "INSERT INTO tarot_recovery_claim("
      "claim_id,account_id,claim_type,state,visibility,is_test,"
      "eligibility_threshold,grace_target,eligibility_balance,reward,"
      "transaction_id,"
      "started_event_id,event_id,start_idempotency_key,"
      "completion_idempotency_key,created_at_ms,expires_at_ms,"
      "completed_at_ms,cooldown_until_ms) VALUES("
      "'00000000-0000-4000-8000-000000000098',"
      "'00000000-0000-4000-8000-000000000010','GRACE','completed',"
      "'private',0,10,25,0,100,"
      "'00000000-0000-4000-8000-000000000011',"
      "'00000000-0000-4000-8000-000000000012',"
      "'00000000-0000-4000-8000-000000000012','terminal:insert',"
      "'terminal:complete',200,1100,300,1000)"));

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000000090','tarot.admin_adjusted.v1',"
        "'tarot_transaction','bad','30','10','20',300,300,'test',"
        "'tarot:bad','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000091','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000000090','tarot:bad','30','bad',1,300)"
        ";"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000092',"
        "'00000000-0000-4000-8000-000000000091',"
        "'00000000-0000-4000-8000-000000000001',-10,300);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000093',"
        "'00000000-0000-4000-8000-000000000091',"
        "'00000000-0000-4000-8000-000000000010',9,300);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=300 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000000091'"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000000094','tarot.admin_adjusted.v1',"
        "'tarot_transaction','negative','30','10','20',301,301,'test',"
        "'tarot:negative','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000000095','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000000094','tarot:negative','30',"
        "'negative',1,301);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000096',"
        "'00000000-0000-4000-8000-000000000095',"
        "'00000000-0000-4000-8000-000000000004',0,301)"));
    REQUIRE_THROWS(fixture.context->connection().execute(
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000096',"
        "'00000000-0000-4000-8000-000000000095',"
        "'00000000-0000-4000-8000-000000000004',1000000001,301)"));
    fixture.context->connection().execute_script(
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000096',"
        "'00000000-0000-4000-8000-000000000095',"
        "'00000000-0000-4000-8000-000000000004',101,301);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000000097',"
        "'00000000-0000-4000-8000-000000000095',"
        "'00000000-0000-4000-8000-000000000010',-101,301);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=301 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000000095'"));
  }
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("Tarot recovery transfers require their exact pending claim",
          "[tarot][recovery][invariant][sql]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  prepare_unlinked_grace(*fixture.context);

  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=300 "
      "WHERE transaction_id='00000000-0000-4000-8000-000000005902'"));
}

TEST_CASE("Tarot invariant checks reject an unclaimed recovery transfer",
          "[tarot][recovery][invariant][corruption]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  fixture.context->connection().execute(
      "DROP TRIGGER tarot_recovery_transaction_seal_link");
  prepare_unlinked_grace(*fixture.context);
  fixture.context->connection().execute(
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=300 "
      "WHERE transaction_id='00000000-0000-4000-8000-000000005902'");

  const auto report = fixture.repository->check_invariants();
  REQUIRE_FALSE(report.valid);
  REQUIRE(report.claim_mismatch_count == 1);
}

TEST_CASE("Tarot invariant checks reject human accounts without grants",
          "[tarot][account][invariant][corruption]") {
  TarotFixture fixture;
  fixture.context->connection().execute(
      "INSERT INTO "
      "tarot_account(account_id,account_kind,user_id,created_at_ms) "
      "VALUES('00000000-0000-4000-8000-000000006000','HUMAN','31',300)");

  const auto report = fixture.repository->check_invariants();
  REQUIRE_FALSE(report.valid);
  REQUIRE(report.orphaned_link_count == 1);
}

TEST_CASE("Tarot test transaction seals require complete audit provenance",
          "[tarot][ledger][audit][sql]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.provision(20, 31));

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000006101',"
        "'tarot.admin_adjusted.v1','tarot_transaction',"
        "'00000000-0000-4000-8000-000000006102','30','10','20',300,300,"
        "'test','tarot:missing-adjustment-audit','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000006102','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000006101',"
        "'tarot:missing-adjustment-audit:transaction',NULL,NULL,1,300);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006103',"
        "'00000000-0000-4000-8000-000000006102',"
        "'00000000-0000-4000-8000-000000000001',-5,300);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006104',"
        "'00000000-0000-4000-8000-000000006102',"
        "'00000000-0000-4000-8000-000000000010',5,300);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=300 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000006102'"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000006111',"
        "'tarot.trial_abandoned.v1','tarot_recovery_claim','unrelated-claim',"
        "'30','10','20',301,301,'test','tarot:wrong-adjustment-event','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000006112','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000006111',"
        "'tarot:wrong-adjustment-event:transaction','30','audited "
        "reason',1,301);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006113',"
        "'00000000-0000-4000-8000-000000006112',"
        "'00000000-0000-4000-8000-000000000001',-5,301);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006114',"
        "'00000000-0000-4000-8000-000000006112',"
        "'00000000-0000-4000-8000-000000000010',5,301);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=301 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000006112'"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000006121',"
        "'tarot.transaction_reversed.v1','tarot_transaction',"
        "'00000000-0000-4000-8000-000000006122','30','10','20',302,302,"
        "'test','tarot:wrong-adjustment-event-type','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000006122','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000006121',"
        "'tarot:wrong-adjustment-event-type:transaction','30','audited "
        "reason',1,302);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006123',"
        "'00000000-0000-4000-8000-000000006122',"
        "'00000000-0000-4000-8000-000000000001',-5,302);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006124',"
        "'00000000-0000-4000-8000-000000006122',"
        "'00000000-0000-4000-8000-000000000010',5,302);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=302 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000006122'"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000006131',"
        "'tarot.admin_adjusted.v1','tarot_transaction',"
        "'00000000-0000-4000-8000-000000006132','30','10','20',303,303,"
        "'test','tarot:wrong-adjustment-account','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000006132','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000006131',"
        "'tarot:wrong-adjustment-account:transaction','30','audited reason',1,"
        "303);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006133',"
        "'00000000-0000-4000-8000-000000006132',"
        "'00000000-0000-4000-8000-000000000001',-5,303);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006134',"
        "'00000000-0000-4000-8000-000000006132',"
        "'00000000-0000-4000-8000-000000000020',5,303);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=303 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000006132'"));
  }

  {
    sanguinius::persistence::Transaction rollback{
        fixture.context->connection(),
        sanguinius::persistence::TransactionMode::immediate};
    fixture.context->connection().execute_script(
        "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
        "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
        "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
        "'00000000-0000-4000-8000-000000006141',"
        "'tarot.admin_adjusted.v1','tarot_transaction',"
        "'00000000-0000-4000-8000-000000006142','30','10','20',304,304,"
        "'test','tarot:whitespace-adjustment-reason','{}');"
        "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
        "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
        "is_test,created_at_ms) VALUES("
        "'00000000-0000-4000-8000-000000006142','TEST_ADJUSTMENT','prepared',"
        "2,'00000000-0000-4000-8000-000000006141',"
        "'tarot:whitespace-adjustment-reason:transaction','30',"
        "char(9) || char(10) || char(11) || char(12) || char(13),1,304);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006143',"
        "'00000000-0000-4000-8000-000000006142',"
        "'00000000-0000-4000-8000-000000000001',-5,304);"
        "INSERT INTO tarot_posting VALUES("
        "'00000000-0000-4000-8000-000000006144',"
        "'00000000-0000-4000-8000-000000006142',"
        "'00000000-0000-4000-8000-000000000010',5,304);");
    REQUIRE_THROWS(fixture.context->connection().execute(
        "UPDATE tarot_transaction SET state='committed',committed_at_ms=304 "
        "WHERE transaction_id='00000000-0000-4000-8000-000000006142'"));
  }

  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("Tarot invariants reject corrupt test transaction audit provenance",
          "[tarot][ledger][audit][invariant][corruption]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  fixture.context->connection().execute("DROP TRIGGER tarot_transaction_seal");
  fixture.context->connection().execute_script(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000006201',"
      "'tarot.trial_abandoned.v1','tarot_recovery_claim','unrelated-claim',"
      "NULL,'10','20',300,300,'test','tarot:corrupt-adjustment-event','{}');"
      "INSERT INTO tarot_transaction(transaction_id,transaction_type,state,"
      "expected_posting_count,event_id,idempotency_key,actor_user_id,reason,"
      "is_test,created_at_ms) VALUES("
      "'00000000-0000-4000-8000-000000006202','TEST_ADJUSTMENT','prepared',2,"
      "'00000000-0000-4000-8000-000000006201',"
      "'tarot:corrupt-adjustment-event:transaction',NULL,NULL,1,300);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000006203',"
      "'00000000-0000-4000-8000-000000006202',"
      "'00000000-0000-4000-8000-000000000001',-5,300);"
      "INSERT INTO tarot_posting VALUES("
      "'00000000-0000-4000-8000-000000006204',"
      "'00000000-0000-4000-8000-000000006202',"
      "'00000000-0000-4000-8000-000000000010',5,300);"
      "UPDATE tarot_transaction SET state='committed',committed_at_ms=300 "
      "WHERE transaction_id='00000000-0000-4000-8000-000000006202';");

  const auto report = fixture.repository->check_invariants();
  REQUIRE_FALSE(report.valid);
  REQUIRE(report.orphaned_link_count == 1);
}

TEST_CASE("completed recovery replays its committed historical balance",
          "[tarot][recovery][replay][history]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("historical-replay-lower", 300),
       .amount = -100,
       .reason = "historical replay setup",
       .transaction_id = uuid(300),
       .event_id = uuid(301),
       .system_posting_id = uuid(302),
       .human_posting_id = uuid(303)}));
  const auto pending = fixture.repository->start_recovery(
      {.invocation = fixture.call("historical-replay-start", 400),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(304),
       .draw_id = std::nullopt,
       .started_event_id = uuid(305),
       .expired_event_id = uuid(306),
       .token_ids = {uuid(307)}});
  REQUIRE(pending.status == TarotRecoveryStatus::pending);
  const auto completed = fixture.repository->complete_recovery(
      {.invocation = fixture.call("historical-replay-complete", 500),
       .token_id = uuid(307),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(308),
       .event_id = uuid(309),
       .mint_posting_id = uuid(310),
       .human_posting_id = uuid(311),
       .outbox_id = uuid(312),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(312))});
  REQUIRE(completed.balance == 25);
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("historical-replay-later", 600),
       .amount = 7,
       .reason = "later independent mutation",
       .transaction_id = uuid(313),
       .event_id = uuid(314),
       .system_posting_id = uuid(315),
       .human_posting_id = uuid(316)}));
  REQUIRE(fixture.repository->balance(30) == 32);

  const auto replay = fixture.repository->complete_recovery(
      {.invocation = fixture.call("historical-replay-duplicate", 700),
       .token_id = uuid(307),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(317),
       .event_id = uuid(318),
       .mint_posting_id = uuid(319),
       .human_posting_id = uuid(320),
       .outbox_id = uuid(321),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(321))});
  REQUIRE(replay.status == TarotRecoveryStatus::completed);
  REQUIRE(replay.balance == 25);
  REQUIRE(fixture.repository->balance(30) == 32);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='GRACE'") == 1);
}

TEST_CASE("recovery terminal transitions tolerate wall clock rollback",
          "[tarot][recovery][clock]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("rollback-clock-lower", 900),
       .amount = -100,
       .reason = "clock rollback setup",
       .transaction_id = uuid(330),
       .event_id = uuid(331),
       .system_posting_id = uuid(332),
       .human_posting_id = uuid(333)}));
  const auto grace = fixture.repository->start_recovery(
      {.invocation = fixture.call("rollback-clock-grace-start", 2'000),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(334),
       .draw_id = std::nullopt,
       .started_event_id = uuid(335),
       .expired_event_id = uuid(336),
       .token_ids = {uuid(337)}});
  REQUIRE(grace.status == TarotRecoveryStatus::pending);
  const auto completed = fixture.repository->complete_recovery(
      {.invocation = fixture.call("rollback-clock-grace-complete", 1'500),
       .token_id = uuid(337),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(338),
       .event_id = uuid(339),
       .mint_posting_id = uuid(340),
       .human_posting_id = uuid(341),
       .outbox_id = uuid(342),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(342))});
  REQUIRE(completed.status == TarotRecoveryStatus::completed);
  REQUIRE(scalar(*fixture.context,
                 "SELECT completed_at_ms FROM tarot_recovery_claim WHERE "
                 "claim_id='00000000-0000-4000-8000-000000000334'") == 2'000);
  REQUIRE(scalar(*fixture.context,
                 "SELECT occurred_at_ms FROM event_journal WHERE "
                 "event_id='00000000-0000-4000-8000-000000000339'") == 2'000);

  const auto trial = fixture.repository->start_recovery(
      {.invocation = fixture.call("rollback-clock-trial-start", 3'000),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 5, .prompt_variant = 0}; },
       .claim_id = uuid(343),
       .draw_id = uuid(344),
       .started_event_id = uuid(345),
       .expired_event_id = uuid(346),
       .token_ids = {uuid(347), uuid(348), uuid(349), uuid(350)}});
  REQUIRE(trial.status == TarotRecoveryStatus::pending);
  const auto abandoned = fixture.repository->complete_recovery(
      {.invocation = fixture.call("rollback-clock-trial-abandon", 2'500),
       .token_id = uuid(350),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(351),
       .event_id = uuid(352),
       .mint_posting_id = uuid(353),
       .human_posting_id = uuid(354),
       .outbox_id = uuid(355),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(355))});
  REQUIRE(abandoned.status == TarotRecoveryStatus::abandoned);
  REQUIRE(scalar(*fixture.context,
                 "SELECT completed_at_ms FROM tarot_recovery_claim WHERE "
                 "claim_id='00000000-0000-4000-8000-000000000343'") == 3'000);
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("Grace and Trial persist deterministic exactly-once recovery",
          "[tarot][recovery][restart][duplicate][cooldown]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  const auto adjustment = fixture.repository->adjust(
      {.invocation = fixture.call("adjust-zero", 300),
       .amount = -100,
       .reason = "bankrupt recovery test",
       .transaction_id = uuid(30),
       .event_id = uuid(31),
       .system_posting_id = uuid(32),
       .human_posting_id = uuid(33)});
  REQUIRE(adjustment.status == TarotMutationStatus::applied);
  REQUIRE(adjustment.balance == 0);

  const auto grace = fixture.repository->start_recovery(
      {.invocation = fixture.call("grace-start", 400),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::public_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(40),
       .draw_id = std::nullopt,
       .started_event_id = uuid(41),
       .expired_event_id = uuid(42),
       .token_ids = {uuid(43)}});
  REQUIRE(grace.status == TarotRecoveryStatus::pending);
  REQUIRE(grace.custom_ids.size() == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT grace_target FROM tarot_recovery_claim WHERE "
                 "claim_id='00000000-0000-4000-8000-000000000040'") == 25);

  fixture.repository = std::make_unique<SqliteTarotRepository>(fixture.context);

  const auto completed_grace = fixture.repository->complete_recovery(
      {.invocation = fixture.call("grace-click", 500),
       .token_id = uuid(43),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(50),
       .event_id = uuid(51),
       .mint_posting_id = uuid(52),
       .human_posting_id = uuid(53),
       .outbox_id = uuid(54),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(54))});
  REQUIRE(completed_grace.status == TarotRecoveryStatus::completed);
  REQUIRE(completed_grace.reward == 25);
  REQUIRE(completed_grace.balance == 25);
  REQUIRE(completed_grace.public_delivery_created);
  const auto duplicate = fixture.repository->complete_recovery(
      {.invocation = fixture.call("grace-click-duplicate", 501),
       .token_id = uuid(43),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(56),
       .event_id = uuid(57),
       .mint_posting_id = uuid(58),
       .human_posting_id = uuid(59),
       .outbox_id = uuid(60),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(60))});
  REQUIRE(duplicate.status == TarotRecoveryStatus::completed);
  REQUIRE(duplicate.balance == 25);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='GRACE'") == 1);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "aggregate_type='tarot_recovery_claim'") == 1);
  auto public_payload = fixture.context->connection().prepare(
      "SELECT payload_json FROM outbox_message WHERE outbox_id=?");
  public_payload.bind(1, uuid(54));
  REQUIRE(public_payload.step());
  REQUIRE(public_payload.column_text(0).find("25") == std::string::npos);
  REQUIRE(public_payload.column_text(0).find("reward") == std::string::npos);
  REQUIRE(public_payload.column_text(0).find(uuid(50)) == std::string::npos);
  REQUIRE(public_payload.column_text(0).find("allowed_user_mentions\":[]") !=
          std::string::npos);
  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto claimed_public =
      durable.claim_due_outbox(500, 1'500, "tarot-test", uuid(61), true);
  REQUIRE(claimed_public.has_value());
  REQUIRE(durable.fail_outbox(*claimed_public, 501, 502,
                              "injected_delivery_failure",
                              sanguinius::OutboxFailureMode::failed) ==
          sanguinius::WorkMutationStatus::applied);
  REQUIRE(fixture.repository->balance(30) == 25);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='GRACE'") == 1);
  REQUIRE(fixture.repository->check_invariants().valid);

  const auto trial = fixture.repository->start_recovery(
      {.invocation = fixture.call("trial-start", 600),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 7, .prompt_variant = 2}; },
       .claim_id = uuid(70),
       .draw_id = uuid(71),
       .started_event_id = uuid(72),
       .expired_event_id = uuid(73),
       .token_ids = {uuid(74), uuid(75), uuid(76), uuid(77)}});
  REQUIRE(trial.status == TarotRecoveryStatus::pending);
  REQUIRE(trial.reward == 7);
  REQUIRE(trial.prompt_variant == 2);
  std::size_t replay_draw_calls{};
  const auto trial_replay = fixture.repository->start_recovery(
      {.invocation = fixture.call("trial-start", 601),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++replay_draw_calls;
             return TarotTrialDraw{.reward = 15, .prompt_variant = 1};
           },
       .claim_id = uuid(160),
       .draw_id = uuid(161),
       .started_event_id = uuid(162),
       .expired_event_id = uuid(163),
       .token_ids = {uuid(164), uuid(165), uuid(166), uuid(167)}});
  REQUIRE(trial_replay.claim_id == trial.claim_id);
  REQUIRE(trial_replay.reward == 7);
  REQUIRE(replay_draw_calls == 0);
  const auto completed_trial = fixture.repository->complete_recovery(
      {.invocation = fixture.call("trial-click", 700),
       .token_id = uuid(75),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(80),
       .event_id = uuid(81),
       .mint_posting_id = uuid(82),
       .human_posting_id = uuid(83),
       .outbox_id = uuid(84),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(84))});
  REQUIRE(completed_trial.status == TarotRecoveryStatus::completed);
  REQUIRE(completed_trial.reward == 7);
  REQUIRE(completed_trial.balance == 32);
  REQUIRE_FALSE(completed_trial.public_delivery_created);
  const auto sibling = fixture.repository->complete_recovery(
      {.invocation = fixture.call("trial-sibling", 701),
       .token_id = uuid(76),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(86),
       .event_id = uuid(87),
       .mint_posting_id = uuid(88),
       .human_posting_id = uuid(89),
       .outbox_id = uuid(90),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(90))});
  REQUIRE(sibling.status == TarotRecoveryStatus::completed);
  REQUIRE(sibling.balance == 32);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "aggregate_type='tarot_recovery_claim'") == 1);

  std::size_t cooldown_draw_calls{};
  const auto cooldown = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("trial-too-soon", 700 + 24LL * 60 * 60 * 1'000 - 1),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++cooldown_draw_calls;
             return TarotTrialDraw{.reward = 9, .prompt_variant = 0};
           },
       .claim_id = uuid(100),
       .draw_id = uuid(101),
       .started_event_id = uuid(102),
       .expired_event_id = uuid(103),
       .token_ids = {uuid(104), uuid(105), uuid(106), uuid(107)}});
  REQUIRE(cooldown.status == TarotRecoveryStatus::cooldown);
  REQUIRE(cooldown_draw_calls == 0);
  std::size_t boundary_draw_calls{};
  const auto exact_boundary = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("trial-exact-boundary", 700 + 24LL * 60 * 60 * 1'000),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++boundary_draw_calls;
             return TarotTrialDraw{.reward = 5, .prompt_variant = 0};
           },
       .claim_id = uuid(130),
       .draw_id = uuid(131),
       .started_event_id = uuid(132),
       .expired_event_id = uuid(133),
       .token_ids = {uuid(134), uuid(135), uuid(136), uuid(137)}});
  REQUIRE(exact_boundary.status == TarotRecoveryStatus::pending);
  REQUIRE(boundary_draw_calls == 1);
  const auto grace_threshold_balance = fixture.repository->adjust(
      {.invocation = fixture.call("grace-threshold-setup", 750),
       .amount = -22,
       .reason = "Grace threshold boundary",
       .transaction_id = uuid(140),
       .event_id = uuid(141),
       .system_posting_id = uuid(142),
       .human_posting_id = uuid(143)});
  REQUIRE(grace_threshold_balance.balance == 10);
  const auto grace_at_threshold = fixture.repository->start_recovery(
      {.invocation = fixture.call("grace-at-threshold", 760),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(144),
       .draw_id = std::nullopt,
       .started_event_id = uuid(145),
       .expired_event_id = uuid(146),
       .token_ids = {uuid(147)}});
  REQUIRE(grace_at_threshold.status == TarotRecoveryStatus::ineligible);
  const auto below_grace_threshold = fixture.repository->adjust(
      {.invocation = fixture.call("grace-below-threshold", 770),
       .amount = -1,
       .reason = "Grace cooldown boundary",
       .transaction_id = uuid(148),
       .event_id = uuid(149),
       .system_posting_id = uuid(150),
       .human_posting_id = uuid(151)});
  REQUIRE(below_grace_threshold.balance == 9);
  const auto grace_too_soon = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("grace-too-soon", 500 + 72LL * 60 * 60 * 1'000 - 1),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(152),
       .draw_id = std::nullopt,
       .started_event_id = uuid(153),
       .expired_event_id = uuid(154),
       .token_ids = {uuid(155)}});
  REQUIRE(grace_too_soon.status == TarotRecoveryStatus::cooldown);
  const auto grace_exact_boundary = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("grace-exact-boundary", 500 + 72LL * 60 * 60 * 1'000),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(156),
       .draw_id = std::nullopt,
       .started_event_id = uuid(157),
       .expired_event_id = uuid(158),
       .token_ids = {uuid(159)}});
  REQUIRE(grace_exact_boundary.status == TarotRecoveryStatus::pending);
  const auto real_namespace = fixture.repository->start_recovery(
      {.invocation = fixture.call("trial-real-namespace", 800),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = false,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 6, .prompt_variant = 1}; },
       .claim_id = uuid(120),
       .draw_id = uuid(121),
       .started_event_id = uuid(122),
       .expired_event_id = uuid(123),
       .token_ids = {uuid(124), uuid(125), uuid(126), uuid(127)}});
  REQUIRE(real_namespace.status == TarotRecoveryStatus::pending);
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE(
    "Tarot test reversals restore balances and property sequences stay valid",
    "[tarot][property][reversal][overflow]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  std::int64_t model = 100;
  const auto adjusted = fixture.repository->adjust(
      {.invocation = fixture.call("reversible-adjustment", 800),
       .amount = 10,
       .reason = "reversal exercise",
       .transaction_id = uuid(180),
       .event_id = uuid(181),
       .system_posting_id = uuid(182),
       .human_posting_id = uuid(183)});
  REQUIRE(adjusted.balance == 110);
  const auto reversed = fixture.repository->reverse(
      {.invocation = fixture.call("reverse-adjustment", 801),
       .original_transaction_id = uuid(180),
       .reason = "restore reference state",
       .transaction_id = uuid(184),
       .event_id = uuid(185),
       .first_posting_id = uuid(186),
       .second_posting_id = uuid(187)});
  REQUIRE(reversed.status == TarotMutationStatus::applied);
  REQUIRE(reversed.balance == 100);
  const auto double_reversal = fixture.repository->reverse(
      {.invocation = fixture.call("double-reverse", 802),
       .original_transaction_id = uuid(180),
       .reason = "must fail",
       .transaction_id = uuid(188),
       .event_id = uuid(189),
       .first_posting_id = uuid(190),
       .second_posting_id = uuid(191)});
  REQUIRE(double_reversal.status == TarotMutationStatus::forbidden);
  const auto positive = fixture.repository->adjust(
      {.invocation = fixture.call("overdraw-positive", 810),
       .amount = 20,
       .reason = "overdraw reversal setup",
       .transaction_id = uuid(230),
       .event_id = uuid(231),
       .system_posting_id = uuid(232),
       .human_posting_id = uuid(233)});
  REQUIRE(positive.balance == 120);
  const auto spent = fixture.repository->adjust(
      {.invocation = fixture.call("overdraw-spend", 811),
       .amount = -120,
       .reason = "overdraw reversal setup",
       .transaction_id = uuid(234),
       .event_id = uuid(235),
       .system_posting_id = uuid(236),
       .human_posting_id = uuid(237)});
  REQUIRE(spent.balance == 0);
  const auto overdraw_reversal = fixture.repository->reverse(
      {.invocation = fixture.call("overdraw-reversal", 812),
       .original_transaction_id = uuid(230),
       .reason = "must preserve non-negative balance",
       .transaction_id = uuid(238),
       .event_id = uuid(239),
       .first_posting_id = uuid(240),
       .second_posting_id = uuid(241)});
  REQUIRE(overdraw_reversal.status == TarotMutationStatus::would_overdraw);
  const auto restore_spend = fixture.repository->reverse(
      {.invocation = fixture.call("restore-spend", 813),
       .original_transaction_id = uuid(234),
       .reason = "restore property reference state",
       .transaction_id = uuid(242),
       .event_id = uuid(243),
       .first_posting_id = uuid(244),
       .second_posting_id = uuid(245)});
  REQUIRE(restore_spend.balance == 120);
  const auto restore_positive = fixture.repository->reverse(
      {.invocation = fixture.call("restore-positive", 814),
       .original_transaction_id = uuid(230),
       .reason = "restore property reference state",
       .transaction_id = uuid(246),
       .event_id = uuid(247),
       .first_posting_id = uuid(248),
       .second_posting_id = uuid(249)});
  REQUIRE(restore_positive.balance == 100);
  const auto non_test_reversal = fixture.repository->reverse(
      {.invocation = fixture.call("non-test-reversal", 815),
       .original_transaction_id = uuid(11),
       .reason = "starting grants are permanent",
       .transaction_id = uuid(250),
       .event_id = uuid(251),
       .first_posting_id = uuid(252),
       .second_posting_id = uuid(253)});
  REQUIRE(non_test_reversal.status == TarotMutationStatus::forbidden);
  const auto lowered = fixture.repository->adjust(
      {.invocation = fixture.call("property-recovery-lower", 900),
       .amount = -95,
       .reason = "property recovery setup",
       .transaction_id = uuid(200),
       .event_id = uuid(201),
       .system_posting_id = uuid(202),
       .human_posting_id = uuid(203)});
  REQUIRE(lowered.status == TarotMutationStatus::applied);
  model = 5;
  const auto grace = fixture.repository->start_recovery(
      {.invocation = fixture.call("property-grace-start", 901),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(204),
       .draw_id = std::nullopt,
       .started_event_id = uuid(205),
       .expired_event_id = uuid(206),
       .token_ids = {uuid(207)}});
  REQUIRE(grace.status == TarotRecoveryStatus::pending);
  REQUIRE(fixture.repository->balance(30) == model);
  REQUIRE(fixture.repository->check_invariants().valid);
  const auto grace_complete = fixture.repository->complete_recovery(
      {.invocation = fixture.call("property-grace-complete", 902),
       .token_id = uuid(207),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(208),
       .event_id = uuid(209),
       .mint_posting_id = uuid(210),
       .human_posting_id = uuid(211),
       .outbox_id = uuid(212),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(212))});
  REQUIRE(grace_complete.status == TarotRecoveryStatus::completed);
  model = 25;
  REQUIRE(grace_complete.balance == model);
  REQUIRE(fixture.repository->check_invariants().valid);
  const auto trial = fixture.repository->start_recovery(
      {.invocation = fixture.call("property-trial-start", 903),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 7, .prompt_variant = 1}; },
       .claim_id = uuid(213),
       .draw_id = uuid(214),
       .started_event_id = uuid(215),
       .expired_event_id = uuid(216),
       .token_ids = {uuid(217), uuid(218), uuid(219), uuid(220)}});
  REQUIRE(trial.status == TarotRecoveryStatus::pending);
  REQUIRE(fixture.repository->balance(30) == model);
  REQUIRE(fixture.repository->check_invariants().valid);
  const auto trial_complete = fixture.repository->complete_recovery(
      {.invocation = fixture.call("property-trial-complete", 904),
       .token_id = uuid(219),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(221),
       .event_id = uuid(222),
       .mint_posting_id = uuid(223),
       .human_posting_id = uuid(224),
       .outbox_id = uuid(225),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(225))});
  REQUIRE(trial_complete.status == TarotRecoveryStatus::completed);
  model += 7;
  REQUIRE(trial_complete.balance == model);
  REQUIRE(fixture.repository->check_invariants().valid);

  std::size_t next_id = 300;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    std::uint64_t state = seed * 0x9e3779b9U;
    for (std::size_t step = 0; step < 10; ++step) {
      state = state * 1'664'525U + 1'013'904'223U;
      std::int64_t delta = static_cast<std::int64_t>((state >> 8U) % 31U) - 15;
      if (delta == 0)
        delta = 1;
      const auto key =
          "property:" + std::to_string(seed) + ":" + std::to_string(step);
      const auto call = fixture.call(
          key, 1'000 + static_cast<std::int64_t>(seed * 10 + step));
      const auto transaction_count = scalar(
          *fixture.context,
          "SELECT count(*) FROM tarot_transaction WHERE state='committed'");
      const auto result = fixture.repository->adjust(
          {.invocation = call,
           .amount = delta,
           .reason = "deterministic property sequence",
           .transaction_id = uuid(next_id++),
           .event_id = uuid(next_id++),
           .system_posting_id = uuid(next_id++),
           .human_posting_id = uuid(next_id++)});
      if (model + delta < 0) {
        REQUIRE(result.status == TarotMutationStatus::would_overdraw);
        REQUIRE(scalar(*fixture.context,
                       "SELECT count(*) FROM tarot_transaction WHERE "
                       "state='committed'") == transaction_count);
      } else {
        model += delta;
        REQUIRE(result.status == TarotMutationStatus::applied);
        REQUIRE(result.balance == model);
        REQUIRE(scalar(*fixture.context,
                       "SELECT count(*) FROM tarot_transaction WHERE "
                       "state='committed'") == transaction_count + 1);
        const auto replay = fixture.repository->adjust(
            {.invocation = call,
             .amount = delta,
             .reason = "deterministic property sequence",
             .transaction_id = uuid(next_id++),
             .event_id = uuid(next_id++),
             .system_posting_id = uuid(next_id++),
             .human_posting_id = uuid(next_id++)});
        REQUIRE(replay.status == TarotMutationStatus::unchanged);
        REQUIRE(replay.balance == model);
        REQUIRE(scalar(*fixture.context,
                       "SELECT count(*) FROM tarot_transaction WHERE "
                       "state='committed'") == transaction_count + 1);
      }
      REQUIRE(fixture.repository->balance(30) == model);
      REQUIRE(scalar(*fixture.context,
                     "SELECT CAST(total(amount) AS INTEGER) FROM "
                     "tarot_posting") == 0);
      REQUIRE(fixture.repository->check_invariants().valid);
    }
  }
  std::vector<std::string> snapshot_tokens;
  for (std::size_t index = 0; index < 9; ++index)
    snapshot_tokens.push_back(uuid(2'001 + index));
  const auto bounded_history = fixture.repository->create_history_snapshot(
      {.invocation = fixture.call("property-history", 3'000),
       .cursor_id = uuid(2'000),
       .page_token_ids = snapshot_tokens});
  REQUIRE(bounded_history.status == sanguinius::TarotPageStatus::available);
  REQUIRE(bounded_history.total == sanguinius::tarot_history_maximum_items);
  REQUIRE(bounded_history.entries.size() ==
          sanguinius::tarot_history_page_size);
  REQUIRE(bounded_history.next_custom_id.has_value());
}

TEST_CASE("Rejected Tarot admin mutations replay after balances change",
          "[tarot][adjustment][reversal][idempotency][replay]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("receipt-drain", 300),
       .amount = -100,
       .reason = "drain for rejected adjustment",
       .transaction_id = uuid(5'000),
       .event_id = uuid(5'001),
       .system_posting_id = uuid(5'002),
       .human_posting_id = uuid(5'003)}));
  const auto rejected_adjustment = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-rejected-adjustment", 301),
       .amount = -1,
       .reason = "must remain rejected",
       .transaction_id = uuid(5'010),
       .event_id = uuid(5'011),
       .system_posting_id = uuid(5'012),
       .human_posting_id = uuid(5'013)});
  REQUIRE(rejected_adjustment.status == TarotMutationStatus::would_overdraw);
  REQUIRE(rejected_adjustment.balance == 0);
  const auto credit = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-credit", 302),
       .amount = 10,
       .reason = "change balance after rejection",
       .transaction_id = uuid(5'020),
       .event_id = uuid(5'021),
       .system_posting_id = uuid(5'022),
       .human_posting_id = uuid(5'023)});
  REQUIRE(credit.balance == 10);
  const auto rejected_adjustment_replay = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-rejected-adjustment", 303),
       .amount = -1,
       .reason = "must remain rejected",
       .transaction_id = uuid(5'030),
       .event_id = uuid(5'031),
       .system_posting_id = uuid(5'032),
       .human_posting_id = uuid(5'033)});
  REQUIRE(rejected_adjustment_replay.status ==
          TarotMutationStatus::would_overdraw);
  REQUIRE(rejected_adjustment_replay.balance == 0);
  REQUIRE(fixture.repository->balance(30) == 10);

  const auto positive = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-positive", 304),
       .amount = 20,
       .reason = "positive reversal target",
       .transaction_id = uuid(5'040),
       .event_id = uuid(5'041),
       .system_posting_id = uuid(5'042),
       .human_posting_id = uuid(5'043)});
  REQUIRE(positive.balance == 30);
  const auto spend = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-spend", 305),
       .amount = -30,
       .reason = "make reversal overdraw",
       .transaction_id = uuid(5'050),
       .event_id = uuid(5'051),
       .system_posting_id = uuid(5'052),
       .human_posting_id = uuid(5'053)});
  REQUIRE(spend.balance == 0);
  const auto rejected_reversal = fixture.repository->reverse(
      {.invocation = fixture.call("receipt-rejected-reversal", 306),
       .original_transaction_id = uuid(5'040),
       .reason = "must remain overdrawn",
       .transaction_id = uuid(5'060),
       .event_id = uuid(5'061),
       .first_posting_id = uuid(5'062),
       .second_posting_id = uuid(5'063)});
  REQUIRE(rejected_reversal.status == TarotMutationStatus::would_overdraw);
  const auto restore = fixture.repository->reverse(
      {.invocation = fixture.call("receipt-restore-spend", 307),
       .original_transaction_id = uuid(5'050),
       .reason = "change balance after reversal rejection",
       .transaction_id = uuid(5'070),
       .event_id = uuid(5'071),
       .first_posting_id = uuid(5'072),
       .second_posting_id = uuid(5'073)});
  REQUIRE(restore.balance == 30);
  const auto rejected_reversal_replay = fixture.repository->reverse(
      {.invocation = fixture.call("receipt-rejected-reversal", 308),
       .original_transaction_id = uuid(5'040),
       .reason = "must remain overdrawn",
       .transaction_id = uuid(5'080),
       .event_id = uuid(5'081),
       .first_posting_id = uuid(5'082),
       .second_posting_id = uuid(5'083)});
  REQUIRE(rejected_reversal_replay.status ==
          TarotMutationStatus::would_overdraw);
  REQUIRE(rejected_reversal_replay.balance == 0);
  REQUIRE(fixture.repository->balance(30) == 30);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "reversal_of_transaction_id='00000000-0000-4000-8000-"
                 "000000005040'") == 0);

  const auto applied_replay = fixture.repository->adjust(
      {.invocation = fixture.call("receipt-credit", 309),
       .amount = 10,
       .reason = "change balance after rejection",
       .transaction_id = uuid(5'090),
       .event_id = uuid(5'091),
       .system_posting_id = uuid(5'092),
       .human_posting_id = uuid(5'093)});
  REQUIRE(applied_replay.status == TarotMutationStatus::unchanged);
  REQUIRE(applied_replay.transaction_id == uuid(5'020));
  REQUIRE(applied_replay.balance == 10);
  REQUIRE(fixture.repository->balance(30) == 30);
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_interaction_receipt SET created_at_ms=999"));
  REQUIRE_THROWS(fixture.context->connection().execute(
      "DELETE FROM tarot_interaction_receipt"));
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("mixed Tarot property sequences preserve the reference ledger model",
          "[tarot][property][recovery][reversal][idempotency]") {
  struct ReversibleTransaction {
    std::string transaction_id;
    std::int64_t human_amount{};
    bool reversed{};
  };
  struct HistoryPosting {
    std::string transaction_id;
    std::int64_t amount{};
    bool operator==(const HistoryPosting &) const = default;
  };

  constexpr std::size_t steps_per_seed = 18;
  constexpr std::int64_t step_interval_ms = 73LL * 60 * 60 * 1'000;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    TarotFixture fixture;
    const auto provisioned = fixture.provision(10);
    REQUIRE(provisioned.created);
    std::int64_t model = 100;
    std::size_t next_id = 200 + static_cast<std::size_t>(seed) * 2'000;
    std::uint64_t state = seed * 0x9e3779b97f4a7c15ULL;
    std::vector<ReversibleTransaction> reversible;
    std::vector<HistoryPosting> immutable_prefix;
    bool saw_grant_replay{};
    bool saw_adjustment{};
    bool saw_grace{};
    bool saw_trial{};
    bool saw_reversal{};

    const auto fresh_id = [&] { return uuid(next_id++); };
    const auto apply_adjustment = [&](const std::int64_t amount,
                                      const std::string &key,
                                      const std::int64_t now_ms) {
      REQUIRE(amount != 0);
      const auto call = fixture.call(key, now_ms);
      const auto transaction_id = fresh_id();
      const auto result = fixture.repository->adjust(
          {.invocation = call,
           .amount = amount,
           .reason = "mixed deterministic property sequence",
           .transaction_id = transaction_id,
           .event_id = fresh_id(),
           .system_posting_id = fresh_id(),
           .human_posting_id = fresh_id()});
      REQUIRE(result.status == TarotMutationStatus::applied);
      model += amount;
      REQUIRE(result.balance == model);
      reversible.push_back({.transaction_id = transaction_id,
                            .human_amount = amount,
                            .reversed = false});
      const auto replay = fixture.repository->adjust(
          {.invocation = call,
           .amount = amount,
           .reason = "mixed deterministic property sequence",
           .transaction_id = fresh_id(),
           .event_id = fresh_id(),
           .system_posting_id = fresh_id(),
           .human_posting_id = fresh_id()});
      REQUIRE(replay.status == TarotMutationStatus::unchanged);
      REQUIRE(replay.balance == model);
      saw_adjustment = true;
    };

    for (std::size_t step = 0; step < steps_per_seed; ++step) {
      state =
          state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
      const auto action = (step + static_cast<std::size_t>(seed)) % 6;
      const auto now_ms =
          1'000'000 + static_cast<std::int64_t>(step + 1) * step_interval_ms;
      const auto key =
          "mixed:" + std::to_string(seed) + ":" + std::to_string(step);

      if (action == 0) {
        const auto replay = fixture.provision(next_id);
        next_id += 5;
        REQUIRE_FALSE(replay.created);
        REQUIRE(replay.balance == model);
        REQUIRE(scalar(*fixture.context,
                       "SELECT count(*) FROM tarot_transaction WHERE "
                       "transaction_type='STARTING_GRANT'") == 1);
        saw_grant_replay = true;
      } else if (action == 1) {
        auto amount = static_cast<std::int64_t>((state >> 16U) % 41U) - 20;
        if (amount == 0)
          amount = 1;
        if (model + amount < 0)
          amount = -model;
        if (amount == 0)
          amount = 1;
        apply_adjustment(amount, key + ":adjust", now_ms);
      } else if (action == 2) {
        const auto candidate = std::find_if(
            reversible.rbegin(), reversible.rend(), [&](const auto &entry) {
              return !entry.reversed && model - entry.human_amount >= 0;
            });
        if (candidate == reversible.rend()) {
          const auto forbidden = fixture.repository->reverse(
              {.invocation = fixture.call(key + ":forbidden", now_ms),
               .original_transaction_id = uuid(11),
               .reason = "starting grant remains immutable",
               .transaction_id = fresh_id(),
               .event_id = fresh_id(),
               .first_posting_id = fresh_id(),
               .second_posting_id = fresh_id()});
          REQUIRE(forbidden.status == TarotMutationStatus::forbidden);
        } else {
          const auto call = fixture.call(key + ":reverse", now_ms);
          const auto transaction_id = fresh_id();
          const auto reversed = fixture.repository->reverse(
              {.invocation = call,
               .original_transaction_id = candidate->transaction_id,
               .reason = "mixed deterministic exact reversal",
               .transaction_id = transaction_id,
               .event_id = fresh_id(),
               .first_posting_id = fresh_id(),
               .second_posting_id = fresh_id()});
          REQUIRE(reversed.status == TarotMutationStatus::applied);
          model -= candidate->human_amount;
          candidate->reversed = true;
          REQUIRE(reversed.balance == model);
          const auto replay = fixture.repository->reverse(
              {.invocation = call,
               .original_transaction_id = candidate->transaction_id,
               .reason = "mixed deterministic exact reversal",
               .transaction_id = fresh_id(),
               .event_id = fresh_id(),
               .first_posting_id = fresh_id(),
               .second_posting_id = fresh_id()});
          REQUIRE(replay.status == TarotMutationStatus::unchanged);
          REQUIRE(replay.transaction_id == transaction_id);
          REQUIRE(replay.balance == model);
          saw_reversal = true;
        }
      } else if (action == 3) {
        if (model >= 10)
          apply_adjustment(9 - model, key + ":grace-setup", now_ms);
        const auto claim_id = fresh_id();
        const auto started_event_id = fresh_id();
        const auto expired_event_id = fresh_id();
        const auto token_id = fresh_id();
        const auto pending = fixture.repository->start_recovery(
            {.invocation = fixture.call(key + ":grace-start", now_ms + 1),
             .kind = TarotRecoveryKind::grace,
             .visibility = TarotVisibility::private_result,
             .is_test = true,
             .threshold = 10,
             .grace_target = 25,
             .cooldown_ms = 72LL * 60 * 60 * 1'000,
             .trial_draw = {},
             .claim_id = claim_id,
             .draw_id = std::nullopt,
             .started_event_id = started_event_id,
             .expired_event_id = expired_event_id,
             .token_ids = {token_id}});
        REQUIRE(pending.status == TarotRecoveryStatus::pending);
        const auto prior_balance = model;
        const auto transaction_id = fresh_id();
        const auto completed = fixture.repository->complete_recovery(
            {.invocation = fixture.call(key + ":grace-complete", now_ms + 2),
             .token_id = token_id,
             .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
             .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
             .transaction_id = transaction_id,
             .event_id = fresh_id(),
             .mint_posting_id = fresh_id(),
             .human_posting_id = fresh_id(),
             .outbox_id = fresh_id(),
             .provider_nonce =
                 sanguinius::discord_nonce_from_uuid(uuid(next_id - 1))});
        REQUIRE(completed.status == TarotRecoveryStatus::completed);
        model = 25;
        REQUIRE(completed.balance == model);
        reversible.push_back({.transaction_id = transaction_id,
                              .human_amount = 25 - prior_balance,
                              .reversed = false});
        const auto replay = fixture.repository->complete_recovery(
            {.invocation = fixture.call(key + ":grace-replay", now_ms + 3),
             .token_id = token_id,
             .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
             .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
             .transaction_id = fresh_id(),
             .event_id = fresh_id(),
             .mint_posting_id = fresh_id(),
             .human_posting_id = fresh_id(),
             .outbox_id = fresh_id(),
             .provider_nonce =
                 sanguinius::discord_nonce_from_uuid(uuid(next_id - 1))});
        REQUIRE(replay.status == TarotRecoveryStatus::completed);
        REQUIRE(replay.balance == model);
        saw_grace = true;
      } else if (action == 4) {
        if (model >= 50)
          apply_adjustment(49 - model, key + ":trial-setup", now_ms);
        const auto reward = 5 + static_cast<std::int64_t>((state >> 24U) % 11U);
        const auto variant = static_cast<std::int64_t>((state >> 32U) % 3U);
        const auto claim_id = fresh_id();
        const auto draw_id = fresh_id();
        const auto started_event_id = fresh_id();
        const auto expired_event_id = fresh_id();
        std::vector<std::string> tokens;
        for (std::size_t index = 0; index < 4; ++index)
          tokens.push_back(fresh_id());
        const auto start_call = fixture.call(key + ":trial-start", now_ms + 1);
        const auto pending = fixture.repository->start_recovery(
            {.invocation = start_call,
             .kind = TarotRecoveryKind::trial,
             .visibility = TarotVisibility::private_result,
             .is_test = true,
             .threshold = 50,
             .grace_target = std::nullopt,
             .cooldown_ms = 24LL * 60 * 60 * 1'000,
             .trial_draw =
                 [=] {
                   return TarotTrialDraw{.reward = reward,
                                         .prompt_variant = variant};
                 },
             .claim_id = claim_id,
             .draw_id = draw_id,
             .started_event_id = started_event_id,
             .expired_event_id = expired_event_id,
             .token_ids = tokens});
        REQUIRE(pending.status == TarotRecoveryStatus::pending);
        std::size_t replay_draw_calls{};
        const auto start_replay = fixture.repository->start_recovery(
            {.invocation = start_call,
             .kind = TarotRecoveryKind::trial,
             .visibility = TarotVisibility::private_result,
             .is_test = true,
             .threshold = 50,
             .grace_target = std::nullopt,
             .cooldown_ms = 24LL * 60 * 60 * 1'000,
             .trial_draw =
                 [&] {
                   ++replay_draw_calls;
                   return TarotTrialDraw{.reward = 15, .prompt_variant = 2};
                 },
             .claim_id = fresh_id(),
             .draw_id = fresh_id(),
             .started_event_id = fresh_id(),
             .expired_event_id = fresh_id(),
             .token_ids = {fresh_id(), fresh_id(), fresh_id(), fresh_id()}});
        REQUIRE(start_replay.claim_id == claim_id);
        REQUIRE(start_replay.reward == reward);
        REQUIRE(replay_draw_calls == 0);
        const auto transaction_id = fresh_id();
        const auto vow = static_cast<std::size_t>((state >> 40U) % 3U);
        const auto completed = fixture.repository->complete_recovery(
            {.invocation = fixture.call(key + ":trial-complete", now_ms + 2),
             .token_id = tokens[vow],
             .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
             .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
             .transaction_id = transaction_id,
             .event_id = fresh_id(),
             .mint_posting_id = fresh_id(),
             .human_posting_id = fresh_id(),
             .outbox_id = fresh_id(),
             .provider_nonce =
                 sanguinius::discord_nonce_from_uuid(uuid(next_id - 1))});
        REQUIRE(completed.status == TarotRecoveryStatus::completed);
        model += reward;
        REQUIRE(completed.balance == model);
        reversible.push_back({.transaction_id = transaction_id,
                              .human_amount = reward,
                              .reversed = false});
        const auto sibling = fixture.repository->complete_recovery(
            {.invocation = fixture.call(key + ":trial-sibling", now_ms + 3),
             .token_id = tokens[(vow + 1) % 3],
             .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
             .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
             .transaction_id = fresh_id(),
             .event_id = fresh_id(),
             .mint_posting_id = fresh_id(),
             .human_posting_id = fresh_id(),
             .outbox_id = fresh_id(),
             .provider_nonce =
                 sanguinius::discord_nonce_from_uuid(uuid(next_id - 1))});
        REQUIRE(sibling.status == TarotRecoveryStatus::completed);
        REQUIRE(sibling.balance == model);
        saw_trial = true;
      } else {
        const auto rejected = fixture.repository->adjust(
            {.invocation = fixture.call(key + ":overdraw", now_ms),
             .amount = -(model + 1),
             .reason = "mixed deterministic overdraw",
             .transaction_id = fresh_id(),
             .event_id = fresh_id(),
             .system_posting_id = fresh_id(),
             .human_posting_id = fresh_id()});
        REQUIRE(rejected.status == TarotMutationStatus::would_overdraw);
        REQUIRE(rejected.balance == model);
      }

      REQUIRE(fixture.repository->balance(30) == model);
      REQUIRE(scalar(*fixture.context,
                     "SELECT CAST(total(amount) AS INTEGER) FROM "
                     "tarot_posting") == 0);
      REQUIRE(fixture.repository->check_invariants().valid);
      std::vector<HistoryPosting> current_history;
      auto history = fixture.context->connection().prepare(
          "SELECT posting.transaction_id, posting.amount "
          "FROM tarot_posting posting JOIN tarot_transaction tx "
          "ON tx.transaction_id = posting.transaction_id "
          "JOIN tarot_account account ON account.account_id = "
          "posting.account_id "
          "WHERE account.user_id = '30' AND tx.state = 'committed' "
          "ORDER BY tx.ledger_sequence");
      while (history.step())
        current_history.push_back({.transaction_id = history.column_text(0),
                                   .amount = history.column_int64(1)});
      REQUIRE(current_history.size() >= immutable_prefix.size());
      REQUIRE(std::equal(immutable_prefix.begin(), immutable_prefix.end(),
                         current_history.begin()));
      immutable_prefix = std::move(current_history);
    }

    REQUIRE(saw_grant_replay);
    REQUIRE(saw_adjustment);
    REQUIRE(saw_grace);
    REQUIRE(saw_trial);
    REQUIRE(saw_reversal);
  }
}

TEST_CASE("Tarot recovery rolls back ledger event and claim when outbox "
          "insertion fails",
          "[tarot][recovery][outbox][rollback]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("rollback-lower", 300),
       .amount = -100,
       .reason = "atomic rollback setup",
       .transaction_id = uuid(30),
       .event_id = uuid(31),
       .system_posting_id = uuid(32),
       .human_posting_id = uuid(33)}));
  const auto pending = fixture.repository->start_recovery(
      {.invocation = fixture.call("rollback-start", 400),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::public_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(40),
       .draw_id = std::nullopt,
       .started_event_id = uuid(41),
       .expired_event_id = uuid(42),
       .token_ids = {uuid(43)}});
  REQUIRE(pending.status == TarotRecoveryStatus::pending);

  sanguinius::persistence::SqliteDurableWorkRepository durable{fixture.context};
  const auto collision_event = sanguinius::EventJournalEntry{
      .event_id = uuid(50),
      .event_type = "owner_test.outbox_collision.v1",
      .aggregate_type = "owner_test",
      .aggregate_id = "tarot-rollback",
      .actor_user_id = 30,
      .guild_id = 10,
      .channel_id = 20,
      .source_message_id = std::nullopt,
      .occurred_at_ms = 450,
      .recorded_at_ms = 450,
      .correlation_id = "tarot-rollback",
      .causation_id = std::nullopt,
      .idempotency_key = "tarot:rollback:collision:event",
      .payload_json = "{}"};
  const auto collision_outbox = sanguinius::OutboxEnqueue{
      .outbox_id = uuid(51),
      .kind = std::string{sanguinius::public_discord_outbox_kind},
      .aggregate_type = "owner_test",
      .aggregate_id = "tarot-rollback",
      .target_guild_id = 10,
      .target_channel_id = 20,
      .target_user_id = std::nullopt,
      .available_at_ms = 450,
      .max_attempts = 5,
      .idempotency_key = "tarot:rollback:collision:outbox",
      .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(51)),
      .created_at_ms = 450};
  REQUIRE(durable.enqueue_public(
      collision_event, collision_outbox,
      {.request = {.guild_id = 10,
                   .channel_id = 20,
                   .message = sanguinius::text_message("collision")},
       .fail_before_first_send = false}));

  REQUIRE_THROWS(fixture.repository->complete_recovery(
      {.invocation = fixture.call("rollback-complete", 500),
       .token_id = uuid(43),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(52),
       .event_id = uuid(53),
       .mint_posting_id = uuid(54),
       .human_posting_id = uuid(55),
       .outbox_id = uuid(51),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(51))}));
  REQUIRE(fixture.repository->balance(30) == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='GRACE'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_id="
                 "'00000000-0000-4000-8000-000000000053'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_recovery_claim WHERE "
                 "claim_id='00000000-0000-4000-8000-000000000040' AND "
                 "state='pending'") == 1);
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("each persisted Trial vow completes exactly once",
          "[tarot][trial][vows][idempotency]") {
  TarotFixture fixture;
  const std::array<sanguinius::DiscordSnowflake, 3> users{9, 30, 31};
  for (std::size_t index = 0; index < users.size(); ++index) {
    const auto user = users[index];
    static_cast<void>(fixture.provision(10 + index * 10, user));
    const auto base = 100 + index * 30;
    const auto adjusted = fixture.repository->adjust(
        {.invocation =
             fixture.call("vow-adjust:" + std::to_string(index),
                          400 + static_cast<std::int64_t>(index), user),
         .amount = -60,
         .reason = "vow coverage",
         .transaction_id = uuid(base),
         .event_id = uuid(base + 1),
         .system_posting_id = uuid(base + 2),
         .human_posting_id = uuid(base + 3)});
    REQUIRE(adjusted.balance == 40);
    const auto pending = fixture.repository->start_recovery(
        {.invocation =
             fixture.call("vow-start:" + std::to_string(index),
                          410 + static_cast<std::int64_t>(index), user),
         .kind = TarotRecoveryKind::trial,
         .visibility = TarotVisibility::private_result,
         .is_test = true,
         .threshold = 50,
         .grace_target = std::nullopt,
         .cooldown_ms = 24LL * 60 * 60 * 1'000,
         .trial_draw =
             [index] {
               return TarotTrialDraw{
                   .reward = 5 + static_cast<std::int64_t>(index),
                   .prompt_variant = static_cast<std::int64_t>(index)};
             },
         .claim_id = uuid(base + 4),
         .draw_id = uuid(base + 5),
         .started_event_id = uuid(base + 6),
         .expired_event_id = uuid(base + 7),
         .token_ids = {uuid(base + 8), uuid(base + 9), uuid(base + 10),
                       uuid(base + 11)}});
    REQUIRE(pending.status == TarotRecoveryStatus::pending);
    const auto completed = fixture.repository->complete_recovery(
        {.invocation =
             fixture.call("vow-complete:" + std::to_string(index),
                          420 + static_cast<std::int64_t>(index), user),
         .token_id = uuid(base + 8 + index),
         .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
         .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
         .transaction_id = uuid(base + 12),
         .event_id = uuid(base + 13),
         .mint_posting_id = uuid(base + 14),
         .human_posting_id = uuid(base + 15),
         .outbox_id = uuid(base + 16),
         .provider_nonce =
             sanguinius::discord_nonce_from_uuid(uuid(base + 16))});
    REQUIRE(completed.status == TarotRecoveryStatus::completed);
    REQUIRE(completed.reward == 5 + static_cast<std::int64_t>(index));
    REQUIRE(completed.balance == 45 + static_cast<std::int64_t>(index));
    const auto sibling = fixture.repository->complete_recovery(
        {.invocation =
             fixture.call("vow-sibling:" + std::to_string(index),
                          430 + static_cast<std::int64_t>(index), user),
         .token_id = uuid(base + 8 + ((index + 1) % 3)),
         .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
         .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
         .transaction_id = uuid(base + 17),
         .event_id = uuid(base + 18),
         .mint_posting_id = uuid(base + 19),
         .human_posting_id = uuid(base + 20),
         .outbox_id = uuid(base + 21),
         .provider_nonce =
             sanguinius::discord_nonce_from_uuid(uuid(base + 21))});
    REQUIRE(sibling.status == TarotRecoveryStatus::completed);
    REQUIRE(sibling.balance == completed.balance);
    REQUIRE(fixture.repository->check_invariants().valid);
  }
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='TRIAL'") == 3);
}

TEST_CASE(
    "Tarot standings opt-out and bounded history snapshots preserve privacy",
    "[tarot][history][standings][privacy]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(30, 9));
  static_cast<void>(fixture.provision(10, 30));
  static_cast<void>(fixture.provision(20, 31));
  auto standings = fixture.repository->standings();
  REQUIRE(standings.size() == 3);
  REQUIRE(standings[0].user_id == sanguinius::DiscordSnowflake{9});
  REQUIRE(standings[1].user_id == sanguinius::DiscordSnowflake{30});
  REQUIRE(standings[2].user_id == sanguinius::DiscordSnowflake{31});

  const auto hidden = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("standings-private", 300, 31),
       .public_standings = false,
       .event_id = uuid(40)});
  REQUIRE(hidden.changed);
  REQUIRE_FALSE(hidden.public_standings);
  standings = fixture.repository->standings();
  REQUIRE(standings.size() == 2);
  REQUIRE(standings[0].user_id == sanguinius::DiscordSnowflake{9});
  REQUIRE(standings[1].user_id == sanguinius::DiscordSnowflake{30});
  REQUIRE(fixture.repository->balance(31) == 100);

  std::vector<std::string> page_tokens;
  for (std::size_t index = 0; index < 9; ++index)
    page_tokens.push_back(uuid(60 + index));
  const auto history = fixture.repository->create_history_snapshot(
      {.invocation = fixture.call("history", 400),
       .cursor_id = uuid(50),
       .page_token_ids = page_tokens});
  REQUIRE(history.status == sanguinius::TarotPageStatus::available);
  REQUIRE(history.total == 1);
  REQUIRE(history.entries.size() == 1);
  REQUIRE_FALSE(history.next_custom_id);

  for (std::size_t step = 0; step < 6; ++step) {
    const auto base = 100 + step * 4;
    const auto result = fixture.repository->adjust(
        {.invocation = fixture.call("history-adjust:" + std::to_string(step),
                                    500 + static_cast<std::int64_t>(step)),
         .amount = 1,
         .reason = "history pagination",
         .transaction_id = uuid(base),
         .event_id = uuid(base + 1),
         .system_posting_id = uuid(base + 2),
         .human_posting_id = uuid(base + 3)});
    REQUIRE(result.status == TarotMutationStatus::applied);
  }
  page_tokens.clear();
  for (std::size_t index = 0; index < 9; ++index)
    page_tokens.push_back(uuid(160 + index));
  const auto paged = fixture.repository->create_history_snapshot(
      {.invocation = fixture.call("history-paged", 600),
       .cursor_id = uuid(150),
       .page_token_ids = page_tokens});
  REQUIRE(paged.total == 7);
  REQUIRE(paged.entries.size() == 5);
  REQUIRE(paged.next_custom_id ==
          std::string{sanguinius::tarot_component_prefix} + uuid(160));
  const auto second = fixture.repository->history_page(
      {.invocation = fixture.call("history-next", 601), .token_id = uuid(160)});
  REQUIRE(second.status == sanguinius::TarotPageStatus::available);
  REQUIRE(second.offset == 5);
  REQUIRE(second.entries.size() == 2);
  const auto wrong_user = fixture.repository->history_page(
      {.invocation = fixture.call("history-wrong-user", 601, 31),
       .token_id = uuid(160)});
  REQUIRE(wrong_user.status == sanguinius::TarotPageStatus::wrong_user);
  auto wrong_scope_call = fixture.call("history-wrong-scope", 601);
  wrong_scope_call.channel_id = 21;
  const auto wrong_scope = fixture.repository->history_page(
      {.invocation = wrong_scope_call, .token_id = uuid(160)});
  REQUIRE(wrong_scope.status == sanguinius::TarotPageStatus::wrong_scope);
  const auto expired = fixture.repository->history_page(
      {.invocation = fixture.call(
           "history-expired", 600 + sanguinius::tarot_interaction_lifetime_ms),
       .token_id = uuid(160)});
  REQUIRE(expired.status == sanguinius::TarotPageStatus::expired);
}

TEST_CASE("Tarot standings visibility replays a superseded interaction",
          "[tarot][standings][privacy][idempotency]") {
  TarotFixture fixture;
  const auto provisioned = fixture.provision(10);
  const auto first = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-original", 300),
       .public_standings = false,
       .event_id = uuid(360)});
  REQUIRE(first.changed);
  REQUIRE_FALSE(first.public_standings);
  const auto second = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-newer", 400),
       .public_standings = true,
       .event_id = uuid(361)});
  REQUIRE(second.changed);
  REQUIRE(second.public_standings);

  const auto replay = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-original", 500),
       .public_standings = false,
       .event_id = uuid(362)});
  REQUIRE_FALSE(replay.changed);
  REQUIRE_FALSE(replay.public_standings);
  REQUIRE(fixture.repository->standings_visibility(30));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'tarot.standings_visibility_changed.v1'") == 2);
  auto visibility_event = fixture.context->connection().prepare(
      "SELECT aggregate_type,aggregate_id FROM event_journal WHERE event_id=?");
  visibility_event.bind(1, uuid(360));
  REQUIRE(visibility_event.step());
  REQUIRE(visibility_event.column_text(0) == "tarot_account");
  REQUIRE(visibility_event.column_text(1) == provisioned.account_id);
  REQUIRE_THROWS(fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-original", 600),
       .public_standings = true,
       .event_id = uuid(363)}));
  REQUIRE(fixture.repository->standings_visibility(30));
}

TEST_CASE("Tarot no-op visibility receipts cannot undo a later opt-out",
          "[tarot][standings][privacy][idempotency][replay]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  const auto no_op = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-no-op", 300),
       .public_standings = true,
       .event_id = uuid(3'600)});
  REQUIRE_FALSE(no_op.changed);
  REQUIRE(no_op.public_standings);

  const auto opted_out = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-opt-out", 400),
       .public_standings = false,
       .event_id = uuid(3'601)});
  REQUIRE(opted_out.changed);
  REQUIRE_FALSE(fixture.repository->standings_visibility(30));

  const auto late_duplicate = fixture.repository->set_standings_visibility(
      {.invocation = fixture.call("visibility-no-op", 500),
       .public_standings = true,
       .event_id = uuid(3'602)});
  REQUIRE_FALSE(late_duplicate.changed);
  REQUIRE(late_duplicate.public_standings);
  REQUIRE_FALSE(fixture.repository->standings_visibility(30));
  REQUIRE(fixture.repository->standings().empty());
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_interaction_receipt WHERE "
                 "operation='standings_visibility'") == 2);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'tarot.standings_visibility_changed.v1'") == 1);
}

TEST_CASE("Tarot recovery start receipts survive eligibility cooldown and "
          "privacy changes",
          "[tarot][recovery][privacy][idempotency][replay]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));

  std::size_t ineligible_draws{};
  const auto ineligible = fixture.repository->start_recovery(
      {.invocation = fixture.call("late-ineligible", 300),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++ineligible_draws;
             return TarotTrialDraw{.reward = 5, .prompt_variant = 0};
           },
       .claim_id = uuid(4'000),
       .draw_id = uuid(4'001),
       .started_event_id = uuid(4'002),
       .expired_event_id = uuid(4'003),
       .token_ids = {uuid(4'004), uuid(4'005), uuid(4'006), uuid(4'007)}});
  REQUIRE(ineligible.status == TarotRecoveryStatus::ineligible);
  REQUIRE(ineligible.balance == 100);
  REQUIRE(ineligible_draws == 0);
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("late-ineligible-lower", 400),
       .amount = -60,
       .reason = "make the delayed request eligible",
       .transaction_id = uuid(4'010),
       .event_id = uuid(4'011),
       .system_posting_id = uuid(4'012),
       .human_posting_id = uuid(4'013)}));
  const auto ineligible_replay = fixture.repository->start_recovery(
      {.invocation = fixture.call("late-ineligible", 450),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++ineligible_draws;
             return TarotTrialDraw{.reward = 15, .prompt_variant = 2};
           },
       .claim_id = uuid(4'020),
       .draw_id = uuid(4'021),
       .started_event_id = uuid(4'022),
       .expired_event_id = uuid(4'023),
       .token_ids = {uuid(4'024), uuid(4'025), uuid(4'026), uuid(4'027)}});
  REQUIRE(ineligible_replay.status == TarotRecoveryStatus::ineligible);
  REQUIRE(ineligible_replay.balance == 100);
  REQUIRE(ineligible_draws == 0);
  REQUIRE(scalar(*fixture.context, "SELECT count(*) FROM tarot_draw") == 0);

  const auto public_claim = fixture.repository->start_recovery(
      {.invocation = fixture.call("public-active", 500),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::public_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 7, .prompt_variant = 1}; },
       .claim_id = uuid(4'030),
       .draw_id = uuid(4'031),
       .started_event_id = uuid(4'032),
       .expired_event_id = uuid(4'033),
       .token_ids = {uuid(4'034), uuid(4'035), uuid(4'036), uuid(4'037)}});
  REQUIRE(public_claim.status == TarotRecoveryStatus::pending);
  REQUIRE(public_claim.visibility == TarotVisibility::public_result);

  std::size_t private_reuse_draws{};
  const auto private_claim = fixture.repository->start_recovery(
      {.invocation = fixture.call("private-active", 501),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++private_reuse_draws;
             return TarotTrialDraw{.reward = 15, .prompt_variant = 2};
           },
       .claim_id = uuid(4'040),
       .draw_id = uuid(4'041),
       .started_event_id = uuid(4'042),
       .expired_event_id = uuid(4'043),
       .token_ids = {uuid(4'044), uuid(4'045), uuid(4'046), uuid(4'047)}});
  REQUIRE(private_claim.status == TarotRecoveryStatus::pending);
  REQUIRE(private_claim.claim_id == public_claim.claim_id);
  REQUIRE(private_claim.visibility == TarotVisibility::private_result);
  REQUIRE(private_reuse_draws == 0);
  REQUIRE(private_claim.committed_event_types ==
          std::vector<std::string>{"tarot.recovery_visibility_tightened.v1"});
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_recovery_claim SET visibility='public' WHERE claim_id="
      "'00000000-0000-4000-8000-000000004030'"));
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM event_journal WHERE event_type="
                 "'tarot.recovery_visibility_tightened.v1'") == 1);

  const auto completed = fixture.repository->complete_recovery(
      {.invocation = fixture.call("private-active-complete", 520),
       .token_id = uuid(4'034),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(4'050),
       .event_id = uuid(4'051),
       .mint_posting_id = uuid(4'052),
       .human_posting_id = uuid(4'053),
       .outbox_id = uuid(4'054),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(4'054))});
  REQUIRE(completed.status == TarotRecoveryStatus::completed);
  REQUIRE(completed.visibility == TarotVisibility::private_result);
  REQUIRE_FALSE(completed.public_delivery_created);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM outbox_message WHERE "
                 "aggregate_type='tarot_recovery_claim'") == 0);

  const auto active_replay = fixture.repository->start_recovery(
      {.invocation = fixture.call("private-active", 600),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++private_reuse_draws;
             return TarotTrialDraw{.reward = 15, .prompt_variant = 2};
           },
       .claim_id = uuid(4'060),
       .draw_id = uuid(4'061),
       .started_event_id = uuid(4'062),
       .expired_event_id = uuid(4'063),
       .token_ids = {uuid(4'064), uuid(4'065), uuid(4'066), uuid(4'067)}});
  REQUIRE(active_replay.status == TarotRecoveryStatus::completed);
  REQUIRE(active_replay.claim_id == public_claim.claim_id);
  REQUIRE(private_reuse_draws == 0);

  std::size_t cooldown_draws{};
  const auto cooldown = fixture.repository->start_recovery(
      {.invocation = fixture.call("late-cooldown", 601),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++cooldown_draws;
             return TarotTrialDraw{.reward = 8, .prompt_variant = 0};
           },
       .claim_id = uuid(4'070),
       .draw_id = uuid(4'071),
       .started_event_id = uuid(4'072),
       .expired_event_id = uuid(4'073),
       .token_ids = {uuid(4'074), uuid(4'075), uuid(4'076), uuid(4'077)}});
  REQUIRE(cooldown.status == TarotRecoveryStatus::cooldown);
  REQUIRE(cooldown_draws == 0);
  const auto cooldown_replay = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("late-cooldown", 520 + 24LL * 60 * 60 * 1'000),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [&] {
             ++cooldown_draws;
             return TarotTrialDraw{.reward = 9, .prompt_variant = 2};
           },
       .claim_id = uuid(4'080),
       .draw_id = uuid(4'081),
       .started_event_id = uuid(4'082),
       .expired_event_id = uuid(4'083),
       .token_ids = {uuid(4'084), uuid(4'085), uuid(4'086), uuid(4'087)}});
  REQUIRE(cooldown_replay.status == TarotRecoveryStatus::cooldown);
  REQUIRE(cooldown_draws == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_recovery_claim") == 1);
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("Tarot claims reject mismatched terminal event types",
          "[tarot][recovery][invariant][sql]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("terminal-event-lower", 300),
       .amount = -100,
       .reason = "terminal event validation setup",
       .transaction_id = uuid(370),
       .event_id = uuid(371),
       .system_posting_id = uuid(372),
       .human_posting_id = uuid(373)}));
  const auto pending = fixture.repository->start_recovery(
      {.invocation = fixture.call("terminal-event-start", 400),
       .kind = TarotRecoveryKind::grace,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 10,
       .grace_target = 25,
       .cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_draw = {},
       .claim_id = uuid(374),
       .draw_id = std::nullopt,
       .started_event_id = uuid(375),
       .expired_event_id = uuid(376),
       .token_ids = {uuid(377)}});
  REQUIRE(pending.status == TarotRecoveryStatus::pending);

  sanguinius::persistence::Transaction rollback{
      fixture.context->connection(),
      sanguinius::persistence::TransactionMode::immediate};
  fixture.context->connection().execute(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000000378',"
      "'tarot.trial_abandoned.v1','tarot_recovery_claim',"
      "'00000000-0000-4000-8000-000000000374','30','10','20',500,500,"
      "'test','tarot:mismatched-terminal-event','{}')");
  REQUIRE_THROWS(fixture.context->connection().execute(
      "UPDATE tarot_recovery_claim SET state='abandoned',"
      "event_id='00000000-0000-4000-8000-000000000378',"
      "completion_idempotency_key='tarot:mismatched-terminal-completion',"
      "completed_at_ms=500 WHERE "
      "claim_id='00000000-0000-4000-8000-000000000374'"));

  fixture.context->connection().execute(
      "INSERT INTO event_journal(event_id,event_type,aggregate_type,"
      "aggregate_id,actor_user_id,guild_id,channel_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,idempotency_key,payload_json) VALUES("
      "'00000000-0000-4000-8000-000000000379',"
      "'tarot.recovery_eligibility_lost.v1','tarot_recovery_claim',"
      "'00000000-0000-4000-8000-000000000374','30','10','20',500,500,"
      "'test','tarot:valid-terminal-event','{}')");
  fixture.context->connection().execute(
      "UPDATE tarot_recovery_claim SET state='abandoned',"
      "event_id='00000000-0000-4000-8000-000000000379',"
      "completion_idempotency_key='tarot:valid-terminal-completion',"
      "completed_at_ms=500 WHERE "
      "claim_id='00000000-0000-4000-8000-000000000374'");
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token WHERE "
                 "entity_type='tarot_recovery_claim' AND "
                 "entity_id='00000000-0000-4000-8000-000000000374' AND "
                 "state='active'") == 0);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM interaction_token WHERE "
                 "entity_type='tarot_recovery_claim' AND "
                 "entity_id='00000000-0000-4000-8000-000000000374' AND "
                 "state='cancelled'") == 1);
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE(
    "Recovery rejects wrong scope, expiry, abandonment and changed eligibility",
    "[tarot][recovery][authorization][expiry]") {
  TarotFixture fixture;
  static_cast<void>(fixture.provision(10));
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("lower-for-trial", 300),
       .amount = -60,
       .reason = "eligibility setup",
       .transaction_id = uuid(30),
       .event_id = uuid(31),
       .system_posting_id = uuid(32),
       .human_posting_id = uuid(33)}));
  const auto pending = fixture.repository->start_recovery(
      {.invocation = fixture.call("changed-eligibility-start", 400),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 8, .prompt_variant = 1}; },
       .claim_id = uuid(40),
       .draw_id = uuid(41),
       .started_event_id = uuid(42),
       .expired_event_id = uuid(43),
       .token_ids = {uuid(44), uuid(45), uuid(46), uuid(47)}});
  REQUIRE(pending.status == TarotRecoveryStatus::pending);

  const auto wrong_user = fixture.repository->complete_recovery(
      {.invocation = fixture.call("wrong-user", 450, 31),
       .token_id = uuid(44),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(50),
       .event_id = uuid(51),
       .mint_posting_id = uuid(52),
       .human_posting_id = uuid(53),
       .outbox_id = uuid(54),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(54))});
  REQUIRE(wrong_user.status == TarotRecoveryStatus::wrong_user);
  auto wrong_scope_call = fixture.call("wrong-scope", 451);
  wrong_scope_call.channel_id = 21;
  const auto wrong_scope = fixture.repository->complete_recovery(
      {.invocation = wrong_scope_call,
       .token_id = uuid(44),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(55),
       .event_id = uuid(56),
       .mint_posting_id = uuid(57),
       .human_posting_id = uuid(58),
       .outbox_id = uuid(59),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(59))});
  REQUIRE(wrong_scope.status == TarotRecoveryStatus::wrong_scope);
  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("lose-eligibility", 460),
       .amount = 10,
       .reason = "eligibility changed",
       .transaction_id = uuid(60),
       .event_id = uuid(61),
       .system_posting_id = uuid(62),
       .human_posting_id = uuid(63)}));
  const auto lost = fixture.repository->complete_recovery(
      {.invocation = fixture.call("lost-click", 470),
       .token_id = uuid(44),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(64),
       .event_id = uuid(65),
       .mint_posting_id = uuid(66),
       .human_posting_id = uuid(67),
       .outbox_id = uuid(68),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(68))});
  REQUIRE(lost.status == TarotRecoveryStatus::lost_eligibility);
  REQUIRE(lost.balance == 50);
  REQUIRE(lost.committed_event_types ==
          std::vector<std::string>{"tarot.recovery_eligibility_lost.v1"});
  const auto lost_replay = fixture.repository->complete_recovery(
      {.invocation = fixture.call("lost-click-replay", 471),
       .token_id = uuid(45),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(64),
       .event_id = uuid(65),
       .mint_posting_id = uuid(66),
       .human_posting_id = uuid(67),
       .outbox_id = uuid(68),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(68))});
  REQUIRE(lost_replay.status == TarotRecoveryStatus::lost_eligibility);
  REQUIRE(lost_replay.committed_event_types.empty());

  static_cast<void>(fixture.repository->adjust(
      {.invocation = fixture.call("eligible-again", 480),
       .amount = -1,
       .reason = "abandon setup",
       .transaction_id = uuid(70),
       .event_id = uuid(71),
       .system_posting_id = uuid(72),
       .human_posting_id = uuid(73)}));
  const auto abandonable = fixture.repository->start_recovery(
      {.invocation = fixture.call("abandon-start", 500),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 9, .prompt_variant = 0}; },
       .claim_id = uuid(80),
       .draw_id = uuid(81),
       .started_event_id = uuid(82),
       .expired_event_id = uuid(83),
       .token_ids = {uuid(84), uuid(85), uuid(86), uuid(87)}});
  REQUIRE(abandonable.status == TarotRecoveryStatus::pending);
  const auto abandoned = fixture.repository->complete_recovery(
      {.invocation = fixture.call("abandon-click", 510),
       .token_id = uuid(87),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(88),
       .event_id = uuid(89),
       .mint_posting_id = uuid(90),
       .human_posting_id = uuid(91),
       .outbox_id = uuid(92),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(92))});
  REQUIRE(abandoned.status == TarotRecoveryStatus::abandoned);
  REQUIRE(abandoned.committed_event_types ==
          std::vector<std::string>{"tarot.trial_abandoned.v1"});

  const auto expiring = fixture.repository->start_recovery(
      {.invocation = fixture.call("expiry-start", 520),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 10, .prompt_variant = 2}; },
       .claim_id = uuid(100),
       .draw_id = uuid(101),
       .started_event_id = uuid(102),
       .expired_event_id = uuid(103),
       .token_ids = {uuid(104), uuid(105), uuid(106), uuid(107)}});
  REQUIRE(expiring.status == TarotRecoveryStatus::pending);
  const auto expired = fixture.repository->complete_recovery(
      {.invocation = fixture.call(
           "expiry-click", 520 + sanguinius::tarot_interaction_lifetime_ms),
       .token_id = uuid(104),
       .grace_cooldown_ms = 72LL * 60 * 60 * 1'000,
       .trial_cooldown_ms = 24LL * 60 * 60 * 1'000,
       .transaction_id = uuid(108),
       .event_id = uuid(109),
       .mint_posting_id = uuid(110),
       .human_posting_id = uuid(111),
       .outbox_id = uuid(112),
       .provider_nonce = sanguinius::discord_nonce_from_uuid(uuid(112))});
  REQUIRE(expired.status == TarotRecoveryStatus::expired);
  REQUIRE(expired.balance == 49);
  REQUIRE(expired.committed_event_types ==
          std::vector<std::string>{"tarot.recovery_expired.v1"});

  const auto stale = fixture.repository->start_recovery(
      {.invocation = fixture.call("stale-start", 2'000),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 11, .prompt_variant = 1}; },
       .claim_id = uuid(120),
       .draw_id = uuid(121),
       .started_event_id = uuid(122),
       .expired_event_id = uuid(123),
       .token_ids = {uuid(124), uuid(125), uuid(126), uuid(127)}});
  REQUIRE(stale.status == TarotRecoveryStatus::pending);
  const auto replacement = fixture.repository->start_recovery(
      {.invocation =
           fixture.call("replacement-start",
                        2'000 + sanguinius::tarot_interaction_lifetime_ms),
       .kind = TarotRecoveryKind::trial,
       .visibility = TarotVisibility::private_result,
       .is_test = true,
       .threshold = 50,
       .grace_target = std::nullopt,
       .cooldown_ms = 24LL * 60 * 60 * 1'000,
       .trial_draw =
           [] { return TarotTrialDraw{.reward = 12, .prompt_variant = 2}; },
       .claim_id = uuid(130),
       .draw_id = uuid(131),
       .started_event_id = uuid(132),
       .expired_event_id = uuid(133),
       .token_ids = {uuid(134), uuid(135), uuid(136), uuid(137)}});
  REQUIRE(replacement.status == TarotRecoveryStatus::pending);
  REQUIRE(replacement.committed_event_types ==
          std::vector<std::string>{"tarot.recovery_expired.v1",
                                   "tarot.trial_started.v1"});
  REQUIRE(fixture.repository->check_invariants().valid);
}

TEST_CASE("competing Tarot account creation and restart grant exactly once",
          "[tarot][concurrency][restart][duplicate]") {
  TarotFixture fixture;
  auto second_context = std::make_shared<SqliteRepositoryContext>(
      Database::open_runtime(fixture.temporary.path()));
  SqliteTarotRepository second{second_context};
  std::barrier start{2};
  sanguinius::TarotAccountProvisionResult first_result;
  sanguinius::TarotAccountProvisionResult second_result;
  std::thread first{[&] {
    start.arrive_and_wait();
    first_result = fixture.provision(300, 31);
  }};
  std::thread other{[&] {
    start.arrive_and_wait();
    second_result = second.ensure_account(
        {.invocation = fixture.call("competing-second", 700, 31),
         .starting_fate = 100,
         .account_id = uuid(310),
         .transaction_id = uuid(311),
         .event_id = uuid(312),
         .mint_posting_id = uuid(313),
         .human_posting_id = uuid(314)});
  }};
  first.join();
  other.join();
  REQUIRE(first_result.created != second_result.created);
  REQUIRE(first_result.account_id == second_result.account_id);
  REQUIRE(first_result.balance == 100);
  REQUIRE(second_result.balance == 100);
  REQUIRE(scalar(*fixture.context,
                 "SELECT count(*) FROM tarot_transaction WHERE "
                 "transaction_type='STARTING_GRANT'") == 1);
  REQUIRE(second.balance(31) == 100);
  REQUIRE(second.check_invariants().valid);
}
