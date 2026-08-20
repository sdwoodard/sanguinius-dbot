CREATE TABLE appearance_policy_snapshot (
    policy_version TEXT PRIMARY KEY CHECK (length(policy_version) BETWEEN 1 AND 80),
    schema_version INTEGER NOT NULL CHECK (schema_version = 1),
    canonical_json TEXT NOT NULL CHECK (json_valid(canonical_json) AND length(canonical_json) <= 65536),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TRIGGER appearance_policy_snapshot_no_update
BEFORE UPDATE ON appearance_policy_snapshot BEGIN
    SELECT RAISE(ABORT, 'appearance policy snapshots are immutable');
END;
CREATE TRIGGER appearance_policy_snapshot_no_delete
BEFORE DELETE ON appearance_policy_snapshot BEGIN
    SELECT RAISE(ABORT, 'appearance policy snapshots are immutable');
END;

CREATE TABLE appearance_mode_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    mode TEXT NOT NULL CHECK (mode IN ('off', 'dry_run')),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
) STRICT;

INSERT INTO appearance_mode_state(singleton,mode,updated_at_ms)
VALUES(1,'off',0);

CREATE TABLE appearance_channel_state (
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20 AND channel_id NOT GLOB '*[^0-9]*' AND channel_id <> '0'),
    human_message_count INTEGER NOT NULL DEFAULT 0 CHECK (human_message_count >= 0),
    last_sanguinius_at_ms INTEGER CHECK (last_sanguinius_at_ms IS NULL OR last_sanguinius_at_ms >= 0),
    human_message_count_at_last_sanguinius INTEGER NOT NULL DEFAULT 0
      CHECK (human_message_count_at_last_sanguinius >= 0 AND human_message_count_at_last_sanguinius <= human_message_count),
    last_author_is_sanguinius INTEGER NOT NULL DEFAULT 0 CHECK (last_author_is_sanguinius IN (0,1)),
    PRIMARY KEY(guild_id,channel_id)
) STRICT;

CREATE TABLE appearance_message_seen (
    message_id TEXT PRIMARY KEY CHECK (length(message_id) BETWEEN 1 AND 20 AND message_id NOT GLOB '*[^0-9]*' AND message_id <> '0'),
    first_observed_at_ms INTEGER NOT NULL CHECK (first_observed_at_ms >= 0)
) STRICT;

CREATE TABLE appearance_message_activity (
    message_id TEXT PRIMARY KEY CHECK (length(message_id) BETWEEN 1 AND 20 AND message_id NOT GLOB '*[^0-9]*' AND message_id <> '0'),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20 AND channel_id NOT GLOB '*[^0-9]*' AND channel_id <> '0'),
    policy_version TEXT NOT NULL REFERENCES appearance_policy_snapshot(policy_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    author_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    author_is_bot INTEGER NOT NULL CHECK (author_is_bot IN (0, 1)),
    excerpt TEXT NOT NULL CHECK (length(CAST(excerpt AS BLOB)) <= 500),
    serious_category TEXT CHECK (serious_category IS NULL OR serious_category IN (
      'abuse_conflict','christianity','credentials_security_pii',
      'crisis_self_harm_emergency','death_serious_health',
      'private_medical_employment_legal_financial','invalid_utf8')),
    observed_at_ms INTEGER NOT NULL CHECK (observed_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > observed_at_ms),
    consumed_candidate_id TEXT
        REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    correlation_id TEXT NOT NULL CHECK (length(correlation_id) BETWEEN 1 AND 160)
) STRICT;

CREATE TABLE appearance_event_observation (
    source_event_id TEXT PRIMARY KEY REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_type TEXT NOT NULL CHECK (length(event_type) BETWEEN 1 AND 96),
    aggregate_type TEXT NOT NULL CHECK (length(aggregate_type) BETWEEN 1 AND 64),
    aggregate_id TEXT NOT NULL CHECK (length(aggregate_id) BETWEEN 1 AND 128),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT,
    actor_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    recorded_at_ms INTEGER NOT NULL CHECK (recorded_at_ms >= 0),
    extraction_result TEXT CHECK (extraction_result IS NULL OR extraction_result IN ('candidate_created', 'source_not_enabled', 'mode_off', 'expired')),
    candidate_id TEXT
        REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    processed_at_ms INTEGER
) STRICT;

