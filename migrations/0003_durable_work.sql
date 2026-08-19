CREATE TABLE event_journal (
    event_id TEXT PRIMARY KEY
        CHECK (length(event_id) = 36
               AND substr(event_id, 9, 1) = '-'
               AND substr(event_id, 14, 1) = '-'
               AND substr(event_id, 19, 1) = '-'
               AND substr(event_id, 24, 1) = '-'
               AND length(replace(event_id, '-', '')) = 32
               AND event_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(event_id, 15, 1) = '4'
               AND substr(event_id, 20, 1) IN ('8', '9', 'a', 'b')),
    event_type TEXT NOT NULL
        CHECK (length(event_type) BETWEEN 1 AND 96
               AND event_type NOT GLOB '*[^a-z0-9_.-]*'),
    aggregate_type TEXT NOT NULL
        CHECK (length(aggregate_type) BETWEEN 1 AND 64
               AND aggregate_type NOT GLOB '*[^a-z0-9_.-]*'),
    aggregate_id TEXT NOT NULL CHECK (length(aggregate_id) BETWEEN 1 AND 128),
    actor_user_id TEXT
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL
        REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT
        CHECK (channel_id IS NULL
               OR (length(channel_id) BETWEEN 1 AND 20
                   AND channel_id NOT GLOB '*[^0-9]*'
                   AND channel_id <> '0'
                   AND (length(channel_id) = 1
                        OR substr(channel_id, 1, 1) <> '0')
                   AND (length(channel_id) < 20
                        OR channel_id <= '18446744073709551615'))),
    source_message_id TEXT
        CHECK (source_message_id IS NULL
               OR (length(source_message_id) BETWEEN 1 AND 20
                   AND source_message_id NOT GLOB '*[^0-9]*'
                   AND source_message_id <> '0'
                   AND (length(source_message_id) = 1
                        OR substr(source_message_id, 1, 1) <> '0')
                   AND (length(source_message_id) < 20
                        OR source_message_id <= '18446744073709551615'))),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    recorded_at_ms INTEGER NOT NULL CHECK (recorded_at_ms >= 0),
    correlation_id TEXT NOT NULL CHECK (length(correlation_id) BETWEEN 1 AND 160),
    causation_id TEXT
        REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    payload_json TEXT NOT NULL
        CHECK (length(payload_json) BETWEEN 2 AND 16384
               AND json_valid(payload_json))
) STRICT;

CREATE TRIGGER event_journal_no_update
BEFORE UPDATE ON event_journal
BEGIN
    SELECT RAISE(ABORT, 'event_journal is append-only');
END;

CREATE TRIGGER event_journal_no_delete
BEFORE DELETE ON event_journal
BEGIN
    SELECT RAISE(ABORT, 'event_journal is append-only');
END;

