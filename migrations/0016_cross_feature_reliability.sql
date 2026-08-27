-- Milestone 18: cross-feature reliability, provider controls, and retention.

-- Accepted voice usage remains immutable while present, but M18 gives it the
-- documented finite metadata-retention window.
DROP TRIGGER voice_transcription_usage_no_delete;

ALTER TABLE application_instance
ADD COLUMN heartbeat_at_ms INTEGER
    CHECK (heartbeat_at_ms IS NULL OR heartbeat_at_ms>=started_at_ms);
UPDATE application_instance
SET heartbeat_at_ms=COALESCE(stopped_at_ms,started_at_ms);

UPDATE scheduled_job
SET state='cancelled', terminal_at_ms=max(updated_at_ms,created_at_ms),
    updated_at_ms=max(updated_at_ms,created_at_ms),
    last_error_code='orchestrator_cutover'
WHERE state='pending'
  AND job_type IN ('appearance.scan.v1','tarot.integration-scan.v1');

CREATE TABLE ai_generation_attempt (
    attempt_id TEXT PRIMARY KEY
        CHECK (length(attempt_id)=36 AND substr(attempt_id,9,1)='-'
               AND substr(attempt_id,14,1)='-' AND substr(attempt_id,19,1)='-'
               AND substr(attempt_id,24,1)='-'
               AND length(replace(attempt_id,'-',''))=32
               AND attempt_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(attempt_id,15,1)='4'
               AND substr(attempt_id,20,1) IN ('8','9','a','b')),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    requester_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    purpose TEXT NOT NULL CHECK (purpose IN
        ('direct','chronicle_summary','appearance','vox_narration','vox_session')),
    priority TEXT NOT NULL CHECK (priority IN ('direct','explicit','optional')),
    model TEXT NOT NULL CHECK (length(CAST(model AS BLOB)) BETWEEN 1 AND 128),
    input_rate_micro_usd_per_million INTEGER NOT NULL
        CHECK (input_rate_micro_usd_per_million>0),
    output_rate_micro_usd_per_million INTEGER NOT NULL
        CHECK (output_rate_micro_usd_per_million>0),
    reserved_input_tokens INTEGER NOT NULL
        CHECK (reserved_input_tokens BETWEEN 1 AND 16000),
    reserved_output_tokens INTEGER NOT NULL
        CHECK (reserved_output_tokens BETWEEN 1 AND 500),
    reserved_micro_usd INTEGER NOT NULL CHECK (reserved_micro_usd>0),
    actual_input_tokens INTEGER CHECK
        (actual_input_tokens IS NULL OR actual_input_tokens BETWEEN 0 AND 16000),
    actual_output_tokens INTEGER CHECK
        (actual_output_tokens IS NULL OR actual_output_tokens BETWEEN 0 AND 500),
    actual_micro_usd INTEGER CHECK
        (actual_micro_usd IS NULL OR actual_micro_usd BETWEEN 0 AND reserved_micro_usd),
    provider_sent INTEGER NOT NULL DEFAULT 0 CHECK (provider_sent IN (0,1)),
    provider_request_id TEXT CHECK
        (provider_request_id IS NULL OR length(CAST(provider_request_id AS BLOB)) BETWEEN 1 AND 128),
    state TEXT NOT NULL CHECK
        (state IN ('reserved','submitted','succeeded','failed','cancelled','unknown')),
    result_code TEXT CHECK
        (result_code IS NULL OR (length(result_code) BETWEEN 1 AND 96
         AND result_code NOT GLOB '*[^a-z0-9_.-]*')),
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(CAST(idempotency_key AS BLOB)) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms>=0),
    submitted_at_ms INTEGER CHECK
        (submitted_at_ms IS NULL OR submitted_at_ms>=created_at_ms),
    completed_at_ms INTEGER CHECK
        (completed_at_ms IS NULL OR completed_at_ms>=created_at_ms),
    CHECK ((state IN ('succeeded','failed','cancelled','unknown')) =
           (completed_at_ms IS NOT NULL)),
    CHECK ((state IN ('submitted','succeeded','failed','unknown')) <= provider_sent),
    CHECK ((state='succeeded') =
           (actual_input_tokens IS NOT NULL AND actual_output_tokens IS NOT NULL
            AND actual_micro_usd IS NOT NULL))
) STRICT;

CREATE INDEX ai_generation_attempt_day
    ON ai_generation_attempt(created_at_ms, state);
CREATE INDEX ai_generation_attempt_user_direct
    ON ai_generation_attempt(requester_user_id, created_at_ms)
    WHERE priority='direct';
