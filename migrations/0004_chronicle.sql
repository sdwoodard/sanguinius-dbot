ALTER TABLE interaction_token
ADD COLUMN expected_entity_revision INTEGER
    CHECK (expected_entity_revision IS NULL OR expected_entity_revision > 0);

CREATE TABLE chronicle_entry (
    entry_id TEXT PRIMARY KEY
        CHECK (length(entry_id) = 36
               AND substr(entry_id, 9, 1) = '-'
               AND substr(entry_id, 14, 1) = '-'
               AND substr(entry_id, 19, 1) = '-'
               AND substr(entry_id, 24, 1) = '-'
               AND length(replace(entry_id, '-', '')) = 32
               AND entry_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(entry_id, 15, 1) = '4'
               AND substr(entry_id, 20, 1) IN ('8', '9', 'a', 'b')),
    entry_type TEXT NOT NULL
        CHECK (entry_type IN ('quote', 'deed', 'prediction', 'incident',
                              'custom')),
    title TEXT NOT NULL
        CHECK (length(CAST(title AS BLOB)) BETWEEN 1 AND 100
               AND length(trim(title, char(9) || char(10) || char(11) ||
                                      char(12) || char(13) || ' ')) > 0),
    body TEXT NOT NULL
        CHECK (length(CAST(body AS BLOB)) BETWEEN 1 AND 1000
               AND length(trim(body, char(9) || char(10) || char(11) ||
                                     char(12) || char(13) || ' ')) > 0),
    visibility TEXT NOT NULL
        CHECK (visibility IN ('shared', 'participant_only')),
    status TEXT NOT NULL
        CHECK (status IN ('proposed', 'canon', 'retracted')),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    created_by_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    submitted_at_ms INTEGER
        CHECK (submitted_at_ms IS NULL OR submitted_at_ms >= created_at_ms),
    approved_at_ms INTEGER
        CHECK (approved_at_ms IS NULL OR approved_at_ms >= created_at_ms),
    approved_by_user_id TEXT
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    retracted_at_ms INTEGER
        CHECK (retracted_at_ms IS NULL OR retracted_at_ms >= created_at_ms),
    retracted_by_user_id TEXT
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_guild_id TEXT NOT NULL
        REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_channel_id TEXT NOT NULL
        CHECK (length(source_channel_id) BETWEEN 1 AND 20
               AND source_channel_id NOT GLOB '*[^0-9]*'
               AND source_channel_id <> '0'
               AND (length(source_channel_id) = 1
                    OR substr(source_channel_id, 1, 1) <> '0')
               AND (length(source_channel_id) < 20
                    OR source_channel_id <= '18446744073709551615')),
    source_message_id TEXT NOT NULL
        CHECK (length(source_message_id) BETWEEN 1 AND 20
               AND source_message_id NOT GLOB '*[^0-9]*'
               AND source_message_id <> '0'
               AND (length(source_message_id) = 1
                    OR substr(source_message_id, 1, 1) <> '0')
               AND (length(source_message_id) < 20
                    OR source_message_id <= '18446744073709551615')),
    source_author_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_text TEXT NOT NULL
        CHECK (length(CAST(source_text AS BLOB)) <= 2000),
    source_text_truncated INTEGER NOT NULL DEFAULT 0
        CHECK (source_text_truncated IN (0, 1)),
    source_attachment_count INTEGER NOT NULL DEFAULT 0
        CHECK (source_attachment_count BETWEEN 0 AND 10),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    UNIQUE (source_guild_id, source_channel_id, source_message_id),
    CHECK ((approved_at_ms IS NULL) = (approved_by_user_id IS NULL)),
    CHECK ((retracted_at_ms IS NULL) = (retracted_by_user_id IS NULL)),
    CHECK ((status = 'proposed'
            AND approved_at_ms IS NULL AND retracted_at_ms IS NULL)
           OR (status = 'canon'
               AND approved_at_ms IS NOT NULL AND retracted_at_ms IS NULL)
           OR (status = 'retracted' AND retracted_at_ms IS NOT NULL))
) STRICT;