CREATE TRIGGER appearance_observe_chronicle_event
AFTER INSERT ON event_journal
WHEN NEW.event_type IN ('chronicle.entry_canonized.v1',
                        'chronicle.session_started.v1',
                        'chronicle.session_completed.v1',
                        'chronicle.title_awarded.v1',
                        'chronicle.anniversary_delivered.v1')
BEGIN
    INSERT INTO appearance_event_observation
      (source_event_id,event_type,aggregate_type,aggregate_id,guild_id,channel_id,
       actor_user_id,occurred_at_ms,recorded_at_ms)
    VALUES
      (NEW.event_id,NEW.event_type,NEW.aggregate_type,NEW.aggregate_id,NEW.guild_id,
       NEW.channel_id,NEW.actor_user_id,NEW.occurred_at_ms,NEW.recorded_at_ms);
END;

CREATE TRIGGER appearance_audit_unsupported_event
AFTER INSERT ON event_journal
WHEN NEW.event_type LIKE 'tarot.%' OR NEW.event_type LIKE 'vox.%'
BEGIN
    INSERT INTO appearance_event_observation
      (source_event_id,event_type,aggregate_type,aggregate_id,guild_id,channel_id,
       actor_user_id,occurred_at_ms,recorded_at_ms,extraction_result,processed_at_ms)
    VALUES
      (NEW.event_id,NEW.event_type,NEW.aggregate_type,NEW.aggregate_id,NEW.guild_id,
       NEW.channel_id,NEW.actor_user_id,NEW.occurred_at_ms,NEW.recorded_at_ms,
       'source_not_enabled',NEW.recorded_at_ms);
END;

CREATE TABLE appearance_candidate (
    candidate_id TEXT PRIMARY KEY CHECK (length(candidate_id) = 36),
    candidate_type TEXT NOT NULL CHECK (candidate_type IN ('conversation','recurrence','chronicle_entry','session_started','session_completed','title_awarded','anniversary','simulation')),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    policy_version TEXT NOT NULL REFERENCES appearance_policy_snapshot(policy_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    deduplication_key TEXT NOT NULL UNIQUE CHECK (length(deduplication_key) BETWEEN 1 AND 200),
    context_json TEXT NOT NULL CHECK (json_valid(context_json) AND length(CAST(context_json AS BLOB)) <= 16384),
    theme_key TEXT CHECK (theme_key IS NULL OR length(theme_key) BETWEEN 1 AND 160),
    owner_simulation INTEGER NOT NULL DEFAULT 0 CHECK (owner_simulation IN (0,1)),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms),
    context_expires_at_ms INTEGER NOT NULL CHECK (context_expires_at_ms > created_at_ms),
    evaluation_started_at_ms INTEGER,
    CHECK (evaluation_started_at_ms IS NULL OR evaluation_started_at_ms >= created_at_ms)
) STRICT;

CREATE TABLE appearance_candidate_source (
    candidate_id TEXT NOT NULL REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    source_kind TEXT NOT NULL CHECK (source_kind IN ('message','event','simulation','chronicle_entry')),
    source_id TEXT NOT NULL CHECK (length(source_id) BETWEEN 1 AND 128),
    source_rank INTEGER NOT NULL CHECK (source_rank BETWEEN 0 AND 24),
    PRIMARY KEY(candidate_id,source_kind,source_id),
    UNIQUE(candidate_id,source_rank)
) STRICT;

CREATE TABLE appearance_candidate_actor (
    candidate_id TEXT NOT NULL REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY(candidate_id,user_id)
) STRICT;

