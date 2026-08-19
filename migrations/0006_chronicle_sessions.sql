-- Milestone 8 requires FTS5. Creating this table is also the migration
-- preflight: SQLite reports a schema error and rolls the migration back when
-- the deployed library was built without FTS5.
CREATE VIRTUAL TABLE chronicle_entry_fts_probe USING fts5(value);
DROP TABLE chronicle_entry_fts_probe;

ALTER TABLE user_preference
ADD COLUMN anniversary_reminders_enabled INTEGER NOT NULL DEFAULT 1
    CHECK (anniversary_reminders_enabled IN (0, 1));

-- chronicle_entry and its direct children are rebuilt together so system
-- entries can omit a Discord message source and the new entry/participant
-- kinds are enforced by SQLite. Existing M7 rows remain Discord-sourced.
CREATE TABLE chronicle_entry_new (
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
                              'custom', 'session_summary', 'title_award')),
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
    source_message_id TEXT
        CHECK (source_message_id IS NULL
               OR (length(source_message_id) BETWEEN 1 AND 20
                   AND source_message_id NOT GLOB '*[^0-9]*'
                   AND source_message_id <> '0'
                   AND (length(source_message_id) = 1
                        OR substr(source_message_id, 1, 1) <> '0')
                   AND (length(source_message_id) < 20
                        OR source_message_id <= '18446744073709551615'))),
    source_author_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_text TEXT NOT NULL
        CHECK (length(CAST(source_text AS BLOB)) <= 2000),
    source_text_truncated INTEGER NOT NULL DEFAULT 0
        CHECK (source_text_truncated IN (0, 1)),
    source_attachment_count INTEGER NOT NULL DEFAULT 0
        CHECK (source_attachment_count BETWEEN 0 AND 10),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    source_kind TEXT NOT NULL DEFAULT 'discord_message'
        CHECK (source_kind IN ('discord_message', 'session_summary',
                               'title_award')),
    UNIQUE (source_guild_id, source_channel_id, source_message_id),
    CHECK ((approved_at_ms IS NULL) = (approved_by_user_id IS NULL)),
    CHECK ((retracted_at_ms IS NULL) = (retracted_by_user_id IS NULL)),
    CHECK ((status = 'proposed'
            AND approved_at_ms IS NULL AND retracted_at_ms IS NULL)
           OR (status = 'canon'
               AND approved_at_ms IS NOT NULL AND retracted_at_ms IS NULL)
           OR (status = 'retracted' AND retracted_at_ms IS NOT NULL)),
    CHECK ((source_kind = 'discord_message' AND source_message_id IS NOT NULL
            AND entry_type NOT IN ('session_summary', 'title_award'))
           OR (source_kind = 'session_summary'
               AND source_message_id IS NULL
               AND entry_type = 'session_summary')
           OR (source_kind = 'title_award'
               AND source_message_id IS NULL
               AND entry_type = 'title_award'))
) STRICT;