CREATE INDEX ai_generation_attempt_retention
    ON ai_generation_attempt(completed_at_ms)
    WHERE completed_at_ms IS NOT NULL;

CREATE TABLE provider_circuit_state (
    provider TEXT PRIMARY KEY CHECK
        (provider IN ('openai_text','openai_tts','openai_transcription')),
    state TEXT NOT NULL CHECK (state IN ('closed','open','half_open')),
    consecutive_failures INTEGER NOT NULL CHECK (consecutive_failures>=0),
    first_failure_at_ms INTEGER CHECK (first_failure_at_ms IS NULL OR first_failure_at_ms>=0),
    opened_at_ms INTEGER CHECK (opened_at_ms IS NULL OR opened_at_ms>=0),
    retry_after_ms INTEGER CHECK
        (retry_after_ms IS NULL OR (opened_at_ms IS NOT NULL AND retry_after_ms>=opened_at_ms)),
    indefinite INTEGER NOT NULL DEFAULT 0 CHECK (indefinite IN (0,1)),
    probe_in_flight INTEGER NOT NULL DEFAULT 0 CHECK (probe_in_flight IN (0,1)),
    last_error_code TEXT CHECK
        (last_error_code IS NULL OR (length(last_error_code) BETWEEN 1 AND 96
         AND last_error_code NOT GLOB '*[^a-z0-9_.-]*')),
    revision INTEGER NOT NULL CHECK (revision>0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms>=0),
    CHECK ((state='closed') = (opened_at_ms IS NULL AND retry_after_ms IS NULL
                               AND indefinite=0 AND probe_in_flight=0)),
    CHECK (state<>'half_open' OR probe_in_flight=1)
) STRICT;

INSERT INTO provider_circuit_state
    (provider,state,consecutive_failures,revision,updated_at_ms)
VALUES ('openai_text','closed',0,1,0),
       ('openai_tts','closed',0,1,0),
       ('openai_transcription','closed',0,1,0);

CREATE TABLE provider_circuit_transition (
    transition_id TEXT PRIMARY KEY CHECK
        (length(transition_id)=36 AND substr(transition_id,9,1)='-'
         AND substr(transition_id,14,1)='-'
         AND substr(transition_id,19,1)='-'
         AND substr(transition_id,24,1)='-'
         AND length(replace(transition_id,'-',''))=32
         AND transition_id NOT GLOB '*[^0-9a-f-]*'
         AND substr(transition_id,15,1)='4'
         AND substr(transition_id,20,1) IN ('8','9','a','b')),
    provider TEXT NOT NULL REFERENCES provider_circuit_state(provider)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_state TEXT NOT NULL CHECK (from_state IN ('closed','open','half_open')),
    to_state TEXT NOT NULL CHECK (to_state IN ('closed','open','half_open')),
    reason_code TEXT NOT NULL CHECK
        (length(reason_code) BETWEEN 1 AND 96
         AND reason_code NOT GLOB '*[^a-z0-9_.-]*'),
    from_revision INTEGER NOT NULL CHECK (from_revision>0),
    to_revision INTEGER NOT NULL CHECK (to_revision=from_revision+1),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms>=0),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160)
) STRICT;
CREATE TRIGGER provider_circuit_transition_no_update
BEFORE UPDATE ON provider_circuit_transition BEGIN
  SELECT RAISE(ABORT,'provider circuit transitions are immutable');
END;
CREATE TRIGGER provider_circuit_transition_no_delete
BEFORE DELETE ON provider_circuit_transition BEGIN
  SELECT RAISE(ABORT,'provider circuit transitions are immutable');
END;

CREATE TABLE runtime_feature_control (
    feature TEXT PRIMARY KEY CHECK (feature IN ('text-ai','tts','vox-output')),
    disabled INTEGER NOT NULL CHECK (disabled IN (0,1)),
    revision INTEGER NOT NULL CHECK (revision>0),
    actor_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    changed_at_ms INTEGER NOT NULL CHECK (changed_at_ms>=0)
) STRICT;
INSERT INTO runtime_feature_control(feature,disabled,revision,changed_at_ms)
VALUES ('text-ai',0,1,0),('tts',0,1,0),('vox-output',0,1,0);