CREATE TABLE appearance_decision (
    decision_id TEXT PRIMARY KEY CHECK (length(decision_id) = 36),
    candidate_id TEXT NOT NULL UNIQUE REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    policy_version TEXT NOT NULL REFERENCES appearance_policy_snapshot(policy_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    application_instance_id TEXT NOT NULL CHECK (length(application_instance_id) BETWEEN 1 AND 128),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision BETWEEN 1 AND 2),
    state TEXT NOT NULL CHECK (state IN ('model_pending','final')),
    action TEXT CHECK (action IS NULL OR action IN ('reject','hypothetical')),
    reason TEXT CHECK (reason IS NULL OR (length(reason) BETWEEN 1 AND 96 AND reason NOT GLOB '*[^a-z0-9_.-]*')),
    gate_json TEXT NOT NULL CHECK (json_valid(gate_json) AND length(gate_json) <= 8192),
    score_json TEXT NOT NULL CHECK (json_valid(score_json) AND length(score_json) <= 8192),
    score INTEGER NOT NULL CHECK (score BETWEEN 0 AND 100),
    human_message_count INTEGER NOT NULL CHECK (human_message_count >= 0),
    model_status TEXT NOT NULL CHECK (length(model_status) BETWEEN 1 AND 96),
    serious_categories_json TEXT NOT NULL DEFAULT '[]'
      CHECK (json_valid(serious_categories_json)
             AND json_type(serious_categories_json) = 'array'
             AND json_array_length(serious_categories_json) <= 7
             AND length(serious_categories_json) <= 512),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    finalized_at_ms INTEGER,
    CHECK ((state = 'final') = (action IS NOT NULL AND reason IS NOT NULL AND finalized_at_ms IS NOT NULL))
) STRICT;

CREATE TRIGGER appearance_decision_transition_only
BEFORE UPDATE ON appearance_decision
WHEN NOT (OLD.state='model_pending' AND OLD.revision=1 AND NEW.state='final'
          AND NEW.revision=2 AND NEW.decision_id=OLD.decision_id
          AND NEW.candidate_id=OLD.candidate_id AND NEW.policy_version=OLD.policy_version
          AND NEW.application_instance_id=OLD.application_instance_id
          AND NEW.created_at_ms=OLD.created_at_ms)
BEGIN
    SELECT RAISE(ABORT, 'invalid appearance decision transition');
END;

CREATE TRIGGER appearance_decision_no_delete
BEFORE DELETE ON appearance_decision BEGIN
    SELECT RAISE(ABORT, 'appearance decisions are durable audit records');
END;

CREATE TABLE appearance_decision_memory (
    decision_id TEXT NOT NULL REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    memory_id TEXT NOT NULL REFERENCES memory(memory_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    memory_revision INTEGER NOT NULL CHECK (memory_revision > 0),
    selection_rank INTEGER NOT NULL CHECK (selection_rank BETWEEN 0 AND 2),
    used_by_model INTEGER NOT NULL CHECK (used_by_model IN (0,1)),
    PRIMARY KEY(decision_id,memory_id),
    UNIQUE(decision_id,selection_rank)
) STRICT;

CREATE TABLE appearance_preview (
    decision_id TEXT PRIMARY KEY REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    preview_text TEXT NOT NULL CHECK (length(CAST(preview_text AS BLOB)) BETWEEN 1 AND 2000),
    tone TEXT NOT NULL CHECK (tone IN ('warm','wry','celebratory','reflective','playful')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms)
) STRICT;

CREATE INDEX appearance_activity_recent ON appearance_message_activity(policy_version,observed_at_ms DESC,message_id DESC);
CREATE INDEX appearance_activity_unconsumed ON appearance_message_activity(policy_version,observed_at_ms,message_id) WHERE consumed_candidate_id IS NULL AND author_is_bot=0;
CREATE INDEX appearance_activity_expiry ON appearance_message_activity(expires_at_ms,message_id);
CREATE INDEX appearance_event_unprocessed ON appearance_event_observation(recorded_at_ms,source_event_id) WHERE processed_at_ms IS NULL;
CREATE INDEX appearance_candidate_expiry ON appearance_candidate(expires_at_ms,candidate_id);
CREATE INDEX appearance_candidate_context_expiry ON appearance_candidate(context_expires_at_ms,candidate_id);
CREATE INDEX appearance_decision_recent ON appearance_decision(created_at_ms DESC,decision_id DESC);
CREATE INDEX appearance_decision_action_recent ON appearance_decision(action,finalized_at_ms DESC) WHERE state='final';

CREATE TRIGGER appearance_dry_run_outbox_guard
BEFORE INSERT ON outbox_message
WHEN NEW.kind LIKE 'appearance.%'
  OR NEW.aggregate_type IN ('appearance','appearance_candidate','appearance_decision')
  OR NEW.aggregate_id IN (SELECT candidate_id FROM appearance_candidate)
  OR NEW.aggregate_id IN (SELECT decision_id FROM appearance_decision)
BEGIN
    SELECT RAISE(ABORT, 'appearance public outbox is forbidden in schema v7');
END;
