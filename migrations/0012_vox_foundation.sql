CREATE TABLE voice_session (
    session_id TEXT PRIMARY KEY
        CHECK (length(session_id) = 36
               AND substr(session_id, 9, 1) = '-'
               AND substr(session_id, 14, 1) = '-'
               AND substr(session_id, 19, 1) = '-'
               AND substr(session_id, 24, 1) = '-'
               AND length(replace(session_id, '-', '')) = 32
               AND session_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(session_id, 15, 1) = '4'
               AND substr(session_id, 20, 1) IN ('8', '9', 'a', 'b')),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    text_channel_id TEXT NOT NULL
        CHECK (length(text_channel_id) BETWEEN 1 AND 20
               AND text_channel_id NOT GLOB '*[^0-9]*'
               AND text_channel_id <> '0'
               AND (length(text_channel_id) = 1 OR substr(text_channel_id, 1, 1) <> '0')
               AND (length(text_channel_id) < 20 OR text_channel_id <= '18446744073709551615')),
    voice_channel_id TEXT NOT NULL
        CHECK (length(voice_channel_id) BETWEEN 1 AND 20
               AND voice_channel_id NOT GLOB '*[^0-9]*'
               AND voice_channel_id <> '0'
               AND (length(voice_channel_id) = 1 OR substr(voice_channel_id, 1, 1) <> '0')
               AND (length(voice_channel_id) < 20 OR voice_channel_id <= '18446744073709551615')),
    summoner_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    deployment_instance_id TEXT NOT NULL REFERENCES application_instance(instance_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    state TEXT NOT NULL CHECK (state IN
        ('connecting','ready','muted','reconnecting','leaving','inactive','failed')),
    state_version INTEGER NOT NULL CHECK (state_version > 0),
    connection_generation INTEGER NOT NULL CHECK (connection_generation > 0),
    reconnect_count INTEGER NOT NULL DEFAULT 0 CHECK (reconnect_count BETWEEN 0 AND 1),
    fixture_state TEXT NOT NULL DEFAULT 'pending'
        CHECK (fixture_state IN ('pending','queued','played','failed')),
    fixture_marker TEXT UNIQUE
        CHECK (fixture_marker IS NULL OR length(fixture_marker) BETWEEN 1 AND 128),
    fixture_queued_at_ms INTEGER CHECK (fixture_queued_at_ms IS NULL OR fixture_queued_at_ms >= 0),
    fixture_played_at_ms INTEGER CHECK (fixture_played_at_ms IS NULL OR fixture_played_at_ms >= 0),
    empty_since_ms INTEGER CHECK (empty_since_ms IS NULL OR empty_since_ms >= 0),
    timeout_job_id TEXT REFERENCES scheduled_job(job_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    started_at_ms INTEGER NOT NULL CHECK (started_at_ms >= 0),
    last_active_at_ms INTEGER NOT NULL CHECK (last_active_at_ms >= started_at_ms),
    ended_at_ms INTEGER CHECK (ended_at_ms IS NULL OR ended_at_ms >= started_at_ms),
    end_reason TEXT CHECK (end_reason IS NULL OR
        (length(end_reason) BETWEEN 1 AND 64 AND end_reason NOT GLOB '*[^a-z0-9_.-]*')),
    last_failure_category TEXT CHECK (last_failure_category IS NULL OR
        (length(last_failure_category) BETWEEN 1 AND 64
         AND last_failure_category NOT GLOB '*[^a-z0-9_.-]*')),
    CHECK ((state IN ('inactive','failed')) = (ended_at_ms IS NOT NULL)),
    CHECK ((ended_at_ms IS NULL) = (end_reason IS NULL)),
    CHECK ((fixture_state = 'pending') = (fixture_queued_at_ms IS NULL)),
    CHECK ((fixture_state = 'played') = (fixture_played_at_ms IS NOT NULL)),
    CHECK (fixture_played_at_ms IS NULL OR fixture_queued_at_ms IS NOT NULL)
) STRICT;

CREATE UNIQUE INDEX voice_session_one_active_guild
    ON voice_session(guild_id)
    WHERE state IN ('connecting','ready','muted','reconnecting','leaving');
CREATE INDEX voice_session_recent
    ON voice_session(started_at_ms DESC, session_id DESC);

CREATE TRIGGER voice_session_guard_update
BEFORE UPDATE ON voice_session
BEGIN
    SELECT CASE WHEN NEW.session_id <> OLD.session_id
                     OR NEW.guild_id <> OLD.guild_id
                     OR NEW.text_channel_id <> OLD.text_channel_id
                     OR NEW.voice_channel_id <> OLD.voice_channel_id
                     OR NEW.summoner_user_id <> OLD.summoner_user_id
                     OR NEW.deployment_instance_id <> OLD.deployment_instance_id
                     OR NEW.started_at_ms <> OLD.started_at_ms
                THEN RAISE(ABORT, 'voice session identity is immutable') END;
    SELECT CASE WHEN NEW.state_version <> OLD.state_version + 1
                THEN RAISE(ABORT, 'voice session revision must advance once') END;
    SELECT CASE WHEN OLD.state IN ('inactive','failed')
                THEN RAISE(ABORT, 'terminal voice session is immutable') END;
END;

CREATE TRIGGER voice_session_no_delete
BEFORE DELETE ON voice_session
BEGIN SELECT RAISE(ABORT, 'voice sessions are retained'); END;

CREATE TABLE voice_session_transition (
    transition_id TEXT PRIMARY KEY
        CHECK (length(transition_id) = 36
               AND substr(transition_id, 9, 1) = '-'
               AND substr(transition_id, 14, 1) = '-'
               AND substr(transition_id, 19, 1) = '-'
               AND substr(transition_id, 24, 1) = '-'
               AND length(replace(transition_id, '-', '')) = 32
               AND transition_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(transition_id, 15, 1) = '4'
               AND substr(transition_id, 20, 1) IN ('8', '9', 'a', 'b')),
    session_id TEXT NOT NULL REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_state TEXT NOT NULL CHECK (from_state IN
        ('inactive','connecting','ready','muted','reconnecting','leaving','failed')),
    to_state TEXT NOT NULL CHECK (to_state IN
        ('inactive','connecting','ready','muted','reconnecting','leaving','failed')),
    from_version INTEGER NOT NULL CHECK (from_version >= 0),
    to_version INTEGER NOT NULL CHECK (to_version = from_version + 1),
    reason TEXT NOT NULL CHECK (length(reason) BETWEEN 1 AND 64
        AND reason NOT GLOB '*[^a-z0-9_.-]*'),
    actor_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0)
) STRICT;

CREATE INDEX voice_session_transition_history
    ON voice_session_transition(session_id, to_version);
CREATE TRIGGER voice_session_transition_no_update BEFORE UPDATE ON voice_session_transition
BEGIN SELECT RAISE(ABORT, 'voice transitions are append-only'); END;
CREATE TRIGGER voice_session_transition_no_delete BEFORE DELETE ON voice_session_transition
BEGIN SELECT RAISE(ABORT, 'voice transitions are retained'); END;

CREATE TABLE voice_interaction_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    operation TEXT NOT NULL CHECK (operation IN ('summon','status','leave','test_disconnect')),
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

CREATE INDEX voice_interaction_receipt_actor
    ON voice_interaction_receipt(actor_user_id, created_at_ms DESC);
CREATE TRIGGER voice_interaction_receipt_no_update BEFORE UPDATE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'voice interaction receipts are immutable'); END;
CREATE TRIGGER voice_interaction_receipt_no_delete BEFORE DELETE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'voice interaction receipts are retained'); END;

CREATE TABLE voice_public_outbox_dependency (
    session_id TEXT NOT NULL REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    predecessor_outbox_id TEXT NOT NULL REFERENCES outbox_message(outbox_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    successor_outbox_id TEXT NOT NULL REFERENCES outbox_message(outbox_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    dependency_kind TEXT NOT NULL CHECK (dependency_kind = 'ready_before_terminal'),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(predecessor_outbox_id, successor_outbox_id),
    CHECK (predecessor_outbox_id <> successor_outbox_id)
) STRICT;

CREATE INDEX voice_public_outbox_dependency_successor
    ON voice_public_outbox_dependency(successor_outbox_id);
CREATE TRIGGER voice_public_outbox_dependency_no_update
BEFORE UPDATE ON voice_public_outbox_dependency
BEGIN SELECT RAISE(ABORT, 'voice public delivery dependencies are immutable'); END;
CREATE TRIGGER voice_public_outbox_dependency_no_delete
BEFORE DELETE ON voice_public_outbox_dependency
BEGIN SELECT RAISE(ABORT, 'voice public delivery dependencies are retained'); END;
