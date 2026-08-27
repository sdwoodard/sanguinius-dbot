#include "sanguinius/database_cli.hpp"

#include "sanguinius/persistence/backup.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_appearance_repository.hpp"
#include "sanguinius/persistence/sqlite_relationship_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"
#include "sanguinius/persistence/sqlite_tarot_repository.hpp"
#include "sanguinius/persistence/sqlite_wager_repository.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

namespace sanguinius {
namespace {

using persistence::Database;
using persistence::DatabaseError;
using persistence::DatabaseErrorCategory;
using persistence::DatabaseMaintenance;
using persistence::MigrationStatus;
using persistence::Migrator;
using persistence::SchemaState;

void print_status(std::ostream &output, const MigrationStatus &status) {
  output << "database=" << persistence::schema_state_name(status.state) << '\n'
         << "current_schema=" << status.current_version << '\n'
         << "target_schema=" << status.target_version << '\n'
         << "pending_migrations=" << status.pending_count << '\n'
         << "sqlite=" << persistence::sqlite_runtime_version() << '\n';
}

[[nodiscard]] bool wal_mode(persistence::SqliteConnection &connection) {
  auto statement = connection.prepare("PRAGMA journal_mode");
  return statement.step() && statement.column_text(0) == "wal" &&
         !statement.step();
}

[[nodiscard]] std::int64_t scalar(persistence::SqliteConnection &connection,
                                  const std::string_view sql) {
  auto query = connection.prepare(sql);
  if (!query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, 0, 0,
                        "Invariant query returned no row."};
  return query.column_int64(0);
}

[[nodiscard]] std::string consumer_state(const std::int64_t lag) {
  return lag == 0 ? "ok" : "lag(" + std::to_string(lag) + ")";
}

} // namespace

bool database_cli_detail::check_core_invariants(
    persistence::SqliteConnection &connection, std::ostream &output) {
  const auto integrity = DatabaseMaintenance::integrity_check(connection);
  const auto durable =
      scalar(connection, "SELECT COUNT(*) FROM scheduled_job WHERE "
                         "(state='claimed')<>(lease_owner IS NOT NULL)");
  const auto outbox =
      scalar(connection, "SELECT COUNT(*) FROM outbox_message WHERE "
                         "state='claimed' AND lease_owner IS NULL");
  const auto outbox_dependencies = scalar(
      connection,
      "SELECT (SELECT COUNT(*) FROM tarot_public_outbox_dependency dependency "
      "JOIN outbox_message predecessor ON predecessor.outbox_id="
      "dependency.predecessor_outbox_id JOIN outbox_message successor ON "
      "successor.outbox_id=dependency.successor_outbox_id WHERE "
      "successor.state='delivered' AND (predecessor.state<>'delivered' OR "
      "successor.delivered_at_ms<predecessor.delivered_at_ms))+"
      "(SELECT COUNT(*) FROM voice_public_outbox_dependency dependency JOIN "
      "outbox_message predecessor ON predecessor.outbox_id="
      "dependency.predecessor_outbox_id JOIN outbox_message successor ON "
      "successor.outbox_id=dependency.successor_outbox_id WHERE "
      "successor.state='delivered' AND (predecessor.state<>'delivered' OR "
      "successor.delivered_at_ms<predecessor.delivered_at_ms))");
  const auto event_receipts = scalar(
      connection,
      "SELECT COUNT(*) FROM tarot_integration_effect_receipt receipt LEFT JOIN "
      "event_journal event ON event.event_id=receipt.source_event_id WHERE "
      "event.event_id IS NULL");
  const auto relationship_consumer_lag = scalar(
      connection,
      "SELECT (SELECT COUNT(*) FROM event_journal event JOIN chronicle_entry "
      "entry ON entry.entry_id=event.aggregate_id JOIN chronicle_participant "
      "participant ON participant.entry_id=event.aggregate_id JOIN "
      "discord_user "
      "user ON user.user_id=participant.user_id JOIN user_preference "
      "preference "
      "ON preference.user_id=participant.user_id WHERE "
      "event.event_type='chronicle.entry_canonized.v1' AND entry.entry_type "
      "NOT "
      "IN ('session_summary','title_award') AND participant.role IN "
      "('source_author','subject') AND user.is_bot=0 AND "
      "preference.chronicle_opt_in=1 AND "
      "COALESCE(json_extract(event.payload_json,'$.test'),0)=0 AND NOT EXISTS "
      "(SELECT 1 FROM relationship_event relationship WHERE "
      "relationship.source_event_id=event.event_id AND "
      "relationship.subject_user_id=participant.user_id))+(SELECT COUNT(*) "
      "FROM event_journal event JOIN chronicle_session_participant participant "
      "ON participant.session_id=event.aggregate_id JOIN discord_user user ON "
      "user.user_id=participant.user_id JOIN user_preference preference ON "
      "preference.user_id=participant.user_id WHERE "
      "event.event_type='chronicle.session_completed.v1' AND user.is_bot=0 AND "
      "preference.chronicle_opt_in=1 AND NOT EXISTS (SELECT 1 FROM "
      "relationship_event relationship WHERE "
      "relationship.source_event_id=event.event_id AND "
      "relationship.subject_user_id=participant.user_id))+(SELECT COUNT(*) "
      "FROM event_journal event JOIN discord_user user ON user.user_id="
      "json_extract(event.payload_json,'$.recipient_user_id') JOIN "
      "user_preference preference ON preference.user_id=user.user_id WHERE "
      "event.event_type='chronicle.title_awarded.v1' AND user.is_bot=0 AND "
      "preference.chronicle_opt_in=1 AND NOT EXISTS (SELECT 1 FROM "
      "relationship_event relationship WHERE "
      "relationship.source_event_id=event.event_id AND "
      "relationship.subject_user_id=user.user_id))");
  const auto tarot_house_consumer_lag = scalar(
      connection,
      "SELECT COUNT(*) FROM tarot_house_wager wager JOIN tarot_event_order "
      "accepted_order ON accepted_order.event_id=wager.accepted_event_id WHERE "
      "wager.state='accepted_funded' AND wager.authority IN "
      "('draw','public_draw') AND EXISTS (SELECT 1 FROM tarot_card_draw draw "
      "JOIN discord_user drawer ON drawer.user_id=draw.user_id JOIN "
      "tarot_event_order draw_order ON draw_order.event_id=draw.event_id WHERE "
      "draw.is_test=wager.is_test AND draw.guild_id=wager.guild_id AND "
      "draw.channel_id=wager.channel_id AND "
      "draw_order.sequence_id>accepted_order.sequence_id AND "
      "draw.drawn_at_ms<=wager.outcome_due_at_ms AND ((wager.authority='draw' "
      "AND draw.user_id=wager.user_id) OR (wager.authority='public_draw' AND "
      "draw.user_id<>wager.user_id AND drawer.is_bot=0 AND "
      "draw.visibility='public')))");
  const auto tarot_integration_consumer_lag = scalar(
      connection,
      "SELECT COUNT(*) FROM tarot_integration_observation WHERE state IN "
      "('pending','processing','failed') OR (state='suppressed' AND "
      "is_test=0 AND COALESCE(last_error,'')<>'integration_disabled')");
  const auto appearance_consumer_lag = scalar(
      connection,
      "SELECT (SELECT COUNT(*) FROM appearance_event_observation WHERE "
      "processed_at_ms IS NULL)+(SELECT COUNT(*) FROM appearance_candidate "
      "candidate WHERE NOT EXISTS(SELECT 1 FROM appearance_decision decision "
      "WHERE decision.candidate_id=candidate.candidate_id))");
  const auto vox_narration_consumer_lag = scalar(
      connection,
      "SELECT (SELECT abs(1-COUNT(*)) FROM voice_narration_cursor)+(SELECT "
      "COUNT(*) FROM event_journal WHERE rowid>COALESCE((SELECT "
      "CAST(last_event_rowid AS INTEGER) FROM voice_narration_cursor WHERE "
      "singleton=1),-1))+(SELECT COUNT(*) FROM voice_narration_cursor WHERE "
      "CAST(last_event_rowid AS INTEGER)>COALESCE((SELECT MAX(rowid) FROM "
      "event_journal),0))");
  const auto chronicle_fts = scalar(
      connection,
      "SELECT (SELECT COUNT(*) FROM chronicle_entry entry LEFT JOIN "
      "chronicle_entry_fts fts ON fts.rowid=entry.rowid WHERE fts.rowid IS "
      "NULL OR fts.title<>entry.title OR fts.body<>entry.body OR "
      "fts.source_text<>entry.source_text)+(SELECT COUNT(*) FROM "
      "chronicle_entry_fts fts LEFT JOIN chronicle_entry entry ON "
      "entry.rowid=fts.rowid WHERE entry.rowid IS NULL)");
  const auto appearance_links =
      scalar(connection, "SELECT COUNT(*) FROM appearance_budget_reservation "
                         "delivery LEFT JOIN appearance_decision decision ON "
                         "decision.decision_id=delivery.decision_id WHERE "
                         "decision.decision_id IS NULL");
  const auto vox =
      scalar(connection, "SELECT COUNT(*) FROM voice_session WHERE state NOT "
                         "IN ('inactive','failed')") > 1;
  const auto active_voice_windows =
      scalar(connection, "SELECT COUNT(*) FROM voice_listening_window WHERE "
                         "state IN ('proposed','arming_transport',"
                         "'arming_indicator','active','transcribing')");
  const auto voice_audit = scalar(
      connection,
      "SELECT (SELECT abs(1-COUNT(*)) FROM main.voice_input_control)+"
      "(SELECT COUNT(*) FROM main.voice_input_control control WHERE "
      "(NOT EXISTS(SELECT 1 FROM main.voice_input_kill_change) AND "
      "(control.disabled<>0 OR control.updated_at_ms<>0)) OR "
      "(EXISTS(SELECT 1 FROM main.voice_input_kill_change) AND "
      "NOT EXISTS(SELECT 1 FROM main.voice_input_kill_change change WHERE "
      "change.rowid=(SELECT MAX(latest.rowid) FROM "
      "main.voice_input_kill_change latest) AND change.occurred_at_ms="
      "control.updated_at_ms AND change.disabled=control.disabled)))+"
      "(SELECT COUNT(*) FROM main.voice_listening_window window WHERE "
      "window.state_version<>(SELECT COUNT(*) FROM "
      "main.voice_listening_transition transition WHERE transition.window_id="
      "window.window_id) OR NOT EXISTS(SELECT 1 FROM "
      "main.voice_listening_transition transition WHERE transition.window_id="
      "window.window_id AND transition.to_version=window.state_version AND "
      "transition.to_state=window.state) OR EXISTS(SELECT 1 FROM "
      "main.voice_listening_transition transition WHERE transition.window_id="
      "window.window_id AND ((transition.to_version=1 AND "
      "(transition.from_version<>0 OR transition.from_state<>'none' OR "
      "transition.to_state<>'proposed')) OR "
      "(transition.to_version>1 AND NOT EXISTS(SELECT 1 FROM "
      "main.voice_listening_transition prior WHERE prior.window_id="
      "transition.window_id AND prior.to_version=transition.from_version AND "
      "prior.to_state=transition.from_state)) OR (transition.to_version>1 AND "
      "NOT ((transition.from_state IN ('proposed','arming_transport',"
      "'arming_indicator','active','transcribing') AND transition.to_state IN "
      "('failed','stopped','abandoned')) OR (transition.from_state='proposed' "
      "AND transition.to_state='arming_transport') OR "
      "(transition.from_state='arming_transport' AND "
      "transition.to_state='arming_indicator') OR "
      "(transition.from_state='arming_indicator' AND "
      "transition.to_state='active') OR (transition.from_state='active' AND "
      "transition.to_state='transcribing') OR "
      "(transition.from_state='transcribing' AND "
      "transition.to_state='completed'))))))+"
      "(SELECT COUNT(*) FROM main.voice_listening_window window WHERE "
      "window.state IN ('proposed','arming_transport','arming_indicator',"
      "'active','transcribing') AND ((SELECT control.disabled FROM "
      "main.voice_input_control control WHERE control.singleton=1)<>0 OR "
      "COALESCE((SELECT consent.attested FROM "
      "main.voice_input_consent_attestation consent ORDER BY consent.rowid "
      "DESC "
      "LIMIT 1),0)<>1))");
  const auto voice = active_voice_windows > 1 || voice_audit != 0;
  const auto speech_narration = scalar(
      connection,
      "SELECT (SELECT COUNT(*) FROM speech_item speech WHERE "
      "speech.state_version>1 AND NOT EXISTS(SELECT 1 FROM "
      "speech_item_transition transition WHERE transition.speech_id="
      "speech.speech_id AND transition.to_version=speech.state_version))+"
      "(SELECT COUNT(*) FROM voice_narration_intent intent WHERE NOT EXISTS("
      "SELECT 1 FROM voice_narration_transition transition WHERE "
      "transition.intent_id=intent.intent_id AND transition.to_version="
      "intent.state_version))+"
      "(SELECT COUNT(*) FROM voice_narration_intent intent JOIN speech_item "
      "speech ON speech.speech_id=intent.speech_id WHERE "
      "speech.source_event_id<>intent.source_event_id OR "
      "speech.voice_session_id<>intent.session_id OR "
      "speech.source_kind<>'vox_feature_narration' OR "
      "speech.narration_rank<>intent.narration_rank)");
  const auto ai = scalar(
      connection,
      "SELECT COUNT(*) FROM ai_generation_attempt WHERE reserved_micro_usd<>("
      "(reserved_input_tokens*input_rate_micro_usd_per_million+999999)/"
      "1000000+(reserved_output_tokens*output_rate_micro_usd_per_million+"
      "999999)/1000000) OR (provider_sent=0 AND "
      "state IN ('submitted','succeeded','failed','unknown')) OR "
      "(state='succeeded' AND (actual_input_tokens IS NULL OR "
      "actual_output_tokens IS NULL OR actual_micro_usd IS NULL OR "
      "actual_input_tokens>reserved_input_tokens OR "
      "actual_output_tokens>reserved_output_tokens OR actual_micro_usd<>("
      "(actual_input_tokens*input_rate_micro_usd_per_million+999999)/1000000+"
      "(actual_output_tokens*output_rate_micro_usd_per_million+999999)/"
      "1000000)))");
  const auto provider_circuits = scalar(
      connection,
      "SELECT (SELECT abs(3-COUNT(*)) FROM provider_circuit_state)+"
      "(SELECT COUNT(*) FROM provider_circuit_state state WHERE "
      "state.revision<>1+(SELECT COUNT(*) FROM provider_circuit_transition "
      "transition WHERE transition.provider=state.provider) OR "
      "(state.revision=1 AND state.state<>'closed') OR "
      "(state.consecutive_failures=0)<>(state.first_failure_at_ms IS NULL) OR "
      "(state.revision>1 AND NOT EXISTS(SELECT 1 FROM "
      "provider_circuit_transition latest WHERE latest.provider=state.provider "
      "AND latest.to_revision=state.revision AND "
      "latest.to_state=state.state)))+"
      "(SELECT COUNT(*) FROM provider_circuit_transition transition WHERE NOT "
      "((transition.from_revision=1 AND transition.from_state='closed') OR "
      "EXISTS(SELECT 1 FROM provider_circuit_transition previous WHERE "
      "previous.provider=transition.provider AND "
      "previous.to_revision=transition.from_revision AND "
      "previous.to_state=transition.from_state AND "
      "previous.occurred_at_ms<=transition.occurred_at_ms)))");
  const auto controls = scalar(
      connection,
      "SELECT (SELECT abs(3-COUNT(*)) FROM runtime_feature_control)+"
      "(SELECT COUNT(*) FROM runtime_feature_control state WHERE "
      "state.revision<>1+(SELECT COUNT(*) FROM "
      "runtime_feature_control_transition transition WHERE "
      "transition.feature=state.feature) OR (state.revision=1 AND "
      "(state.disabled<>0 OR state.actor_user_id IS NOT NULL OR "
      "state.changed_at_ms<>0)) OR (state.revision>1 AND NOT EXISTS(SELECT 1 "
      "FROM runtime_feature_control_transition latest WHERE "
      "latest.feature=state.feature AND latest.to_revision=state.revision AND "
      "latest.to_disabled=state.disabled AND "
      "latest.actor_user_id=state.actor_user_id AND "
      "latest.occurred_at_ms=state.changed_at_ms)))+"
      "(SELECT COUNT(*) FROM runtime_feature_control_transition transition "
      "WHERE NOT ((transition.from_revision=1 AND "
      "transition.from_disabled=0) OR EXISTS(SELECT 1 FROM "
      "runtime_feature_control_transition previous WHERE "
      "previous.feature=transition.feature AND "
      "previous.to_revision=transition.from_revision AND "
      "previous.to_disabled=transition.from_disabled AND "
      "previous.occurred_at_ms<=transition.occurred_at_ms)))");
  const auto interaction_snapshots =
      scalar(connection,
             "SELECT COUNT(*) FROM interaction_list_snapshot snapshot WHERE "
             "snapshot.item_count<>(SELECT COUNT(*) FROM "
             "interaction_list_snapshot_item item WHERE "
             "item.snapshot_id=snapshot.snapshot_id)");
  output << "integrity=" << (integrity.integrity_ok ? "ok" : "failed") << '\n'
         << "foreign_keys=" << (integrity.foreign_keys_ok ? "ok" : "failed")
         << '\n'
         << "durable_work=" << (durable == 0 ? "ok" : "failed") << '\n'
         << "outbox=" << (outbox == 0 ? "ok" : "failed") << '\n'
         << "outbox_dependencies="
         << (outbox_dependencies == 0 ? "ok" : "failed") << '\n'
         << "event_receipts=" << (event_receipts == 0 ? "ok" : "failed") << '\n'
         << "consumer_relationships="
         << consumer_state(relationship_consumer_lag) << '\n'
         << "consumer_tarot_house=" << consumer_state(tarot_house_consumer_lag)
         << '\n'
         << "consumer_tarot_integration="
         << consumer_state(tarot_integration_consumer_lag) << '\n'
         << "consumer_appearances=" << consumer_state(appearance_consumer_lag)
         << '\n'
         << "consumer_vox_narration="
         << consumer_state(vox_narration_consumer_lag) << '\n'
         << "chronicle_fts=" << (chronicle_fts == 0 ? "ok" : "drift") << '\n'
         << "appearance_budget_links="
         << (appearance_links == 0 ? "ok" : "failed") << '\n'
         << "vox=" << (vox == 0 ? "ok" : "failed") << '\n'
         << "speech_narration=" << (speech_narration == 0 ? "ok" : "failed")
         << '\n'
         << "voice_input_privacy=" << (voice == 0 ? "ok" : "failed") << '\n'
         << "ai_budgets=" << (ai == 0 ? "ok" : "failed") << '\n'
         << "provider_circuits=" << (provider_circuits == 0 ? "ok" : "failed")
         << '\n'
         << "runtime_controls=" << (controls == 0 ? "ok" : "failed") << '\n'
         << "interaction_snapshots="
         << (interaction_snapshots == 0 ? "ok" : "failed") << '\n';
  return integrity.ok() && durable == 0 && outbox == 0 &&
         outbox_dependencies == 0 && event_receipts == 0 &&
         relationship_consumer_lag == 0 && tarot_house_consumer_lag == 0 &&
         tarot_integration_consumer_lag == 0 && appearance_consumer_lag == 0 &&
         vox_narration_consumer_lag == 0 && chronicle_fts == 0 &&
         appearance_links == 0 && vox == 0 && speech_narration == 0 &&
         voice == 0 && ai == 0 && provider_circuits == 0 && controls == 0 &&
         interaction_snapshots == 0;
}

int run_database_command(const DatabaseCommand &command,
                         const std::filesystem::path &database,
                         const BuildInfo &build, const Clock &clock,
                         std::ostream &output, std::ostream &errors) {
  try {
    persistence::verify_sqlite_runtime();
    const Migrator migrator{persistence::production_migrations(), build, clock};
    if (command.type == DatabaseCommandType::status) {
      std::error_code status_error;
      const auto file_status = std::filesystem::status(database, status_error);
      const bool absent =
          status_error == std::errc::no_such_file_or_directory ||
          (!status_error && !std::filesystem::exists(file_status));
      if (status_error && !absent) {
        throw DatabaseError{DatabaseErrorCategory::io, 0, 0,
                            "Database status inspection failed (io)."};
      }
      if (absent) {
        const auto status = migrator.version_zero_status();
        output << "database=absent\n"
               << "current_schema=" << status.current_version << '\n'
               << "target_schema=" << status.target_version << '\n'
               << "pending_migrations=" << status.pending_count << '\n'
               << "sqlite=" << persistence::sqlite_runtime_version() << '\n';
        return 0;
      }
      if (!std::filesystem::is_regular_file(file_status)) {
        throw DatabaseError{DatabaseErrorCategory::io, 0, 0,
                            "Database status inspection failed (io)."};
      }
    }

    switch (command.type) {
    case DatabaseCommandType::status: {
      auto opened = Database::open_inspection(database);
      auto status = migrator.inspect(opened.connection());
      if (status.state == SchemaState::current &&
          !wal_mode(opened.connection())) {
        status.state = SchemaState::incompatible;
      }
      print_status(output, status);
      return status.state == SchemaState::incompatible ? 1 : 0;
    }
    case DatabaseCommandType::check: {
      auto opened = Database::open_inspection(database);
      if (!wal_mode(opened.connection())) {
        throw DatabaseError{DatabaseErrorCategory::incompatible, 0, 0,
                            "Database is not in required WAL mode."};
      }
      migrator.require_current(opened.connection());
      print_status(output, migrator.inspect(opened.connection()));
      return 0;
    }
    case DatabaseCommandType::migrate: {
      auto opened = Database::open_migration(database);
      const auto status = migrator.apply(opened.connection());
      print_status(output, status);
      return 0;
    }
    case DatabaseCommandType::integrity: {
      auto opened = Database::open_inspection(database);
      const auto result =
          DatabaseMaintenance::integrity_check(opened.connection());
      output << "integrity=" << (result.integrity_ok ? "ok" : "failed") << '\n'
             << "foreign_keys=" << (result.foreign_keys_ok ? "ok" : "failed")
             << '\n';
      return result.ok() ? 0 : 1;
    }
    case DatabaseCommandType::backup: {
      if (!command.destination.has_value() || command.destination->empty()) {
        errors << "Database backup destination is required.\n";
        return 2;
      }
      auto opened = Database::open_inspection(database);
      const auto result = DatabaseMaintenance::backup(
          opened.connection(), database, *command.destination, migrator);
      output << "backup=verified\n"
             << "schema_state="
             << persistence::schema_state_name(result.migration.state) << '\n'
             << "schema=" << result.migration.current_version << '\n'
             << "size_bytes=" << result.size_bytes << '\n';
      return 0;
    }
    case DatabaseCommandType::relationships_check: {
      auto opened = Database::open_inspection(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      const auto result = relationships.check_projection();
      output << "relationships=" << (result.valid ? "ok" : "drift") << '\n'
             << "events=" << result.event_count << '\n'
             << "projections=" << result.projection_count << '\n'
             << "mismatches=" << result.mismatch_count << '\n';
      return result.valid ? 0 : 1;
    }
    case DatabaseCommandType::relationships_rebuild: {
      auto opened = Database::open_migration(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      const auto result = relationships.rebuild_projection();
      output << "relationships=rebuilt\n"
             << "events=" << result.event_count << '\n'
             << "projections=" << result.projection_count << '\n'
             << "mismatches=" << result.mismatch_count << '\n';
      return 0;
    }
    case DatabaseCommandType::tarot_check: {
      auto opened = Database::open_inspection(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteTarotRepository tarot{context};
      persistence::SqliteWagerRepository wagers{context};
      persistence::SqliteTarotHouseRepository house{context};
      const auto result = tarot.check_invariants();
      const auto wager_result = wagers.check_invariants();
      const auto house_result = house.economy();
      const auto projection_result = house.check_player_projection();
      output << "tarot=" << (result.valid ? "ok" : "failed") << '\n'
             << "accounts=" << result.account_count << '\n'
             << "transactions=" << result.committed_transaction_count << '\n'
             << "postings=" << result.posting_count << '\n'
             << "prepared=" << result.prepared_transaction_count << '\n'
             << "unbalanced=" << result.unbalanced_transaction_count << '\n'
             << "negative_history=" << result.negative_history_count << '\n'
             << "overflow=" << result.overflow_count << '\n'
             << "illegal_reversals=" << result.illegal_reversal_count << '\n'
             << "claim_mismatches=" << result.claim_mismatch_count << '\n'
             << "orphaned_links=" << result.orphaned_link_count << '\n'
             << "wagers=" << (wager_result.valid ? "ok" : "failed") << '\n'
             << "wager_open_funded="
             << wager_result.open_funded_obligation_count << '\n'
             << "wager_obligation_fate="
             << wager_result.open_funded_obligation_amount << '\n'
             << "wager_escrow_fate=" << wager_result.escrow_balance << '\n'
             << "wager_disputes=" << wager_result.disputed_count << '\n'
             << "wager_malformed_transfers="
             << wager_result.malformed_transfer_count << '\n'
             << "wager_invalid_deadline_action_links="
             << wager_result.invalid_deadline_action_link_count << '\n'
             << "wager_orphaned_links=" << wager_result.orphaned_link_count
             << '\n'
             << "house=" << (house_result.valid ? "ok" : "failed") << '\n'
             << "house_open_funded=" << house_result.open_house_wagers << '\n'
             << "house_exposure_fate=" << house_result.non_test_exposure << '\n'
             << "house_test_exposure_fate=" << house_result.test_exposure
             << '\n'
             << "house_obligation_fate=" << house_result.expected_house_escrow
             << '\n'
             << "combined_escrow_fate=" << house_result.escrow_balance << '\n'
             << "house_malformed_transfers="
             << house_result.malformed_transfer_count << '\n'
             << "house_malformed_offer_deadlines="
             << house_result.malformed_offer_deadline_count << '\n'
             << "tarot_player_projection="
             << (projection_result.valid ? "ok" : "drift") << '\n'
             << "tarot_player_events=" << projection_result.event_count << '\n'
             << "tarot_player_projections="
             << projection_result.projection_count << '\n'
             << "tarot_player_mismatches=" << projection_result.mismatch_count
             << '\n';
      return result.valid && wager_result.valid && house_result.valid &&
                     projection_result.valid
                 ? 0
                 : 1;
    }
    case DatabaseCommandType::tarot_rebuild: {
      auto opened = Database::open_migration(database);
      migrator.require_current(opened.connection());
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteTarotHouseRepository house{context};
      persistence::SqliteTarotIntegrationRepository integration{context};
      const auto result = house.rebuild_player_projection();
      output << "tarot_player_projection=rebuilt\n"
             << "tarot_player_events=" << result.event_count << '\n'
             << "tarot_player_projections=" << result.projection_count << '\n'
             << "tarot_player_mismatches=" << result.mismatch_count << '\n';
      return result.valid ? 0 : 1;
    }
    case DatabaseCommandType::invariants_check: {
      auto opened = Database::open_inspection(database);
      migrator.require_current(opened.connection());
      const auto core_valid = database_cli_detail::check_core_invariants(
          opened.connection(), output);
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      persistence::SqliteAppearanceRepository appearances{context};
      persistence::SqliteTarotRepository tarot{context};
      persistence::SqliteWagerRepository wagers{context};
      persistence::SqliteTarotHouseRepository house{context};
      persistence::SqliteTarotIntegrationRepository integration{context};
      const auto relationship = relationships.check_projection();
      const auto ledger = tarot.check_invariants();
      const auto wager = wagers.check_invariants();
      const auto economy = house.economy();
      const auto player = house.check_player_projection();
      const auto integration_projection = integration.check_projection();
      const auto appearance_violations =
          appearances.public_outbox_violation_count();
      output << "relationships=" << (relationship.valid ? "ok" : "drift")
             << '\n'
             << "appearance_provenance="
             << (appearance_violations == 0 ? "ok" : "failed") << '\n'
             << "tarot_ledger=" << (ledger.valid ? "ok" : "failed") << '\n'
             << "tarot_wagers=" << (wager.valid ? "ok" : "failed") << '\n'
             << "tarot_house=" << (economy.valid ? "ok" : "failed") << '\n'
             << "tarot_player_projection=" << (player.valid ? "ok" : "drift")
             << '\n'
             << "tarot_integration_titles="
             << (integration_projection.valid ? "ok" : "drift") << '\n';
      return core_valid && appearance_violations == 0 && relationship.valid &&
                     ledger.valid && wager.valid && economy.valid &&
                     player.valid && integration_projection.valid
                 ? 0
                 : 1;
    }
    case DatabaseCommandType::invariants_rebuild: {
      auto opened = Database::open_migration(database);
      migrator.require_current(opened.connection());
      const auto current =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              clock.now().time_since_epoch())
              .count();
      auto active = opened.connection().prepare(
          "SELECT COUNT(*) FROM application_instance WHERE stopped_at_ms IS "
          "NULL AND COALESCE(heartbeat_at_ms,started_at_ms)>=?");
      active.bind(1, std::max<std::int64_t>(0, current - 2 * 60 * 1'000));
      if (!active.step() || active.column_int64(0) != 0) {
        errors << "Projection rebuild refused: an application instance was "
                  "recently active.\n";
        return 1;
      }
      auto context = std::make_shared<persistence::SqliteRepositoryContext>(
          std::move(opened));
      persistence::SqliteRelationshipRepository relationships{context};
      persistence::SqliteAppearanceRepository appearances{context};
      persistence::SqliteTarotHouseRepository house{context};
      persistence::SqliteTarotIntegrationRepository integration{context};
      persistence::SqliteTarotRepository tarot{context};
      persistence::SqliteWagerRepository wagers{context};
      persistence::Transaction transaction{
          context->connection(), persistence::TransactionMode::immediate};
      context->connection().execute(
          "INSERT INTO chronicle_entry_fts(chronicle_entry_fts) "
          "VALUES('rebuild')");
      const auto relationship = relationships.rebuild_projection_uncommitted();
      const auto player = house.rebuild_player_projection_uncommitted();
      const auto integration_projection =
          integration.rebuild_projection_uncommitted();
      const auto core_valid = database_cli_detail::check_core_invariants(
          context->connection(), output);
      const auto relationship_check = relationships.check_projection();
      const auto ledger = tarot.check_invariants();
      const auto wager = wagers.check_invariants();
      const auto economy = house.economy();
      const auto player_check = house.check_player_projection();
      const auto integration_check = integration.check_projection();
      const auto appearance_violations =
          appearances.public_outbox_violation_count();
      const bool valid =
          core_valid && appearance_violations == 0 && relationship.valid &&
          player.valid && integration_projection.valid &&
          relationship_check.valid && ledger.valid && wager.valid &&
          economy.valid && player_check.valid && integration_check.valid;
      output << "chronicle_fts=rebuilt\n"
             << "appearance_provenance="
             << (appearance_violations == 0 ? "ok" : "failed") << '\n'
             << "relationships=" << (relationship.valid ? "rebuilt" : "failed")
             << '\n'
             << "tarot_player_projection="
             << (player.valid ? "rebuilt" : "failed") << '\n'
             << "tarot_integration_titles="
             << (integration_projection.valid ? "rebuilt" : "failed") << '\n';
      output << "post_rebuild_check=" << (valid ? "ok" : "failed") << '\n';
      if (!valid)
        return 1;
      transaction.commit();
      return 0;
    }
    }
  } catch (const DatabaseError &error) {
    errors << "Database command failed ("
           << persistence::database_error_category_name(error.category())
           << ").\n";
    return 1;
  } catch (const std::exception &) {
    errors << "Database command failed (other).\n";
    return 1;
  }
  errors << "Database command failed (other).\n";
  return 1;
}

} // namespace sanguinius
