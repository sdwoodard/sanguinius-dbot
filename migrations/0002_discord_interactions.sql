CREATE TABLE pending_notice (
    notice_id TEXT PRIMARY KEY
        CHECK (length(notice_id) = 36
               AND substr(notice_id, 9, 1) = '-'
               AND substr(notice_id, 14, 1) = '-'
               AND substr(notice_id, 19, 1) = '-'
               AND substr(notice_id, 24, 1) = '-'
               AND length(replace(notice_id, '-', '')) = 32
               AND notice_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(notice_id, 15, 1) = '4'
               AND substr(notice_id, 20, 1) IN ('8', '9', 'a', 'b')),
    target_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    notice_type TEXT NOT NULL
        CHECK (length(notice_type) BETWEEN 1 AND 64
               AND notice_type NOT GLOB '*[^a-z0-9_.-]*'),
    payload_json TEXT NOT NULL
        CHECK (length(payload_json) BETWEEN 2 AND 8192
               AND json_valid(payload_json)),
    source_aggregate_type TEXT
        CHECK (source_aggregate_type IS NULL
               OR (length(source_aggregate_type) BETWEEN 1 AND 64
                   AND source_aggregate_type NOT GLOB '*[^a-z0-9_.-]*')),
    source_aggregate_id TEXT
        CHECK (source_aggregate_id IS NULL
               OR length(source_aggregate_id) BETWEEN 1 AND 128),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'opened', 'consumed', 'expired',
                         'cancelled')),
    expires_at_ms INTEGER
        CHECK (expires_at_ms IS NULL OR expires_at_ms >= created_at_ms),
    opened_at_ms INTEGER
        CHECK (opened_at_ms IS NULL OR opened_at_ms >= created_at_ms),
    consumed_at_ms INTEGER
        CHECK (consumed_at_ms IS NULL
               OR (opened_at_ms IS NOT NULL
                   AND consumed_at_ms >= opened_at_ms)),
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    opened_idempotency_key TEXT UNIQUE
        CHECK (opened_idempotency_key IS NULL
               OR length(opened_idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((source_aggregate_type IS NULL) = (source_aggregate_id IS NULL)),
    CHECK ((opened_at_ms IS NULL) = (opened_idempotency_key IS NULL)),
    CHECK ((state = 'pending'
            AND opened_at_ms IS NULL AND consumed_at_ms IS NULL)
           OR (state = 'opened'
               AND opened_at_ms IS NOT NULL AND consumed_at_ms IS NULL)
           OR (state = 'consumed'
               AND opened_at_ms IS NOT NULL AND consumed_at_ms IS NOT NULL)
           OR (state IN ('expired', 'cancelled')
               AND consumed_at_ms IS NULL))
) STRICT;

CREATE TABLE interaction_token (
    token_id TEXT PRIMARY KEY
        CHECK (length(token_id) = 36
               AND substr(token_id, 9, 1) = '-'
               AND substr(token_id, 14, 1) = '-'
               AND substr(token_id, 19, 1) = '-'
               AND substr(token_id, 24, 1) = '-'
               AND length(replace(token_id, '-', '')) = 32
               AND token_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(token_id, 15, 1) = '4'
               AND substr(token_id, 20, 1) IN ('8', '9', 'a', 'b')),
    token_version INTEGER NOT NULL CHECK (token_version = 1),
    interaction_kind TEXT NOT NULL
        CHECK (interaction_kind IN ('button', 'select', 'modal')),
    action TEXT NOT NULL
        CHECK (length(action) BETWEEN 1 AND 64
               AND action NOT GLOB '*[^a-z0-9_.-]*'),
    entity_type TEXT NOT NULL
        CHECK (length(entity_type) BETWEEN 1 AND 64
               AND entity_type NOT GLOB '*[^a-z0-9_.-]*'),
    entity_id TEXT NOT NULL CHECK (length(entity_id) BETWEEN 1 AND 128),
    expected_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL
        CHECK (length(guild_id) BETWEEN 1 AND 20
               AND guild_id NOT GLOB '*[^0-9]*'
               AND guild_id <> '0'
               AND (length(guild_id) = 1 OR substr(guild_id, 1, 1) <> '0')
               AND (length(guild_id) < 20
                    OR guild_id <= '18446744073709551615')),
    channel_id TEXT NOT NULL
        CHECK (length(channel_id) BETWEEN 1 AND 20
               AND channel_id NOT GLOB '*[^0-9]*'
               AND channel_id <> '0'
               AND (length(channel_id) = 1 OR substr(channel_id, 1, 1) <> '0')
               AND (length(channel_id) < 20
                    OR channel_id <= '18446744073709551615')),
    state TEXT NOT NULL CHECK (state IN ('active', 'used', 'cancelled')),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms >= created_at_ms),
    used_at_ms INTEGER CHECK (used_at_ms IS NULL OR used_at_ms >= created_at_ms),
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((state = 'used') = (used_at_ms IS NOT NULL))
) STRICT;

CREATE TABLE notice_reveal_attempt (
    idempotency_key TEXT PRIMARY KEY
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    interaction_kind TEXT NOT NULL
        CHECK (interaction_kind IN ('inbox', 'button')),
    target_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    notice_id TEXT
        REFERENCES pending_notice(notice_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    token_id TEXT
        REFERENCES interaction_token(token_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    result_kind TEXT NOT NULL
        CHECK (result_kind IN ('notice', 'no_pending_notice')),
    delivery_state TEXT NOT NULL
        CHECK (delivery_state IN ('prepared', 'delivered', 'failed')),
    reservation_expires_at_ms INTEGER NOT NULL
        CHECK (reservation_expires_at_ms >= created_at_ms),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    completed_at_ms INTEGER
        CHECK (completed_at_ms IS NULL OR completed_at_ms >= created_at_ms),
    CHECK ((result_kind = 'notice') = (notice_id IS NOT NULL)),
    CHECK ((interaction_kind = 'button') = (token_id IS NOT NULL)),
    CHECK (interaction_kind <> 'button' OR result_kind = 'notice'),
    CHECK ((delivery_state = 'prepared') = (completed_at_ms IS NULL))
) STRICT;

CREATE INDEX pending_notice_target_state_created
    ON pending_notice(target_user_id, state, created_at_ms);

CREATE INDEX pending_notice_active_expiry
    ON pending_notice(expires_at_ms)
    WHERE state IN ('pending', 'opened') AND expires_at_ms IS NOT NULL;

CREATE INDEX interaction_token_active_expiry
    ON interaction_token(expires_at_ms)
    WHERE state = 'active';

CREATE INDEX interaction_token_entity
    ON interaction_token(entity_type, entity_id, action);

CREATE INDEX notice_reveal_attempt_reservation
    ON notice_reveal_attempt(notice_id, reservation_expires_at_ms)
    WHERE delivery_state = 'prepared' AND notice_id IS NOT NULL;
