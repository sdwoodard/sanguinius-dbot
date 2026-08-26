ALTER TABLE voice_session ADD COLUMN muted_at_ms INTEGER;
ALTER TABLE voice_session ADD COLUMN mute_until_ms INTEGER;
ALTER TABLE voice_session ADD COLUMN mute_job_id TEXT REFERENCES scheduled_job(job_id)
    ON UPDATE RESTRICT ON DELETE RESTRICT;

CREATE TRIGGER voice_session_mute_guard_update
BEFORE UPDATE ON voice_session
BEGIN
    SELECT CASE WHEN NEW.state = 'muted' AND NEW.muted_at_ms IS NULL
                THEN RAISE(ABORT, 'muted voice session requires timestamp') END;
    SELECT CASE WHEN NEW.state NOT IN ('muted','reconnecting')
                     AND (NEW.muted_at_ms IS NOT NULL OR NEW.mute_until_ms IS NOT NULL
                          OR NEW.mute_job_id IS NOT NULL)
                THEN RAISE(ABORT, 'unmuted voice session retains mute metadata') END;
    SELECT CASE WHEN NEW.mute_until_ms IS NOT NULL
                     AND (NEW.mute_until_ms <= NEW.muted_at_ms OR NEW.mute_job_id IS NULL)
                THEN RAISE(ABORT, 'timed mute metadata is invalid') END;
    SELECT CASE WHEN NEW.mute_job_id IS NOT NULL AND NEW.mute_until_ms IS NULL
                THEN RAISE(ABORT, 'mute job requires expiry') END;
END;

DROP TRIGGER voice_interaction_receipt_no_update;
DROP TRIGGER voice_interaction_receipt_no_delete;
DROP INDEX voice_interaction_receipt_actor;
ALTER TABLE voice_interaction_receipt RENAME TO voice_interaction_receipt_v12;
CREATE TABLE voice_interaction_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    operation TEXT NOT NULL CHECK (operation IN
        ('summon','status','leave','test_disconnect','say','mute','voice','speech_test')),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL
        CHECK (length(channel_id) BETWEEN 1 AND 20 AND channel_id NOT GLOB '*[^0-9]*'
               AND channel_id <> '0' AND (length(channel_id) = 1 OR substr(channel_id, 1, 1) <> '0')
               AND (length(channel_id) < 20 OR channel_id <= '18446744073709551615')),
    request_json TEXT NOT NULL CHECK (json_valid(request_json)
        AND json_type(request_json) = 'object' AND length(CAST(request_json AS BLOB)) BETWEEN 2 AND 2048),
    result_json TEXT NOT NULL CHECK (json_valid(result_json)
        AND json_type(result_json) = 'object' AND length(CAST(result_json AS BLOB)) BETWEEN 2 AND 4096),
    session_id TEXT REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;
INSERT INTO voice_interaction_receipt
SELECT * FROM voice_interaction_receipt_v12;
DROP TABLE voice_interaction_receipt_v12;
CREATE INDEX voice_interaction_receipt_actor
    ON voice_interaction_receipt(actor_user_id, created_at_ms DESC);
CREATE TRIGGER voice_interaction_receipt_no_update BEFORE UPDATE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'voice interaction receipts are immutable'); END;
CREATE TRIGGER voice_interaction_receipt_no_delete BEFORE DELETE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'voice interaction receipts are retained'); END;

CREATE TABLE vox_voice_configuration (
    guild_id TEXT PRIMARY KEY REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    voice_id TEXT NOT NULL CHECK (voice_id = 'onyx'),
    revision INTEGER NOT NULL CHECK (revision > 0),
    updated_by_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
) STRICT;
INSERT INTO vox_voice_configuration(guild_id,voice_id,revision,updated_at_ms)
SELECT guild_id,'onyx',1,created_at_ms FROM guild_config;

CREATE TABLE tts_cache_entry (
    cache_key TEXT PRIMARY KEY CHECK (length(cache_key) = 64
        AND cache_key NOT GLOB '*[^0-9a-f]*'),
    file_name TEXT NOT NULL UNIQUE CHECK (file_name = cache_key || '.pcm'),
    checksum TEXT NOT NULL CHECK (length(checksum) = 64
        AND checksum NOT GLOB '*[^0-9a-f]*'),
    byte_count INTEGER NOT NULL CHECK (byte_count BETWEEN 4 AND 3840000
        AND byte_count % 4 = 0),
    frame_count INTEGER NOT NULL CHECK (frame_count = byte_count / 4),
    provider TEXT NOT NULL CHECK (provider IN ('openai','static')),
    model TEXT NOT NULL CHECK (length(model) BETWEEN 1 AND 64),
    voice_id TEXT NOT NULL CHECK (length(voice_id) BETWEEN 1 AND 64),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    last_access_at_ms INTEGER NOT NULL CHECK (last_access_at_ms >= created_at_ms)
) STRICT;
CREATE INDEX tts_cache_entry_lru ON tts_cache_entry(last_access_at_ms,cache_key);