CREATE TABLE chronicle_participant (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    role TEXT NOT NULL
        CHECK (role IN ('proposer', 'source_author', 'subject')),
    PRIMARY KEY (entry_id, user_id, role)
) STRICT;

CREATE TABLE chronicle_tag (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    tag TEXT NOT NULL
        CHECK (length(CAST(tag AS BLOB)) BETWEEN 1 AND 32
               AND tag = lower(tag)
               AND tag NOT GLOB '*[^a-z0-9_-]*'),
    PRIMARY KEY (entry_id, tag)
) STRICT;

CREATE TABLE chronicle_attachment (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 9),
    attachment_id TEXT NOT NULL
        CHECK (length(attachment_id) BETWEEN 1 AND 20
               AND attachment_id NOT GLOB '*[^0-9]*'
               AND attachment_id <> '0'
               AND (length(attachment_id) = 1
                    OR substr(attachment_id, 1, 1) <> '0')
               AND (length(attachment_id) < 20
                    OR attachment_id <= '18446744073709551615')),
    filename TEXT NOT NULL
        CHECK (length(CAST(filename AS BLOB)) BETWEEN 1 AND 255),
    content_type TEXT
        CHECK (content_type IS NULL
               OR length(CAST(content_type AS BLOB)) BETWEEN 1 AND 127),
    byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
    width INTEGER CHECK (width IS NULL OR width BETWEEN 1 AND 4294967295),
    height INTEGER CHECK (height IS NULL OR height BETWEEN 1 AND 4294967295),
    is_ephemeral INTEGER NOT NULL CHECK (is_ephemeral IN (0, 1)),
    is_spoiler INTEGER NOT NULL CHECK (is_spoiler IN (0, 1)),
    PRIMARY KEY (entry_id, position),
    UNIQUE (entry_id, attachment_id)
) STRICT;

CREATE TABLE chronicle_approval (
    approval_id TEXT PRIMARY KEY
        CHECK (length(approval_id) = 36
               AND substr(approval_id, 9, 1) = '-'
               AND substr(approval_id, 14, 1) = '-'
               AND substr(approval_id, 19, 1) = '-'
               AND substr(approval_id, 24, 1) = '-'
               AND length(replace(approval_id, '-', '')) = 32
               AND approval_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(approval_id, 15, 1) = '4'
               AND substr(approval_id, 20, 1) IN ('8', '9', 'a', 'b')),
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    reviewer_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    approval_role TEXT NOT NULL
        CHECK (approval_role IN ('proposer', 'participant', 'owner',
                                 'owner_stale', 'owner_test')),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'approved', 'declined', 'cancelled')),
    entry_revision INTEGER NOT NULL CHECK (entry_revision > 0),
    notice_id TEXT
        REFERENCES pending_notice(notice_id) ON UPDATE RESTRICT ON DELETE SET NULL,
    requested_at_ms INTEGER NOT NULL CHECK (requested_at_ms >= 0),
    acted_at_ms INTEGER
        CHECK (acted_at_ms IS NULL OR acted_at_ms >= requested_at_ms),
    interaction_idempotency_key TEXT UNIQUE
        CHECK (interaction_idempotency_key IS NULL
               OR length(interaction_idempotency_key) BETWEEN 1 AND 160),
    UNIQUE (entry_id, reviewer_user_id, approval_role),
    CHECK ((state = 'pending') = (acted_at_ms IS NULL))
) STRICT;

