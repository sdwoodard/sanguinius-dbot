CREATE TABLE relationship_event (
    relationship_event_id TEXT PRIMARY KEY
        CHECK (length(relationship_event_id) = 36
               AND substr(relationship_event_id, 9, 1) = '-'
               AND substr(relationship_event_id, 14, 1) = '-'
               AND substr(relationship_event_id, 19, 1) = '-'
               AND substr(relationship_event_id, 24, 1) = '-'
               AND length(replace(relationship_event_id, '-', '')) = 32
               AND relationship_event_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(relationship_event_id, 15, 1) = '4'
               AND substr(relationship_event_id, 20, 1) IN ('8', '9', 'a', 'b')),
    subject_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_event_id TEXT NOT NULL
        REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_type TEXT NOT NULL
        CHECK (length(event_type) BETWEEN 1 AND 96
               AND event_type NOT GLOB '*[^a-z0-9_.-]*'),
    reason_code TEXT NOT NULL
        CHECK (length(reason_code) BETWEEN 1 AND 96
               AND reason_code NOT GLOB '*[^a-z0-9_.-]*'),
    policy_version INTEGER NOT NULL CHECK (policy_version > 0),
    reason_version INTEGER NOT NULL CHECK (reason_version > 0),
    subject_version INTEGER NOT NULL CHECK (subject_version > 0),
    delta_familiarity INTEGER NOT NULL CHECK (delta_familiarity BETWEEN -5 AND 5),
    delta_esteem INTEGER NOT NULL CHECK (delta_esteem BETWEEN -5 AND 5),
    delta_mirth INTEGER NOT NULL CHECK (delta_mirth BETWEEN -5 AND 5),
    delta_reliability INTEGER NOT NULL CHECK (delta_reliability BETWEEN -5 AND 5),
    delta_wariness INTEGER NOT NULL CHECK (delta_wariness BETWEEN -5 AND 5),
    old_familiarity INTEGER NOT NULL CHECK (old_familiarity BETWEEN 0 AND 100),
    old_esteem INTEGER NOT NULL CHECK (old_esteem BETWEEN 0 AND 100),
    old_mirth INTEGER NOT NULL CHECK (old_mirth BETWEEN 0 AND 100),
    old_reliability INTEGER NOT NULL CHECK (old_reliability BETWEEN 0 AND 100),
    old_wariness INTEGER NOT NULL CHECK (old_wariness BETWEEN 0 AND 100),
    new_familiarity INTEGER NOT NULL CHECK (new_familiarity BETWEEN 0 AND 100),
    new_esteem INTEGER NOT NULL CHECK (new_esteem BETWEEN 0 AND 100),
    new_mirth INTEGER NOT NULL CHECK (new_mirth BETWEEN 0 AND 100),
    new_reliability INTEGER NOT NULL CHECK (new_reliability BETWEEN 0 AND 100),
    new_wariness INTEGER NOT NULL CHECK (new_wariness BETWEEN 0 AND 100),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    is_test INTEGER NOT NULL DEFAULT 0 CHECK (is_test IN (0, 1)),
    UNIQUE (source_event_id, subject_user_id),
    UNIQUE (subject_user_id, subject_version),
    CHECK (new_familiarity = old_familiarity + delta_familiarity),
    CHECK (new_esteem = old_esteem + delta_esteem),
    CHECK (new_mirth = old_mirth + delta_mirth),
    CHECK (new_reliability = old_reliability + delta_reliability),
    CHECK (new_wariness = old_wariness + delta_wariness)
) STRICT;

CREATE TRIGGER relationship_event_no_update
BEFORE UPDATE ON relationship_event
BEGIN
    SELECT RAISE(ABORT, 'relationship_event is append-only');
END;

CREATE TRIGGER relationship_event_no_delete
BEFORE DELETE ON relationship_event
BEGIN
    SELECT RAISE(ABORT, 'relationship_event is append-only');
END;