CREATE TABLE speech_item (
    speech_id TEXT PRIMARY KEY
        CHECK (length(speech_id) = 36 AND substr(speech_id,9,1) = '-'
               AND substr(speech_id,14,1) = '-' AND substr(speech_id,19,1) = '-'
               AND substr(speech_id,24,1) = '-' AND length(replace(speech_id,'-','')) = 32
               AND speech_id NOT GLOB '*[^0-9a-f-]*'),
    voice_session_id TEXT NOT NULL REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_event_id TEXT CHECK (source_event_id IS NULL OR length(source_event_id) BETWEEN 1 AND 160),
    source_kind TEXT NOT NULL CHECK (length(source_kind) BETWEEN 1 AND 64
        AND source_kind NOT GLOB '*[^a-z0-9_.-]*'),
    text TEXT CHECK (text IS NULL OR length(CAST(text AS BLOB)) BETWEEN 1 AND 1400),
    text_hash TEXT NOT NULL CHECK (length(text_hash) = 64 AND text_hash NOT GLOB '*[^0-9a-f]*'),
    scalar_count INTEGER NOT NULL CHECK (scalar_count BETWEEN 1 AND 350),
    provider TEXT NOT NULL CHECK (provider IN ('openai','static')),
    model TEXT NOT NULL CHECK (length(model) BETWEEN 1 AND 64),
    voice_id TEXT NOT NULL CHECK (length(voice_id) BETWEEN 1 AND 64),
    priority INTEGER NOT NULL CHECK (priority IN (100,200,300,400)),
    state TEXT NOT NULL CHECK (state IN
        ('pending','synthesizing','ready','playing','played','failed','expired','cancelled')),
    state_version INTEGER NOT NULL CHECK (state_version > 0),
    earliest_at_ms INTEGER NOT NULL CHECK (earliest_at_ms >= 0),
    expires_at_ms INTEGER CHECK (expires_at_ms IS NULL OR expires_at_ms > earliest_at_ms),
    interruptible INTEGER NOT NULL CHECK (interruptible IN (0,1)),
    deduplication_key TEXT NOT NULL UNIQUE CHECK (length(deduplication_key) BETWEEN 1 AND 160),
    provider_request_id TEXT CHECK (provider_request_id IS NULL OR length(provider_request_id) BETWEEN 1 AND 256),
    cache_key TEXT CHECK (cache_key IS NULL OR (length(cache_key) = 64
        AND cache_key NOT GLOB '*[^0-9a-f]*')),
    cache_checksum TEXT CHECK (cache_checksum IS NULL OR (length(cache_checksum) = 64
        AND cache_checksum NOT GLOB '*[^0-9a-f]*')),
    marker TEXT UNIQUE CHECK (marker IS NULL OR length(marker) BETWEEN 1 AND 128),
    duration_ms INTEGER CHECK (duration_ms IS NULL OR duration_ms BETWEEN 1 AND 20000),
    attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count BETWEEN 0 AND 2),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    terminal_at_ms INTEGER CHECK (terminal_at_ms IS NULL OR terminal_at_ms >= created_at_ms),
    last_error_code TEXT CHECK (last_error_code IS NULL OR
        (length(last_error_code) BETWEEN 1 AND 64 AND last_error_code NOT GLOB '*[^a-z0-9_.-]*')),
    CHECK ((state IN ('pending','synthesizing')) = (text IS NOT NULL)),
    CHECK ((state IN ('played','failed','expired','cancelled')) = (terminal_at_ms IS NOT NULL)),
    CHECK ((cache_key IS NULL) = (cache_checksum IS NULL)),
    CHECK (state NOT IN ('ready','playing','played') OR
        (cache_key IS NOT NULL AND duration_ms IS NOT NULL)),
    CHECK (state <> 'playing' OR marker IS NOT NULL),
    CHECK (state <> 'played' OR marker IS NOT NULL)
) STRICT;
CREATE INDEX speech_item_queue
    ON speech_item(voice_session_id,state,priority DESC,earliest_at_ms,created_at_ms,speech_id);
CREATE INDEX speech_item_terminal ON speech_item(terminal_at_ms) WHERE terminal_at_ms IS NOT NULL;
CREATE TRIGGER speech_item_guard_update BEFORE UPDATE ON speech_item
BEGIN
    SELECT CASE WHEN NEW.speech_id <> OLD.speech_id
                     OR NEW.voice_session_id <> OLD.voice_session_id
                     OR NEW.source_event_id IS NOT OLD.source_event_id
                     OR NEW.source_kind <> OLD.source_kind
                     OR NEW.text_hash <> OLD.text_hash
                     OR NEW.scalar_count <> OLD.scalar_count
                     OR NEW.provider <> OLD.provider OR NEW.model <> OLD.model
                     OR NEW.voice_id <> OLD.voice_id OR NEW.priority <> OLD.priority
                     OR NEW.earliest_at_ms <> OLD.earliest_at_ms
                     OR NEW.expires_at_ms IS NOT OLD.expires_at_ms
                     OR NEW.interruptible <> OLD.interruptible
                     OR NEW.deduplication_key <> OLD.deduplication_key
                     OR NEW.created_at_ms <> OLD.created_at_ms
                THEN RAISE(ABORT, 'speech item identity and policy are immutable') END;
    SELECT CASE WHEN NEW.state_version <> OLD.state_version + 1
                THEN RAISE(ABORT, 'speech revision must advance once') END;
    SELECT CASE WHEN OLD.state IN ('played','failed','expired','cancelled')
                THEN RAISE(ABORT, 'terminal speech item is immutable') END;
    SELECT CASE WHEN NOT (
        (OLD.state='pending' AND NEW.state IN ('synthesizing','cancelled','expired')) OR
        (OLD.state='synthesizing' AND NEW.state IN ('ready','failed','cancelled','expired')) OR
        (OLD.state='ready' AND NEW.state IN ('playing','failed','cancelled','expired')) OR
        (OLD.state='playing' AND NEW.state IN ('played','failed','cancelled')))
        THEN RAISE(ABORT, 'invalid speech transition') END;