CREATE TABLE runtime_feature_control_transition (
    transition_id TEXT PRIMARY KEY CHECK
        (length(transition_id)=36 AND substr(transition_id,9,1)='-'
         AND substr(transition_id,14,1)='-'
         AND substr(transition_id,19,1)='-'
         AND substr(transition_id,24,1)='-'
         AND length(replace(transition_id,'-',''))=32
         AND transition_id NOT GLOB '*[^0-9a-f-]*'
         AND substr(transition_id,15,1)='4'
         AND substr(transition_id,20,1) IN ('8','9','a','b')),
    feature TEXT NOT NULL REFERENCES runtime_feature_control(feature)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_disabled INTEGER NOT NULL CHECK (from_disabled IN (0,1)),
    to_disabled INTEGER NOT NULL CHECK (to_disabled IN (0,1)),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_revision INTEGER NOT NULL CHECK (from_revision>0),
    to_revision INTEGER NOT NULL CHECK (to_revision=from_revision+1),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms>=0),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160)
) STRICT;
CREATE TRIGGER runtime_feature_control_transition_no_update
BEFORE UPDATE ON runtime_feature_control_transition BEGIN
  SELECT RAISE(ABORT,'runtime feature control transitions are immutable');
END;
CREATE TRIGGER runtime_feature_control_transition_no_delete
BEFORE DELETE ON runtime_feature_control_transition BEGIN
  SELECT RAISE(ABORT,'runtime feature control transitions are immutable');
END;

CREATE TABLE retention_run (
    run_id TEXT PRIMARY KEY CHECK
        (length(run_id)=36 AND substr(run_id,9,1)='-'
         AND substr(run_id,14,1)='-'
         AND substr(run_id,19,1)='-'
         AND substr(run_id,24,1)='-'
         AND length(replace(run_id,'-',''))=32
         AND run_id NOT GLOB '*[^0-9a-f-]*'
         AND substr(run_id,15,1)='4'
         AND substr(run_id,20,1) IN ('8','9','a','b')),
    state TEXT NOT NULL CHECK (state IN ('running','completed','failed')),
    counts_json TEXT NOT NULL CHECK
        (json_valid(counts_json) AND json_type(counts_json)='object'
         AND length(counts_json) BETWEEN 2 AND 4096),
    started_at_ms INTEGER NOT NULL CHECK (started_at_ms>=0),
    completed_at_ms INTEGER CHECK
        (completed_at_ms IS NULL OR completed_at_ms>=started_at_ms),
    error_code TEXT CHECK
        (error_code IS NULL OR (length(error_code) BETWEEN 1 AND 96
         AND error_code NOT GLOB '*[^a-z0-9_.-]*')),
    CHECK ((state='running') = (completed_at_ms IS NULL)),
    CHECK ((state='failed') = (error_code IS NOT NULL))
) STRICT;
CREATE INDEX retention_run_recent ON retention_run(started_at_ms DESC,run_id DESC);

CREATE TABLE interaction_list_snapshot (
    snapshot_id TEXT PRIMARY KEY
        CHECK (length(snapshot_id)=36 AND substr(snapshot_id,9,1)='-'
               AND substr(snapshot_id,14,1)='-' AND substr(snapshot_id,19,1)='-'
               AND substr(snapshot_id,24,1)='-'
               AND length(replace(snapshot_id,'-',''))=32
               AND snapshot_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(snapshot_id,15,1)='4'
               AND substr(snapshot_id,20,1) IN ('8','9','a','b')),
    snapshot_kind TEXT NOT NULL
        CHECK (snapshot_kind IN ('chronicle_titles','tarot_house_history')),
    viewer_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    subject_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    owner_view INTEGER NOT NULL DEFAULT 0 CHECK (owner_view IN (0,1)),
    item_count INTEGER NOT NULL CHECK (item_count BETWEEN 0 AND 50),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms>=0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms>created_at_ms)
) STRICT;
CREATE TABLE interaction_list_snapshot_item (
    snapshot_id TEXT NOT NULL REFERENCES interaction_list_snapshot(snapshot_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 49),
    item_id TEXT NOT NULL
        CHECK (length(item_id)=36 AND substr(item_id,9,1)='-'
               AND substr(item_id,14,1)='-' AND substr(item_id,19,1)='-'
               AND substr(item_id,24,1)='-'
               AND length(replace(item_id,'-',''))=32
               AND item_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(item_id,15,1)='4'
               AND substr(item_id,20,1) IN ('8','9','a','b')),
    PRIMARY KEY (snapshot_id,position),
    UNIQUE (snapshot_id,item_id)
) STRICT;
CREATE INDEX interaction_list_snapshot_expiry
    ON interaction_list_snapshot(expires_at_ms,snapshot_id);
CREATE TRIGGER interaction_list_snapshot_no_update
BEFORE UPDATE ON interaction_list_snapshot BEGIN
  SELECT RAISE(ABORT,'interaction list snapshots are immutable');
END;
CREATE TRIGGER interaction_list_snapshot_item_no_update
BEFORE UPDATE ON interaction_list_snapshot_item BEGIN
  SELECT RAISE(ABORT,'interaction list snapshot items are immutable');
END;