CREATE TABLE relationship_state (
    subject_user_id TEXT PRIMARY KEY
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    familiarity INTEGER NOT NULL DEFAULT 0 CHECK (familiarity BETWEEN 0 AND 100),
    esteem INTEGER NOT NULL DEFAULT 0 CHECK (esteem BETWEEN 0 AND 100),
    mirth INTEGER NOT NULL DEFAULT 0 CHECK (mirth BETWEEN 0 AND 100),
    reliability INTEGER NOT NULL DEFAULT 0 CHECK (reliability BETWEEN 0 AND 100),
    wariness INTEGER NOT NULL DEFAULT 0 CHECK (wariness BETWEEN 0 AND 100),
    interaction_count INTEGER NOT NULL DEFAULT 0 CHECK (interaction_count >= 0),
    last_interaction_at_ms INTEGER NOT NULL CHECK (last_interaction_at_ms >= 0),
    projection_version INTEGER NOT NULL CHECK (projection_version > 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
) STRICT;

CREATE TABLE ai_prompt_attempt (
    attempt_id TEXT PRIMARY KEY
        CHECK (length(attempt_id) = 36
               AND substr(attempt_id, 9, 1) = '-'
               AND substr(attempt_id, 14, 1) = '-'
               AND substr(attempt_id, 19, 1) = '-'
               AND substr(attempt_id, 24, 1) = '-'
               AND length(replace(attempt_id, '-', '')) = 32
               AND attempt_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(attempt_id, 15, 1) = '4'
               AND substr(attempt_id, 20, 1) IN ('8', '9', 'a', 'b')),
    application_instance_id TEXT NOT NULL
        REFERENCES application_instance(instance_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    requester_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL
        REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL
        CHECK (length(channel_id) BETWEEN 1 AND 20
               AND channel_id NOT GLOB '*[^0-9]*'
               AND channel_id <> '0'
               AND (length(channel_id) = 1 OR substr(channel_id, 1, 1) <> '0')
               AND (length(channel_id) < 20
                    OR channel_id <= '18446744073709551615')),
    source_message_id TEXT NOT NULL
        CHECK (length(source_message_id) BETWEEN 1 AND 20
               AND source_message_id NOT GLOB '*[^0-9]*'
               AND source_message_id <> '0'
               AND (length(source_message_id) = 1
                    OR substr(source_message_id, 1, 1) <> '0')
               AND (length(source_message_id) < 20
                    OR source_message_id <= '18446744073709551615')),
    state TEXT NOT NULL
        CHECK (state IN ('prepared', 'succeeded', 'model_failed', 'cancelled',
                         'privacy_invalidated', 'abandoned')),
    correlation_id TEXT NOT NULL CHECK (length(correlation_id) BETWEEN 1 AND 160),
    preference_updated_at_ms INTEGER NOT NULL
        CHECK (preference_updated_at_ms >= 0),
    prepared_at_ms INTEGER NOT NULL CHECK (prepared_at_ms >= 0),
    completed_at_ms INTEGER
        CHECK (completed_at_ms IS NULL OR completed_at_ms >= prepared_at_ms),
    failure_code TEXT
        CHECK (failure_code IS NULL
               OR (length(failure_code) BETWEEN 1 AND 96
                   AND failure_code NOT GLOB '*[^a-z0-9_.-]*')),
    UNIQUE (guild_id, channel_id, source_message_id),
    CHECK ((state = 'prepared') = (completed_at_ms IS NULL)),
    CHECK ((state = 'succeeded') = (completed_at_ms IS NOT NULL
                                     AND failure_code IS NULL))
) STRICT;

CREATE TABLE ai_prompt_attempt_memory (
    attempt_id TEXT NOT NULL
        REFERENCES ai_prompt_attempt(attempt_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    memory_id TEXT NOT NULL
        REFERENCES memory(memory_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    memory_revision INTEGER NOT NULL CHECK (memory_revision > 0),
    rank_position INTEGER NOT NULL CHECK (rank_position BETWEEN 0 AND 2),
    selected_at_ms INTEGER NOT NULL CHECK (selected_at_ms >= 0),
    PRIMARY KEY (attempt_id, memory_id),
    UNIQUE (attempt_id, rank_position)
) STRICT;

CREATE TRIGGER ai_prompt_attempt_transition_only
BEFORE UPDATE ON ai_prompt_attempt
WHEN OLD.state <> 'prepared'
     OR NEW.state = 'prepared'
     OR NEW.attempt_id <> OLD.attempt_id
     OR NEW.application_instance_id <> OLD.application_instance_id
     OR NEW.requester_user_id <> OLD.requester_user_id
     OR NEW.guild_id <> OLD.guild_id
     OR NEW.channel_id <> OLD.channel_id
     OR NEW.source_message_id <> OLD.source_message_id
     OR NEW.correlation_id <> OLD.correlation_id
     OR NEW.preference_updated_at_ms <> OLD.preference_updated_at_ms
     OR NEW.prepared_at_ms <> OLD.prepared_at_ms
BEGIN
    SELECT RAISE(ABORT, 'invalid ai_prompt_attempt transition');
END;

CREATE TRIGGER ai_prompt_attempt_no_delete
BEFORE DELETE ON ai_prompt_attempt
BEGIN
    SELECT RAISE(ABORT, 'ai_prompt_attempt is an audit record');
END;

CREATE TRIGGER ai_prompt_attempt_memory_no_update
BEFORE UPDATE ON ai_prompt_attempt_memory
BEGIN
    SELECT RAISE(ABORT, 'ai_prompt_attempt_memory is immutable');
END;

CREATE TRIGGER ai_prompt_attempt_memory_no_delete
BEFORE DELETE ON ai_prompt_attempt_memory
BEGIN
    SELECT RAISE(ABORT, 'ai_prompt_attempt_memory is immutable');
END;

CREATE INDEX relationship_event_subject_time
    ON relationship_event(subject_user_id, occurred_at_ms DESC,
                          relationship_event_id DESC);
CREATE INDEX relationship_event_source
    ON relationship_event(source_event_id, subject_user_id);
CREATE INDEX relationship_event_reason_time
    ON relationship_event(reason_code, occurred_at_ms DESC);
CREATE INDEX ai_prompt_attempt_prepared
    ON ai_prompt_attempt(application_instance_id, prepared_at_ms, attempt_id)
    WHERE state = 'prepared';
CREATE INDEX ai_prompt_attempt_memory_cooldown
    ON ai_prompt_attempt_memory(memory_id, selected_at_ms DESC, attempt_id);
CREATE INDEX memory_prompt_candidates
    ON memory(status, visibility, sensitivity, expires_at_ms,
              last_used_at_ms, created_at_ms DESC);