END;

CREATE TABLE speech_item_transition (
    transition_id TEXT PRIMARY KEY CHECK (length(transition_id) = 36),
    speech_id TEXT NOT NULL REFERENCES speech_item(speech_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    from_state TEXT NOT NULL CHECK (from_state IN
        ('pending','synthesizing','ready','playing','played','failed','expired','cancelled')),
    to_state TEXT NOT NULL CHECK (to_state IN
        ('pending','synthesizing','ready','playing','played','failed','expired','cancelled')),
    from_version INTEGER NOT NULL CHECK (from_version > 0),
    to_version INTEGER NOT NULL CHECK (to_version = from_version + 1),
    reason TEXT NOT NULL CHECK (length(reason) BETWEEN 1 AND 64
        AND reason NOT GLOB '*[^a-z0-9_.-]*'),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0)
) STRICT;
CREATE INDEX speech_item_transition_history ON speech_item_transition(speech_id,to_version);
CREATE TRIGGER speech_item_transition_no_update BEFORE UPDATE ON speech_item_transition
BEGIN SELECT RAISE(ABORT, 'speech transitions are append-only'); END;

CREATE TABLE tts_usage_attempt (
    attempt_id TEXT PRIMARY KEY CHECK (length(attempt_id) = 36),
    -- Intentionally not an FK: terminal speech metadata expires after 30 days,
    -- while sanitized cost diagnostics remain available for 13 months.
    speech_id TEXT NOT NULL CHECK (length(speech_id) = 36),
    attempt_number INTEGER NOT NULL CHECK (attempt_number BETWEEN 1 AND 2),
    provider TEXT NOT NULL CHECK (provider = 'openai'),
    model TEXT NOT NULL CHECK (model = 'tts-1'),
    voice_id TEXT NOT NULL CHECK (voice_id = 'onyx'),
    scalar_count INTEGER NOT NULL CHECK (scalar_count BETWEEN 1 AND 350),
    estimated_micro_usd INTEGER NOT NULL CHECK (estimated_micro_usd BETWEEN 15 AND 5250),
    state TEXT NOT NULL CHECK (state IN ('submitted','succeeded','failed','unknown')),
    provider_request_id TEXT CHECK (provider_request_id IS NULL OR length(provider_request_id) BETWEEN 1 AND 256),
    latency_ms INTEGER CHECK (latency_ms IS NULL OR latency_ms >= 0),
    duration_ms INTEGER CHECK (duration_ms IS NULL OR duration_ms BETWEEN 1 AND 20000),
    error_code TEXT CHECK (error_code IS NULL OR
        (length(error_code) BETWEEN 1 AND 64 AND error_code NOT GLOB '*[^a-z0-9_.-]*')),
    submitted_at_ms INTEGER NOT NULL CHECK (submitted_at_ms >= 0),
    completed_at_ms INTEGER CHECK (completed_at_ms IS NULL OR completed_at_ms >= submitted_at_ms),
    UNIQUE(speech_id,attempt_number),
    CHECK ((state='submitted') = (completed_at_ms IS NULL))
) STRICT;
CREATE INDEX tts_usage_attempt_budget ON tts_usage_attempt(submitted_at_ms,state);
CREATE TRIGGER tts_usage_attempt_guard_update BEFORE UPDATE ON tts_usage_attempt
BEGIN
    SELECT CASE WHEN NEW.attempt_id <> OLD.attempt_id OR NEW.speech_id <> OLD.speech_id
                     OR NEW.attempt_number <> OLD.attempt_number
                     OR NEW.provider <> OLD.provider OR NEW.model <> OLD.model
                     OR NEW.voice_id <> OLD.voice_id OR NEW.scalar_count <> OLD.scalar_count
                     OR NEW.estimated_micro_usd <> OLD.estimated_micro_usd
                     OR NEW.submitted_at_ms <> OLD.submitted_at_ms
                THEN RAISE(ABORT, 'TTS usage identity is immutable') END;
    SELECT CASE WHEN OLD.state <> 'submitted' OR NEW.state = 'submitted'
                THEN RAISE(ABORT, 'TTS usage completion is immutable') END;
END;