CREATE TABLE chronicle_participant_new (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry_new(entry_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    role TEXT NOT NULL
        CHECK (role IN ('proposer', 'source_author', 'subject',
                        'session_participant', 'title_recipient')),
    PRIMARY KEY (entry_id, user_id, role)
) STRICT;

CREATE TABLE chronicle_tag_new (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry_new(entry_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    tag TEXT NOT NULL
        CHECK (length(CAST(tag AS BLOB)) BETWEEN 1 AND 32
               AND tag = lower(tag)
               AND tag NOT GLOB '*[^a-z0-9_-]*'),
    PRIMARY KEY (entry_id, tag)
) STRICT;

CREATE TABLE chronicle_attachment_new (
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry_new(entry_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 9),
    attachment_id TEXT NOT NULL
        CHECK (length(attachment_id) BETWEEN 1 AND 20
               AND attachment_id NOT GLOB '*[^0-9]*'
               AND attachment_id <> '0'
               AND (length(attachment_id) = 1
                    OR substr(attachment_id, 1, 1) <> '0')
               AND (length(attachment_id) < 20
                    OR attachment_id <= '18446744073709551615')),
    filename TEXT NOT NULL CHECK (length(CAST(filename AS BLOB)) BETWEEN 1 AND 255),
    content_type TEXT CHECK (content_type IS NULL OR length(CAST(content_type AS BLOB)) BETWEEN 1 AND 127),
    byte_size INTEGER NOT NULL CHECK (byte_size >= 0),
    width INTEGER CHECK (width IS NULL OR width BETWEEN 1 AND 4294967295),
    height INTEGER CHECK (height IS NULL OR height BETWEEN 1 AND 4294967295),
    is_ephemeral INTEGER NOT NULL CHECK (is_ephemeral IN (0, 1)),
    is_spoiler INTEGER NOT NULL CHECK (is_spoiler IN (0, 1)),
    PRIMARY KEY (entry_id, position),
    UNIQUE (entry_id, attachment_id)
) STRICT;

CREATE TABLE chronicle_approval_new (
    approval_id TEXT PRIMARY KEY,
    entry_id TEXT NOT NULL
        REFERENCES chronicle_entry_new(entry_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    reviewer_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    approval_role TEXT NOT NULL
        CHECK (approval_role IN ('proposer', 'participant', 'owner',
                                 'owner_stale', 'owner_test')),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'approved', 'declined', 'cancelled')),
    entry_revision INTEGER NOT NULL CHECK (entry_revision > 0),
    notice_id TEXT REFERENCES pending_notice(notice_id)
        ON UPDATE RESTRICT ON DELETE SET NULL,
    requested_at_ms INTEGER NOT NULL CHECK (requested_at_ms >= 0),
    acted_at_ms INTEGER CHECK (acted_at_ms IS NULL OR acted_at_ms >= requested_at_ms),
    interaction_idempotency_key TEXT UNIQUE
        CHECK (interaction_idempotency_key IS NULL OR length(interaction_idempotency_key) BETWEEN 1 AND 160),
    UNIQUE (entry_id, reviewer_user_id, approval_role),
    CHECK ((state = 'pending') = (acted_at_ms IS NULL))
) STRICT;

INSERT INTO chronicle_entry_new
SELECT entry_id,entry_type,title,body,visibility,status,occurred_at_ms,
       created_at_ms,created_by_user_id,submitted_at_ms,approved_at_ms,
       approved_by_user_id,retracted_at_ms,retracted_by_user_id,
       source_guild_id,source_channel_id,source_message_id,
       source_author_user_id,source_text,source_text_truncated,
       source_attachment_count,revision,'discord_message'
FROM chronicle_entry;
INSERT INTO chronicle_participant_new SELECT * FROM chronicle_participant;
INSERT INTO chronicle_tag_new SELECT * FROM chronicle_tag;
INSERT INTO chronicle_attachment_new SELECT * FROM chronicle_attachment;
INSERT INTO chronicle_approval_new SELECT * FROM chronicle_approval;

CREATE TEMP TABLE chronicle_rebuild_verify (
    row_delta INTEGER NOT NULL CHECK (row_delta = 0)
);
INSERT INTO chronicle_rebuild_verify
SELECT (SELECT count(*) FROM chronicle_entry_new) -
       (SELECT count(*) FROM chronicle_entry);
DROP TABLE chronicle_rebuild_verify;

DROP TABLE chronicle_approval;
DROP TABLE chronicle_attachment;
DROP TABLE chronicle_tag;
DROP TABLE chronicle_participant;
DROP TABLE chronicle_entry;
ALTER TABLE chronicle_entry_new RENAME TO chronicle_entry;
ALTER TABLE chronicle_participant_new RENAME TO chronicle_participant;
ALTER TABLE chronicle_tag_new RENAME TO chronicle_tag;
ALTER TABLE chronicle_attachment_new RENAME TO chronicle_attachment;
ALTER TABLE chronicle_approval_new RENAME TO chronicle_approval;

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

CREATE TABLE chronicle_session (
    session_id TEXT PRIMARY KEY,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL,
    opened_by_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    state TEXT NOT NULL CHECK (state IN ('open', 'closing', 'closed', 'abandoned')),
    opened_at_ms INTEGER NOT NULL CHECK (opened_at_ms >= 0),
    closing_at_ms INTEGER CHECK (closing_at_ms IS NULL OR closing_at_ms >= opened_at_ms),
    closed_at_ms INTEGER CHECK (closed_at_ms IS NULL OR closed_at_ms >= opened_at_ms),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    start_idempotency_key TEXT NOT NULL UNIQUE,
    close_idempotency_key TEXT UNIQUE,
    CHECK ((state = 'open' AND closing_at_ms IS NULL AND closed_at_ms IS NULL)
           OR (state = 'closing' AND closing_at_ms IS NOT NULL AND closed_at_ms IS NULL)
           OR (state IN ('closed', 'abandoned') AND closed_at_ms IS NOT NULL))
) STRICT;
CREATE UNIQUE INDEX chronicle_session_one_active_guild
    ON chronicle_session(guild_id)
    WHERE state IN ('open', 'closing');
CREATE INDEX chronicle_session_recent
    ON chronicle_session(guild_id, opened_at_ms DESC, session_id DESC);

CREATE TABLE chronicle_session_participant (
    session_id TEXT NOT NULL REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    joined_at_ms INTEGER NOT NULL CHECK (joined_at_ms >= 0),
    PRIMARY KEY (session_id, user_id)
) STRICT;

CREATE TABLE chronicle_session_event (
    session_id TEXT NOT NULL REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    event_id TEXT NOT NULL REFERENCES event_journal(event_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    linked_at_ms INTEGER NOT NULL CHECK (linked_at_ms >= 0),
    PRIMARY KEY (session_id, event_id)
) STRICT;

CREATE TABLE chronicle_session_entry (
    session_id TEXT NOT NULL REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    linked_at_ms INTEGER NOT NULL CHECK (linked_at_ms >= 0),
    PRIMARY KEY (session_id, entry_id)
) STRICT;

CREATE TRIGGER chronicle_session_link_event_after_insert
AFTER INSERT ON event_journal
WHEN NEW.event_type GLOB 'chronicle.*'
BEGIN
    INSERT OR IGNORE INTO chronicle_session_event(session_id,event_id,linked_at_ms)
    SELECT session_id,NEW.event_id,NEW.recorded_at_ms
    FROM chronicle_session
    WHERE guild_id=NEW.guild_id AND state='open';
    INSERT OR IGNORE INTO chronicle_session_participant(session_id,user_id,joined_at_ms)
    SELECT session_id,NEW.actor_user_id,NEW.recorded_at_ms
    FROM chronicle_session s
    JOIN user_preference p ON p.user_id=NEW.actor_user_id
    WHERE s.guild_id=NEW.guild_id AND s.state='open'
      AND NEW.actor_user_id IS NOT NULL AND p.chronicle_opt_in=1;
END;

CREATE TRIGGER chronicle_session_link_entry_after_insert
AFTER INSERT ON chronicle_entry
BEGIN
    INSERT OR IGNORE INTO chronicle_session_entry(session_id,entry_id,linked_at_ms)
    SELECT session_id,NEW.entry_id,NEW.created_at_ms
    FROM chronicle_session
    WHERE guild_id=NEW.source_guild_id AND state='open';
END;

CREATE TRIGGER chronicle_session_bound_event_after_insert
AFTER INSERT ON chronicle_session_event
BEGIN
    DELETE FROM chronicle_session_event
    WHERE session_id=NEW.session_id AND event_id IN (
        SELECT event_id FROM chronicle_session_event
        WHERE session_id=NEW.session_id
        ORDER BY linked_at_ms DESC,event_id DESC
        LIMIT -1 OFFSET 200
    );
END;

CREATE TRIGGER chronicle_session_bound_entry_after_insert
AFTER INSERT ON chronicle_session_entry
BEGIN
    DELETE FROM chronicle_session_entry
    WHERE session_id=NEW.session_id AND entry_id IN (
        SELECT se.entry_id FROM chronicle_session_entry se
        JOIN chronicle_entry e ON e.entry_id=se.entry_id
        WHERE se.session_id=NEW.session_id
        ORDER BY CASE WHEN e.status='canon' AND e.visibility='shared'
                      THEN 0 ELSE 1 END,
                 se.linked_at_ms DESC,se.entry_id DESC
        LIMIT -1 OFFSET 50
    );
END;

CREATE TRIGGER chronicle_session_relink_eligible_entry_after_update
AFTER UPDATE OF status,visibility ON chronicle_entry
WHEN NEW.status='canon' AND NEW.visibility='shared'
BEGIN
    INSERT OR IGNORE INTO chronicle_session_entry(session_id,entry_id,linked_at_ms)
    SELECT session_id,NEW.entry_id,coalesce(NEW.approved_at_ms,NEW.created_at_ms)
    FROM chronicle_session
    WHERE guild_id=NEW.source_guild_id AND state='open';
    INSERT OR IGNORE INTO chronicle_session_participant(session_id,user_id,joined_at_ms)
    SELECT se.session_id,cp.user_id,
           coalesce(NEW.approved_at_ms,NEW.created_at_ms)
    FROM chronicle_session_entry se
    JOIN chronicle_session s ON s.session_id=se.session_id AND s.state='open'
    JOIN chronicle_participant cp ON cp.entry_id=NEW.entry_id
    JOIN user_preference p ON p.user_id=cp.user_id AND p.chronicle_opt_in=1
    WHERE se.entry_id=NEW.entry_id;
END;

CREATE TRIGGER chronicle_session_link_entry_participant_after_insert
AFTER INSERT ON chronicle_participant
BEGIN
    INSERT OR IGNORE INTO chronicle_session_participant(session_id,user_id,joined_at_ms)
    SELECT se.session_id,NEW.user_id,e.created_at_ms
    FROM chronicle_session_entry se
    JOIN chronicle_session s ON s.session_id=se.session_id AND s.state='open'
    JOIN chronicle_entry e ON e.entry_id=se.entry_id
    JOIN user_preference p ON p.user_id=NEW.user_id AND p.chronicle_opt_in=1
    WHERE se.entry_id=NEW.entry_id;
END;

CREATE TABLE chronicle_session_context (
    context_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    message_id TEXT NOT NULL,
    author_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    excerpt TEXT NOT NULL CHECK (length(CAST(excerpt AS BLOB)) BETWEEN 1 AND 500),
    observed_at_ms INTEGER NOT NULL CHECK (observed_at_ms >= 0),
    UNIQUE (session_id, message_id)
) STRICT;
CREATE INDEX chronicle_session_context_order
    ON chronicle_session_context(session_id, observed_at_ms, context_id);
CREATE TRIGGER chronicle_session_context_purge_after_opt_out
AFTER UPDATE OF chronicle_opt_in ON user_preference
WHEN OLD.chronicle_opt_in=1 AND NEW.chronicle_opt_in=0
BEGIN
    DELETE FROM chronicle_session_context WHERE author_user_id=NEW.user_id;
END;

CREATE TABLE chronicle_summary_draft (
    draft_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL UNIQUE REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    state TEXT NOT NULL CHECK (state IN ('pending', 'approved', 'rejected')),
    chapter_title TEXT NOT NULL CHECK (length(CAST(chapter_title AS BLOB)) BETWEEN 1 AND 100),
    summary TEXT NOT NULL CHECK (length(CAST(summary AS BLOB)) BETWEEN 1 AND 1000),
    source TEXT NOT NULL CHECK (source IN ('fallback', 'model', 'manual')),
    model_failure_category TEXT
        CHECK (model_failure_category IS NULL OR length(model_failure_category) BETWEEN 1 AND 96),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms),
    decided_at_ms INTEGER CHECK (decided_at_ms IS NULL OR decided_at_ms >= created_at_ms),
    decided_by_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    approved_entry_id TEXT REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    -- The durable outbox materializes this notice after the summary transaction.
    -- Keep the future notice identifier without a premature foreign key.
    review_notice_id TEXT,
    CHECK ((state = 'pending' AND decided_at_ms IS NULL AND decided_by_user_id IS NULL)
           OR (state IN ('approved', 'rejected') AND decided_at_ms IS NOT NULL
               AND decided_by_user_id IS NOT NULL))
) STRICT;

CREATE TABLE chronicle_summary_highlight (
    draft_id TEXT NOT NULL REFERENCES chronicle_summary_draft(draft_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 9),
    PRIMARY KEY (draft_id, entry_id),
    UNIQUE (draft_id, position)
) STRICT;

CREATE TABLE chronicle_title_definition (
    definition_id TEXT PRIMARY KEY,
    title TEXT NOT NULL CHECK (length(CAST(title AS BLOB)) BETWEEN 1 AND 100),
    description TEXT NOT NULL CHECK (length(CAST(description AS BLOB)) BETWEEN 1 AND 500),
    provenance TEXT NOT NULL CHECK (provenance IN ('owner_curated', 'session_ai')),
    session_id TEXT REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    supporting_entry_id TEXT REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    proposed_by_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((provenance = 'owner_curated' AND session_id IS NULL)
           OR (provenance = 'session_ai' AND session_id IS NOT NULL))
) STRICT;

CREATE TABLE chronicle_title_grant (
    grant_id TEXT PRIMARY KEY,
    definition_id TEXT NOT NULL REFERENCES chronicle_title_definition(definition_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    recipient_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    state TEXT NOT NULL CHECK (state IN ('proposed', 'active', 'rejected', 'revoked')),
    featured INTEGER NOT NULL DEFAULT 0 CHECK (featured IN (0, 1)),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    source_idempotency_key TEXT NOT NULL UNIQUE,
    proposed_at_ms INTEGER NOT NULL CHECK (proposed_at_ms >= 0),
    decided_at_ms INTEGER,
    decided_by_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    revoked_at_ms INTEGER,
    revoked_by_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    award_entry_id TEXT REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    CHECK ((state = 'proposed' AND decided_at_ms IS NULL AND decided_by_user_id IS NULL
            AND revoked_at_ms IS NULL AND revoked_by_user_id IS NULL)
           OR (state IN ('active', 'rejected') AND decided_at_ms IS NOT NULL
               AND decided_by_user_id IS NOT NULL AND revoked_at_ms IS NULL
               AND revoked_by_user_id IS NULL)
           OR (state = 'revoked' AND decided_at_ms IS NOT NULL
               AND decided_by_user_id IS NOT NULL AND revoked_at_ms IS NOT NULL
               AND revoked_by_user_id IS NOT NULL)),
    CHECK (featured = 0 OR state = 'active')
) STRICT;
CREATE UNIQUE INDEX chronicle_title_one_featured
    ON chronicle_title_grant(recipient_user_id) WHERE featured = 1;
CREATE UNIQUE INDEX chronicle_title_one_session_recipient
    ON chronicle_title_grant(recipient_user_id, definition_id);

-- Prompt attempts retain only the identities and revisions of approved
-- Chronicle context. Completion revalidates this snapshot before any reply or
-- relationship mutation is authorized; no Chronicle prose is duplicated here.
CREATE TABLE ai_prompt_attempt_chronicle_context (
    attempt_id TEXT PRIMARY KEY REFERENCES ai_prompt_attempt(attempt_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    featured_title_grant_id TEXT REFERENCES chronicle_title_grant(grant_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    featured_title_revision INTEGER CHECK (featured_title_revision > 0),
    latest_summary_entry_id TEXT REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    latest_summary_revision INTEGER CHECK (latest_summary_revision > 0),
    open_session_id TEXT REFERENCES chronicle_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    open_session_revision INTEGER CHECK (open_session_revision > 0),
    CHECK ((featured_title_grant_id IS NULL) =
           (featured_title_revision IS NULL)),
    CHECK ((latest_summary_entry_id IS NULL) =
           (latest_summary_revision IS NULL)),
    CHECK ((open_session_id IS NULL) = (open_session_revision IS NULL))
) STRICT;
CREATE TRIGGER ai_prompt_attempt_chronicle_context_no_update
BEFORE UPDATE ON ai_prompt_attempt_chronicle_context
BEGIN
    SELECT RAISE(ABORT, 'prompt Chronicle context is immutable');
END;
CREATE TRIGGER ai_prompt_attempt_chronicle_context_no_delete
BEFORE DELETE ON ai_prompt_attempt_chronicle_context
BEGIN
    SELECT RAISE(ABORT, 'prompt Chronicle context is immutable');
END;

CREATE TABLE chronicle_search_cursor (
    cursor_id TEXT PRIMARY KEY,
    viewer_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    presentation TEXT NOT NULL DEFAULT 'recall'
        CHECK (presentation IN ('recall', 'timeline')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms)
) STRICT;
CREATE TABLE chronicle_search_item (
    cursor_id TEXT NOT NULL REFERENCES chronicle_search_cursor(cursor_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 49),
    item_kind TEXT NOT NULL CHECK (item_kind IN ('entry', 'memory')),
    item_id TEXT NOT NULL,
    rank_value REAL NOT NULL,
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    PRIMARY KEY (cursor_id, position),
    UNIQUE (cursor_id, item_kind, item_id)
) STRICT;
CREATE INDEX chronicle_search_cursor_expiry
    ON chronicle_search_cursor(expires_at_ms, cursor_id);

CREATE TABLE chronicle_anniversary_delivery (
    delivery_id TEXT PRIMARY KEY,
    entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    local_year INTEGER NOT NULL CHECK (local_year BETWEEN 1970 AND 9999),
    local_date TEXT NOT NULL CHECK (length(local_date) = 10),
    is_test INTEGER NOT NULL CHECK (is_test IN (0, 1)),
    outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE (entry_id, local_year, is_test),
    UNIQUE (local_date, is_test)
) STRICT;

CREATE VIRTUAL TABLE chronicle_entry_fts USING fts5(
    title, body, source_text,
    content='chronicle_entry', content_rowid='rowid',
    tokenize='unicode61 remove_diacritics 2'
);
INSERT INTO chronicle_entry_fts(rowid,title,body,source_text)
SELECT rowid,title,body,source_text FROM chronicle_entry;
CREATE TRIGGER chronicle_entry_fts_after_insert AFTER INSERT ON chronicle_entry BEGIN
    INSERT INTO chronicle_entry_fts(rowid,title,body,source_text)
    VALUES (new.rowid,new.title,new.body,new.source_text);
END;
CREATE TRIGGER chronicle_entry_fts_after_delete AFTER DELETE ON chronicle_entry BEGIN
    INSERT INTO chronicle_entry_fts(chronicle_entry_fts,rowid,title,body,source_text)
    VALUES ('delete',old.rowid,old.title,old.body,old.source_text);
END;
CREATE TRIGGER chronicle_entry_fts_after_update AFTER UPDATE ON chronicle_entry BEGIN
    INSERT INTO chronicle_entry_fts(chronicle_entry_fts,rowid,title,body,source_text)
    VALUES ('delete',old.rowid,old.title,old.body,old.source_text);
    INSERT INTO chronicle_entry_fts(rowid,title,body,source_text)
    VALUES (new.rowid,new.title,new.body,new.source_text);
END;