CREATE TABLE memory (
    memory_id TEXT PRIMARY KEY
        CHECK (length(memory_id) = 36
               AND substr(memory_id, 9, 1) = '-'
               AND substr(memory_id, 14, 1) = '-'
               AND substr(memory_id, 19, 1) = '-'
               AND substr(memory_id, 24, 1) = '-'
               AND length(replace(memory_id, '-', '')) = 32
               AND memory_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(memory_id, 15, 1) = '4'
               AND substr(memory_id, 20, 1) IN ('8', '9', 'a', 'b')),
    memory_type TEXT NOT NULL CHECK (memory_type = 'explicit'),
    text TEXT NOT NULL
        CHECK (length(CAST(text AS BLOB)) BETWEEN 1 AND 500
               AND length(trim(text, char(9) || char(10) || char(11) ||
                                     char(12) || char(13) || ' ')) > 0),
    visibility TEXT NOT NULL CHECK (visibility IN ('shared', 'self_only')),
    sensitivity TEXT NOT NULL
        CHECK (sensitivity IN ('ordinary', 'personal', 'sensitive')),
    status TEXT NOT NULL
        CHECK (status IN ('confirmed', 'retracted', 'expired')),
    confidence_basis TEXT NOT NULL CHECK (confidence_basis = 'user_confirmed'),
    source_event_id TEXT NOT NULL
        REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_by_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    confirmed_by_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    confirmed_at_ms INTEGER NOT NULL CHECK (confirmed_at_ms >= created_at_ms),
    expires_at_ms INTEGER CHECK (expires_at_ms IS NULL OR expires_at_ms > confirmed_at_ms),
    retracted_at_ms INTEGER
        CHECK (retracted_at_ms IS NULL OR retracted_at_ms >= confirmed_at_ms),
    expired_at_ms INTEGER
        CHECK (expired_at_ms IS NULL OR expired_at_ms >= confirmed_at_ms),
    last_used_at_ms INTEGER
        CHECK (last_used_at_ms IS NULL OR last_used_at_ms >= confirmed_at_ms),
    use_count INTEGER NOT NULL DEFAULT 0 CHECK (use_count >= 0),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    creation_idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(creation_idempotency_key) BETWEEN 1 AND 160),
    CHECK (visibility = 'self_only' OR sensitivity = 'ordinary'),
    CHECK ((status = 'confirmed'
            AND retracted_at_ms IS NULL AND expired_at_ms IS NULL)
           OR (status = 'retracted'
               AND retracted_at_ms IS NOT NULL AND expired_at_ms IS NULL)
           OR (status = 'expired'
               AND expired_at_ms IS NOT NULL AND retracted_at_ms IS NULL))
) STRICT;

CREATE TABLE memory_subject (
    memory_id TEXT NOT NULL
        REFERENCES memory(memory_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    subject_type TEXT NOT NULL CHECK (subject_type IN ('user', 'guild', 'topic', 'game')),
    subject_id TEXT NOT NULL CHECK (length(subject_id) BETWEEN 1 AND 128),
    PRIMARY KEY (memory_id, subject_type, subject_id)
) STRICT;

CREATE INDEX chronicle_entry_status_time
    ON chronicle_entry(status, occurred_at_ms DESC, entry_id DESC);
CREATE INDEX chronicle_participant_user
    ON chronicle_participant(user_id, entry_id);
CREATE INDEX chronicle_tag_lookup ON chronicle_tag(tag, entry_id);
CREATE TRIGGER chronicle_tag_maximum_before_insert
BEFORE INSERT ON chronicle_tag
WHEN (SELECT count(*) FROM chronicle_tag WHERE entry_id = NEW.entry_id) >= 5
BEGIN
    SELECT RAISE(ABORT, 'chronicle entry tag limit exceeded');
END;
CREATE INDEX chronicle_approval_pending
    ON chronicle_approval(reviewer_user_id, requested_at_ms, approval_id)
    WHERE state = 'pending';
CREATE INDEX memory_status_expiry
    ON memory(status, expires_at_ms, memory_id);
CREATE INDEX memory_subject_lookup
    ON memory_subject(subject_type, subject_id, memory_id);
CREATE INDEX outbox_chronicle_aggregate_sequence
    ON outbox_message(aggregate_id, created_at_ms, state)
    WHERE aggregate_type = 'chronicle_entry'
      AND state IN ('pending', 'claimed');

-- T-030 records server-wide consent. Import it only for identities already
-- represented when this migration runs; user_preference keeps its conservative
-- default for identities first observed later.
UPDATE user_preference
SET chronicle_opt_in = 1
WHERE chronicle_opt_in = 0;