CREATE TABLE scheduled_job (
    job_id TEXT PRIMARY KEY
        CHECK (length(job_id) = 36
               AND substr(job_id, 9, 1) = '-'
               AND substr(job_id, 14, 1) = '-'
               AND substr(job_id, 19, 1) = '-'
               AND substr(job_id, 24, 1) = '-'
               AND length(replace(job_id, '-', '')) = 32
               AND job_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(job_id, 15, 1) = '4'
               AND substr(job_id, 20, 1) IN ('8', '9', 'a', 'b')),
    job_type TEXT NOT NULL
        CHECK (length(job_type) BETWEEN 1 AND 96
               AND job_type NOT GLOB '*[^a-z0-9_.-]*'),
    aggregate_type TEXT
        CHECK (aggregate_type IS NULL
               OR (length(aggregate_type) BETWEEN 1 AND 64
                   AND aggregate_type NOT GLOB '*[^a-z0-9_.-]*')),
    aggregate_id TEXT
        CHECK (aggregate_id IS NULL OR length(aggregate_id) BETWEEN 1 AND 128),
    payload_json TEXT NOT NULL
        CHECK (length(payload_json) BETWEEN 2 AND 16384
               AND json_valid(payload_json)),
    due_at_ms INTEGER NOT NULL CHECK (due_at_ms >= 0),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'claimed', 'completed', 'cancelled', 'dead')),
    attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    max_attempts INTEGER NOT NULL CHECK (max_attempts BETWEEN 1 AND 20),
    lease_owner TEXT CHECK (lease_owner IS NULL OR length(lease_owner) BETWEEN 1 AND 128),
    lease_token TEXT
        CHECK (lease_token IS NULL
               OR (length(lease_token) = 36
                   AND substr(lease_token, 9, 1) = '-'
                   AND substr(lease_token, 14, 1) = '-'
                   AND substr(lease_token, 19, 1) = '-'
                   AND substr(lease_token, 24, 1) = '-'
                   AND length(replace(lease_token, '-', '')) = 32
                   AND lease_token NOT GLOB '*[^0-9a-f-]*'
                   AND substr(lease_token, 15, 1) = '4'
                   AND substr(lease_token, 20, 1) IN ('8', '9', 'a', 'b'))),
    lease_until_ms INTEGER CHECK (lease_until_ms IS NULL OR lease_until_ms >= 0),
    last_error_code TEXT
        CHECK (last_error_code IS NULL
               OR (length(last_error_code) BETWEEN 1 AND 96
                   AND last_error_code NOT GLOB '*[^a-z0-9_.-]*')),
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms),
    completed_at_ms INTEGER
        CHECK (completed_at_ms IS NULL OR completed_at_ms >= created_at_ms),
    terminal_at_ms INTEGER
        CHECK (terminal_at_ms IS NULL OR terminal_at_ms >= created_at_ms),
    CHECK ((aggregate_type IS NULL) = (aggregate_id IS NULL)),
    CHECK ((state = 'claimed') =
           (lease_owner IS NOT NULL AND lease_token IS NOT NULL
            AND lease_until_ms IS NOT NULL)),
    CHECK ((state = 'completed') = (completed_at_ms IS NOT NULL)),
    CHECK ((state IN ('completed', 'cancelled', 'dead')) =
           (terminal_at_ms IS NOT NULL))
) STRICT;

CREATE TABLE outbox_message (
    outbox_id TEXT PRIMARY KEY
        CHECK (length(outbox_id) = 36
               AND substr(outbox_id, 9, 1) = '-'
               AND substr(outbox_id, 14, 1) = '-'
               AND substr(outbox_id, 19, 1) = '-'
               AND substr(outbox_id, 24, 1) = '-'
               AND length(replace(outbox_id, '-', '')) = 32
               AND outbox_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(outbox_id, 15, 1) = '4'
               AND substr(outbox_id, 20, 1) IN ('8', '9', 'a', 'b')),
    kind TEXT NOT NULL
        CHECK (length(kind) BETWEEN 1 AND 96
               AND kind NOT GLOB '*[^a-z0-9_.-]*'),
    aggregate_type TEXT
        CHECK (aggregate_type IS NULL
               OR (length(aggregate_type) BETWEEN 1 AND 64
                   AND aggregate_type NOT GLOB '*[^a-z0-9_.-]*')),
    aggregate_id TEXT
        CHECK (aggregate_id IS NULL OR length(aggregate_id) BETWEEN 1 AND 128),
    target_guild_id TEXT NOT NULL
        REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    target_channel_id TEXT NOT NULL
        CHECK (length(target_channel_id) BETWEEN 1 AND 20
               AND target_channel_id NOT GLOB '*[^0-9]*'
               AND target_channel_id <> '0'
               AND (length(target_channel_id) = 1
                    OR substr(target_channel_id, 1, 1) <> '0')
               AND (length(target_channel_id) < 20
                    OR target_channel_id <= '18446744073709551615')),
    target_user_id TEXT
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    payload_json TEXT NOT NULL
        CHECK (length(payload_json) BETWEEN 2 AND 16384
               AND json_valid(payload_json)),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'claimed', 'delivered', 'failed', 'dead',
                         'cancelled')),
    attempt_count INTEGER NOT NULL DEFAULT 0 CHECK (attempt_count >= 0),
    max_attempts INTEGER NOT NULL CHECK (max_attempts BETWEEN 1 AND 20),
    lease_owner TEXT CHECK (lease_owner IS NULL OR length(lease_owner) BETWEEN 1 AND 128),
    lease_token TEXT
        CHECK (lease_token IS NULL
               OR (length(lease_token) = 36
                   AND substr(lease_token, 9, 1) = '-'
                   AND substr(lease_token, 14, 1) = '-'
                   AND substr(lease_token, 19, 1) = '-'
                   AND substr(lease_token, 24, 1) = '-'
                   AND length(replace(lease_token, '-', '')) = 32
                   AND lease_token NOT GLOB '*[^0-9a-f-]*'
                   AND substr(lease_token, 15, 1) = '4'
                   AND substr(lease_token, 20, 1) IN ('8', '9', 'a', 'b'))),
    lease_until_ms INTEGER CHECK (lease_until_ms IS NULL OR lease_until_ms >= 0),
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    provider_nonce TEXT NOT NULL UNIQUE
        CHECK (length(provider_nonce) = 25
               AND provider_nonce NOT GLOB '*[^0-9a-f]*'),
    provider_message_id TEXT
        CHECK (provider_message_id IS NULL
               OR (length(provider_message_id) BETWEEN 1 AND 20
                   AND provider_message_id NOT GLOB '*[^0-9]*'
                   AND provider_message_id <> '0'
                   AND (length(provider_message_id) = 1
                        OR substr(provider_message_id, 1, 1) <> '0')
                   AND (length(provider_message_id) < 20
                        OR provider_message_id <= '18446744073709551615'))),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    available_at_ms INTEGER NOT NULL CHECK (available_at_ms >= created_at_ms),
    first_attempt_at_ms INTEGER
        CHECK (first_attempt_at_ms IS NULL OR first_attempt_at_ms >= created_at_ms),
    first_attempt_elapsed_ms INTEGER
        CHECK (first_attempt_elapsed_ms IS NULL OR first_attempt_elapsed_ms >= 0),
    first_attempt_boot_id TEXT
        CHECK (first_attempt_boot_id IS NULL
               OR (length(first_attempt_boot_id) BETWEEN 1 AND 64
                   AND first_attempt_boot_id NOT GLOB '*[^a-z0-9-]*')),
    submission_started_at_ms INTEGER
        CHECK (submission_started_at_ms IS NULL
               OR submission_started_at_ms >= created_at_ms),
    delivered_at_ms INTEGER
        CHECK (delivered_at_ms IS NULL OR delivered_at_ms >= created_at_ms),
    terminal_at_ms INTEGER
        CHECK (terminal_at_ms IS NULL OR terminal_at_ms >= created_at_ms),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms),
    last_error_code TEXT
        CHECK (last_error_code IS NULL
               OR (length(last_error_code) BETWEEN 1 AND 96
                   AND last_error_code NOT GLOB '*[^a-z0-9_.-]*')),
    CHECK ((aggregate_type IS NULL) = (aggregate_id IS NULL)),
    CHECK ((state = 'claimed') =
           (lease_owner IS NOT NULL AND lease_token IS NOT NULL
            AND lease_until_ms IS NOT NULL)),
    CHECK (submission_started_at_ms IS NULL OR state = 'claimed'),
    CHECK (submission_started_at_ms IS NULL OR first_attempt_at_ms IS NOT NULL),
    CHECK ((first_attempt_at_ms IS NULL) =
           (first_attempt_elapsed_ms IS NULL)),
    CHECK ((first_attempt_at_ms IS NULL) =
           (first_attempt_boot_id IS NULL)),
    CHECK ((state = 'delivered') = (delivered_at_ms IS NOT NULL)),
    CHECK ((state IN ('delivered', 'failed', 'dead', 'cancelled')) =
           (terminal_at_ms IS NOT NULL)),
    CHECK (provider_message_id IS NULL OR state = 'delivered')
) STRICT;

CREATE INDEX event_journal_recent
    ON event_journal(recorded_at_ms DESC, event_id DESC);
CREATE INDEX event_journal_type_time
    ON event_journal(event_type, occurred_at_ms DESC);
CREATE INDEX event_journal_aggregate
    ON event_journal(aggregate_type, aggregate_id, occurred_at_ms DESC);

CREATE INDEX scheduled_job_pending_due
    ON scheduled_job(due_at_ms, job_id) WHERE state = 'pending';
CREATE INDEX scheduled_job_expired_lease
    ON scheduled_job(lease_until_ms, job_id) WHERE state = 'claimed';
CREATE INDEX scheduled_job_dead_recent
    ON scheduled_job(updated_at_ms DESC, job_id DESC) WHERE state = 'dead';

CREATE INDEX outbox_pending_available
    ON outbox_message(available_at_ms, outbox_id) WHERE state = 'pending';
CREATE INDEX outbox_expired_lease
    ON outbox_message(lease_until_ms, outbox_id) WHERE state = 'claimed';
CREATE INDEX outbox_failed_dead_recent
    ON outbox_message(updated_at_ms DESC, outbox_id DESC)
    WHERE state IN ('failed', 'dead');
