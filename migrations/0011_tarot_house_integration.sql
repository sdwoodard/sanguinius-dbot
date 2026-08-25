-- Milestone 13: versioned Emperor's Tarot draws, House wagers, player
-- projections, and durable integration observations. Catalog prose and
-- accepted terms are immutable audit data. Fate remains an integer,
-- double-entry, no-cash-value game currency.

-- Chronicle entries gain deterministic Tarot-event provenance. Rebuild the
-- parent through a fully populated replacement and preserve every direct child
-- in transaction-local copies before the swap. Any count mismatch aborts the
-- whole migration before an accepted table is dropped.
CREATE TABLE chronicle_entry_m13_new (
    entry_id TEXT PRIMARY KEY
        CHECK (length(entry_id)=36 AND substr(entry_id,9,1)='-'
          AND substr(entry_id,14,1)='-' AND substr(entry_id,19,1)='-'
          AND substr(entry_id,24,1)='-' AND length(replace(entry_id,'-',''))=32
          AND entry_id NOT GLOB '*[^0-9a-f-]*' AND substr(entry_id,15,1)='4'
          AND substr(entry_id,20,1) IN ('8','9','a','b')),
    entry_type TEXT NOT NULL CHECK (entry_type IN
        ('quote','deed','prediction','incident','custom','session_summary','title_award')),
    title TEXT NOT NULL CHECK (length(CAST(title AS BLOB)) BETWEEN 1 AND 100
        AND length(trim(title,char(9)||char(10)||char(11)||char(12)||char(13)||' '))>0),
    body TEXT NOT NULL CHECK (length(CAST(body AS BLOB)) BETWEEN 1 AND 1000
        AND length(trim(body,char(9)||char(10)||char(11)||char(12)||char(13)||' '))>0),
    visibility TEXT NOT NULL CHECK (visibility IN ('shared','participant_only')),
    status TEXT NOT NULL CHECK (status IN ('proposed','canon','retracted')),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms>=0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms>=0),
    created_by_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    submitted_at_ms INTEGER CHECK (submitted_at_ms IS NULL OR submitted_at_ms>=created_at_ms),
    approved_at_ms INTEGER CHECK (approved_at_ms IS NULL OR approved_at_ms>=created_at_ms),
    approved_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    retracted_at_ms INTEGER CHECK (retracted_at_ms IS NULL OR retracted_at_ms>=created_at_ms),
    retracted_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_channel_id TEXT NOT NULL CHECK (length(source_channel_id) BETWEEN 1 AND 20
        AND source_channel_id NOT GLOB '*[^0-9]*' AND source_channel_id<>'0'
        AND (length(source_channel_id)=1 OR substr(source_channel_id,1,1)<>'0')
        AND (length(source_channel_id)<20 OR source_channel_id<='18446744073709551615')),
    source_message_id TEXT CHECK (source_message_id IS NULL OR
        (length(source_message_id) BETWEEN 1 AND 20
         AND source_message_id NOT GLOB '*[^0-9]*' AND source_message_id<>'0'
         AND (length(source_message_id)=1 OR substr(source_message_id,1,1)<>'0')
         AND (length(source_message_id)<20 OR source_message_id<='18446744073709551615'))),
    source_author_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_text TEXT NOT NULL CHECK (length(CAST(source_text AS BLOB))<=2000),
    source_text_truncated INTEGER NOT NULL DEFAULT 0 CHECK (source_text_truncated IN (0,1)),
    source_attachment_count INTEGER NOT NULL DEFAULT 0 CHECK (source_attachment_count BETWEEN 0 AND 10),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision>0),
    source_kind TEXT NOT NULL DEFAULT 'discord_message' CHECK (source_kind IN
        ('discord_message','session_summary','title_award','tarot_event')),
    source_event_id TEXT UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    UNIQUE(source_guild_id,source_channel_id,source_message_id),
    CHECK ((approved_at_ms IS NULL)=(approved_by_user_id IS NULL)),
    CHECK ((retracted_at_ms IS NULL)=(retracted_by_user_id IS NULL)),
    CHECK ((status='proposed' AND approved_at_ms IS NULL AND retracted_at_ms IS NULL)
        OR (status='canon' AND approved_at_ms IS NOT NULL AND retracted_at_ms IS NULL)
        OR (status='retracted' AND retracted_at_ms IS NOT NULL)),
    CHECK ((source_kind='discord_message' AND source_message_id IS NOT NULL
            AND source_event_id IS NULL AND entry_type NOT IN ('session_summary','title_award'))
        OR (source_kind='session_summary' AND source_message_id IS NULL
            AND source_event_id IS NULL AND entry_type='session_summary')
        OR (source_kind='title_award' AND source_message_id IS NULL
            AND source_event_id IS NULL AND entry_type='title_award')
        OR (source_kind='tarot_event' AND source_message_id IS NULL
            AND source_event_id IS NOT NULL
            AND entry_type IN ('deed','prediction','incident','custom')))
) STRICT;

INSERT INTO chronicle_entry_m13_new(rowid,entry_id,entry_type,title,body,
  visibility,status,occurred_at_ms,created_at_ms,created_by_user_id,
  submitted_at_ms,approved_at_ms,approved_by_user_id,retracted_at_ms,
  retracted_by_user_id,source_guild_id,source_channel_id,source_message_id,
  source_author_user_id,source_text,source_text_truncated,
  source_attachment_count,revision,source_kind,source_event_id)
SELECT rowid,entry_id,entry_type,title,body,visibility,status,occurred_at_ms,
  created_at_ms,created_by_user_id,submitted_at_ms,approved_at_ms,
  approved_by_user_id,retracted_at_ms,retracted_by_user_id,source_guild_id,
  source_channel_id,source_message_id,source_author_user_id,source_text,
  source_text_truncated,source_attachment_count,revision,source_kind,NULL
FROM chronicle_entry;

CREATE TEMP TABLE m13_chronicle_participant AS SELECT * FROM chronicle_participant;
CREATE TEMP TABLE m13_chronicle_tag AS SELECT * FROM chronicle_tag;
CREATE TEMP TABLE m13_chronicle_attachment AS SELECT * FROM chronicle_attachment;
CREATE TEMP TABLE m13_chronicle_approval AS SELECT * FROM chronicle_approval;
CREATE TEMP TABLE m13_chronicle_session_entry AS SELECT * FROM chronicle_session_entry;
CREATE TEMP TABLE m13_chronicle_summary_draft AS SELECT * FROM chronicle_summary_draft;
CREATE TEMP TABLE m13_chronicle_summary_highlight AS SELECT * FROM chronicle_summary_highlight;
CREATE TEMP TABLE m13_chronicle_title_definition AS SELECT * FROM chronicle_title_definition;
CREATE TEMP TABLE m13_chronicle_title_grant AS SELECT * FROM chronicle_title_grant;
CREATE TEMP TABLE m13_ai_prompt_attempt_chronicle_context AS SELECT * FROM ai_prompt_attempt_chronicle_context;
CREATE TEMP TABLE m13_chronicle_anniversary_delivery AS SELECT * FROM chronicle_anniversary_delivery;
CREATE TEMP TABLE m13_chronicle_appearance_source AS SELECT * FROM chronicle_appearance_source;
CREATE TEMP TABLE m13_chronicle_verify(delta INTEGER NOT NULL CHECK(delta=0));
INSERT INTO m13_chronicle_verify VALUES
  ((SELECT count(*) FROM chronicle_entry_m13_new)-(SELECT count(*) FROM chronicle_entry)),
  ((SELECT count(*) FROM m13_chronicle_participant)-(SELECT count(*) FROM chronicle_participant)),
  ((SELECT count(*) FROM m13_chronicle_tag)-(SELECT count(*) FROM chronicle_tag)),
  ((SELECT count(*) FROM m13_chronicle_attachment)-(SELECT count(*) FROM chronicle_attachment)),
  ((SELECT count(*) FROM m13_chronicle_approval)-(SELECT count(*) FROM chronicle_approval)),
  ((SELECT count(*) FROM m13_chronicle_session_entry)-(SELECT count(*) FROM chronicle_session_entry)),
  ((SELECT count(*) FROM m13_chronicle_summary_draft)-(SELECT count(*) FROM chronicle_summary_draft)),
  ((SELECT count(*) FROM m13_chronicle_summary_highlight)-(SELECT count(*) FROM chronicle_summary_highlight)),
  ((SELECT count(*) FROM m13_chronicle_title_definition)-(SELECT count(*) FROM chronicle_title_definition)),
  ((SELECT count(*) FROM m13_chronicle_title_grant)-(SELECT count(*) FROM chronicle_title_grant)),
  ((SELECT count(*) FROM m13_ai_prompt_attempt_chronicle_context)-(SELECT count(*) FROM ai_prompt_attempt_chronicle_context)),
  ((SELECT count(*) FROM m13_chronicle_anniversary_delivery)-(SELECT count(*) FROM chronicle_anniversary_delivery)),
  ((SELECT count(*) FROM m13_chronicle_appearance_source)-(SELECT count(*) FROM chronicle_appearance_source));

DROP TRIGGER appearance_callback_withdrawal_cancel;
DROP TRIGGER appearance_chronicle_preference_withdrawal_cancel;
DROP VIEW appearance_candidate_source_user;
DROP TABLE chronicle_appearance_source;
DROP TABLE chronicle_anniversary_delivery;
DROP TABLE ai_prompt_attempt_chronicle_context;
DROP TABLE chronicle_title_grant;
DROP TABLE chronicle_title_definition;
DROP TABLE chronicle_summary_highlight;
DROP TABLE chronicle_summary_draft;
DROP TABLE chronicle_session_entry;
DROP TABLE chronicle_approval;
DROP TABLE chronicle_attachment;
DROP TABLE chronicle_tag;
DROP TABLE chronicle_participant;
DROP TABLE chronicle_entry;
ALTER TABLE chronicle_entry_m13_new RENAME TO chronicle_entry;

CREATE TABLE chronicle_participant (
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  role TEXT NOT NULL CHECK(role IN ('proposer','source_author','subject','session_participant','title_recipient')),
  PRIMARY KEY(entry_id,user_id,role)
) STRICT;
CREATE TABLE chronicle_tag (
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  tag TEXT NOT NULL CHECK(length(CAST(tag AS BLOB)) BETWEEN 1 AND 32
    AND tag=lower(tag) AND tag NOT GLOB '*[^a-z0-9_-]*'),
  PRIMARY KEY(entry_id,tag)
) STRICT;
CREATE TABLE chronicle_attachment (
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  position INTEGER NOT NULL CHECK(position BETWEEN 0 AND 9),
  attachment_id TEXT NOT NULL CHECK(length(attachment_id) BETWEEN 1 AND 20
    AND attachment_id NOT GLOB '*[^0-9]*' AND attachment_id<>'0'
    AND (length(attachment_id)=1 OR substr(attachment_id,1,1)<>'0')
    AND (length(attachment_id)<20 OR attachment_id<='18446744073709551615')),
  filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 255),
  content_type TEXT CHECK(content_type IS NULL OR length(CAST(content_type AS BLOB)) BETWEEN 1 AND 127),
  byte_size INTEGER NOT NULL CHECK(byte_size>=0),
  width INTEGER CHECK(width IS NULL OR width BETWEEN 1 AND 4294967295),
  height INTEGER CHECK(height IS NULL OR height BETWEEN 1 AND 4294967295),
  is_ephemeral INTEGER NOT NULL CHECK(is_ephemeral IN (0,1)),
  is_spoiler INTEGER NOT NULL CHECK(is_spoiler IN (0,1)),
  PRIMARY KEY(entry_id,position), UNIQUE(entry_id,attachment_id)
) STRICT;
CREATE TABLE chronicle_approval (
  approval_id TEXT PRIMARY KEY,
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  reviewer_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  approval_role TEXT NOT NULL CHECK(approval_role IN ('proposer','participant','owner','owner_stale','owner_test')),
  state TEXT NOT NULL CHECK(state IN ('pending','approved','declined','cancelled')),
  entry_revision INTEGER NOT NULL CHECK(entry_revision>0),
  notice_id TEXT REFERENCES pending_notice(notice_id) ON UPDATE RESTRICT ON DELETE SET NULL,
  requested_at_ms INTEGER NOT NULL CHECK(requested_at_ms>=0),
  acted_at_ms INTEGER CHECK(acted_at_ms IS NULL OR acted_at_ms>=requested_at_ms),
  interaction_idempotency_key TEXT UNIQUE CHECK(interaction_idempotency_key IS NULL OR length(interaction_idempotency_key) BETWEEN 1 AND 160),
  UNIQUE(entry_id,reviewer_user_id,approval_role),
  CHECK((state='pending')=(acted_at_ms IS NULL))
) STRICT;
CREATE TABLE chronicle_session_entry (
  session_id TEXT NOT NULL REFERENCES chronicle_session(session_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  linked_at_ms INTEGER NOT NULL CHECK(linked_at_ms>=0),
  PRIMARY KEY(session_id,entry_id)
) STRICT;
CREATE TABLE chronicle_summary_draft (
  draft_id TEXT PRIMARY KEY,
  session_id TEXT NOT NULL UNIQUE REFERENCES chronicle_session(session_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  state TEXT NOT NULL CHECK(state IN ('pending','approved','rejected')),
  chapter_title TEXT NOT NULL CHECK(length(CAST(chapter_title AS BLOB)) BETWEEN 1 AND 100),
  summary TEXT NOT NULL CHECK(length(CAST(summary AS BLOB)) BETWEEN 1 AND 1000),
  source TEXT NOT NULL CHECK(source IN ('fallback','model','manual')),
  model_failure_category TEXT CHECK(model_failure_category IS NULL OR length(model_failure_category) BETWEEN 1 AND 96),
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
  created_at_ms INTEGER NOT NULL CHECK(created_at_ms>=0),
  updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms>=created_at_ms),
  decided_at_ms INTEGER CHECK(decided_at_ms IS NULL OR decided_at_ms>=created_at_ms),
  decided_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  approved_entry_id TEXT REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  review_notice_id TEXT,
  CHECK((state='pending' AND decided_at_ms IS NULL AND decided_by_user_id IS NULL)
    OR (state IN ('approved','rejected') AND decided_at_ms IS NOT NULL AND decided_by_user_id IS NOT NULL))
) STRICT;
CREATE TABLE chronicle_summary_highlight (
  draft_id TEXT NOT NULL REFERENCES chronicle_summary_draft(draft_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  position INTEGER NOT NULL CHECK(position BETWEEN 0 AND 9),
  PRIMARY KEY(draft_id,entry_id), UNIQUE(draft_id,position)
) STRICT;
CREATE TABLE chronicle_title_definition (
  definition_id TEXT PRIMARY KEY,
  title TEXT NOT NULL CHECK(length(CAST(title AS BLOB)) BETWEEN 1 AND 100),
  description TEXT NOT NULL CHECK(length(CAST(description AS BLOB)) BETWEEN 1 AND 500),
  provenance TEXT NOT NULL CHECK(provenance IN ('owner_curated','session_ai','tarot_system')),
  session_id TEXT REFERENCES chronicle_session(session_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  supporting_entry_id TEXT REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  proposed_by_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  created_at_ms INTEGER NOT NULL CHECK(created_at_ms>=0),
  CHECK((provenance IN ('owner_curated','tarot_system') AND session_id IS NULL)
    OR (provenance='session_ai' AND session_id IS NOT NULL))
) STRICT;
CREATE TABLE chronicle_title_grant (
  grant_id TEXT PRIMARY KEY,
  definition_id TEXT NOT NULL REFERENCES chronicle_title_definition(definition_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  recipient_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  state TEXT NOT NULL CHECK(state IN ('proposed','active','rejected','revoked')),
  featured INTEGER NOT NULL DEFAULT 0 CHECK(featured IN (0,1)),
  revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
  source_idempotency_key TEXT NOT NULL UNIQUE,
  proposed_at_ms INTEGER NOT NULL CHECK(proposed_at_ms>=0),
  decided_at_ms INTEGER,
  decided_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  revoked_at_ms INTEGER,
  revoked_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  award_entry_id TEXT REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  CHECK((state='proposed' AND decided_at_ms IS NULL AND decided_by_user_id IS NULL AND revoked_at_ms IS NULL AND revoked_by_user_id IS NULL)
    OR (state IN ('active','rejected') AND decided_at_ms IS NOT NULL AND decided_by_user_id IS NOT NULL AND revoked_at_ms IS NULL AND revoked_by_user_id IS NULL)
    OR (state='revoked' AND decided_at_ms IS NOT NULL AND decided_by_user_id IS NOT NULL AND revoked_at_ms IS NOT NULL AND revoked_by_user_id IS NOT NULL)),
  CHECK(featured=0 OR state='active')
) STRICT;
CREATE TABLE ai_prompt_attempt_chronicle_context (
  attempt_id TEXT PRIMARY KEY REFERENCES ai_prompt_attempt(attempt_id) ON UPDATE RESTRICT ON DELETE CASCADE,
  featured_title_grant_id TEXT REFERENCES chronicle_title_grant(grant_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  featured_title_revision INTEGER CHECK(featured_title_revision>0),
  latest_summary_entry_id TEXT REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  latest_summary_revision INTEGER CHECK(latest_summary_revision>0),
  open_session_id TEXT REFERENCES chronicle_session(session_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  open_session_revision INTEGER CHECK(open_session_revision>0),
  CHECK((featured_title_grant_id IS NULL)=(featured_title_revision IS NULL)),
  CHECK((latest_summary_entry_id IS NULL)=(latest_summary_revision IS NULL)),
  CHECK((open_session_id IS NULL)=(open_session_revision IS NULL))
) STRICT;
CREATE TABLE chronicle_anniversary_delivery (
  delivery_id TEXT PRIMARY KEY,
  entry_id TEXT NOT NULL REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  local_year INTEGER NOT NULL CHECK(local_year BETWEEN 1970 AND 9999),
  local_date TEXT NOT NULL CHECK(length(local_date)=10),
  is_test INTEGER NOT NULL CHECK(is_test IN (0,1)),
  outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  created_at_ms INTEGER NOT NULL CHECK(created_at_ms>=0),
  UNIQUE(entry_id,local_year,is_test), UNIQUE(local_date,is_test)
) STRICT;
CREATE TABLE chronicle_appearance_source (
  entry_id TEXT PRIMARY KEY REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  decision_id TEXT NOT NULL UNIQUE REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
  created_at_ms INTEGER NOT NULL CHECK(created_at_ms>=0)
) STRICT;

INSERT INTO chronicle_participant SELECT * FROM m13_chronicle_participant;
INSERT INTO chronicle_tag SELECT * FROM m13_chronicle_tag;
INSERT INTO chronicle_attachment SELECT * FROM m13_chronicle_attachment;
INSERT INTO chronicle_approval SELECT * FROM m13_chronicle_approval;
INSERT INTO chronicle_session_entry SELECT * FROM m13_chronicle_session_entry;
INSERT INTO chronicle_summary_draft SELECT * FROM m13_chronicle_summary_draft;
INSERT INTO chronicle_summary_highlight SELECT * FROM m13_chronicle_summary_highlight;
INSERT INTO chronicle_title_definition SELECT * FROM m13_chronicle_title_definition;
INSERT INTO chronicle_title_grant SELECT * FROM m13_chronicle_title_grant;
INSERT INTO ai_prompt_attempt_chronicle_context SELECT * FROM m13_ai_prompt_attempt_chronicle_context;
INSERT INTO chronicle_anniversary_delivery SELECT * FROM m13_chronicle_anniversary_delivery;
INSERT INTO chronicle_appearance_source SELECT * FROM m13_chronicle_appearance_source;

DELETE FROM m13_chronicle_verify;
INSERT INTO m13_chronicle_verify VALUES
  ((SELECT count(*) FROM chronicle_participant)-(SELECT count(*) FROM m13_chronicle_participant)),
  ((SELECT count(*) FROM chronicle_tag)-(SELECT count(*) FROM m13_chronicle_tag)),
  ((SELECT count(*) FROM chronicle_attachment)-(SELECT count(*) FROM m13_chronicle_attachment)),
  ((SELECT count(*) FROM chronicle_approval)-(SELECT count(*) FROM m13_chronicle_approval)),
  ((SELECT count(*) FROM chronicle_session_entry)-(SELECT count(*) FROM m13_chronicle_session_entry)),
  ((SELECT count(*) FROM chronicle_summary_draft)-(SELECT count(*) FROM m13_chronicle_summary_draft)),
  ((SELECT count(*) FROM chronicle_summary_highlight)-(SELECT count(*) FROM m13_chronicle_summary_highlight)),
  ((SELECT count(*) FROM chronicle_title_definition)-(SELECT count(*) FROM m13_chronicle_title_definition)),
  ((SELECT count(*) FROM chronicle_title_grant)-(SELECT count(*) FROM m13_chronicle_title_grant)),
  ((SELECT count(*) FROM ai_prompt_attempt_chronicle_context)-(SELECT count(*) FROM m13_ai_prompt_attempt_chronicle_context)),
  ((SELECT count(*) FROM chronicle_anniversary_delivery)-(SELECT count(*) FROM m13_chronicle_anniversary_delivery)),
  ((SELECT count(*) FROM chronicle_appearance_source)-(SELECT count(*) FROM m13_chronicle_appearance_source));

DROP TABLE m13_chronicle_appearance_source;
DROP TABLE m13_chronicle_anniversary_delivery;
DROP TABLE m13_ai_prompt_attempt_chronicle_context;
DROP TABLE m13_chronicle_title_grant;
DROP TABLE m13_chronicle_title_definition;
DROP TABLE m13_chronicle_summary_highlight;
DROP TABLE m13_chronicle_summary_draft;
DROP TABLE m13_chronicle_session_entry;
DROP TABLE m13_chronicle_approval;
DROP TABLE m13_chronicle_attachment;
DROP TABLE m13_chronicle_tag;
DROP TABLE m13_chronicle_participant;
DROP TABLE m13_chronicle_verify;

CREATE INDEX chronicle_entry_status_time ON chronicle_entry(status,occurred_at_ms DESC,entry_id DESC);
CREATE INDEX chronicle_participant_user ON chronicle_participant(user_id,entry_id);
CREATE INDEX chronicle_tag_lookup ON chronicle_tag(tag,entry_id);
CREATE INDEX chronicle_approval_pending ON chronicle_approval(reviewer_user_id,requested_at_ms,approval_id) WHERE state='pending';
CREATE UNIQUE INDEX chronicle_title_one_featured ON chronicle_title_grant(recipient_user_id) WHERE featured=1;
CREATE UNIQUE INDEX chronicle_title_one_session_recipient ON chronicle_title_grant(recipient_user_id,definition_id);

CREATE TRIGGER chronicle_tag_maximum_before_insert BEFORE INSERT ON chronicle_tag
WHEN (SELECT count(*) FROM chronicle_tag WHERE entry_id=NEW.entry_id)>=5
BEGIN SELECT RAISE(ABORT,'chronicle entry tag limit exceeded'); END;
CREATE TRIGGER chronicle_session_link_entry_after_insert AFTER INSERT ON chronicle_entry
BEGIN
  INSERT OR IGNORE INTO chronicle_session_entry(session_id,entry_id,linked_at_ms)
  SELECT session_id,NEW.entry_id,NEW.created_at_ms FROM chronicle_session
  WHERE guild_id=NEW.source_guild_id AND state='open';
END;
CREATE TRIGGER chronicle_session_relink_eligible_entry_after_update
AFTER UPDATE OF status,visibility ON chronicle_entry
WHEN NEW.status='canon' AND NEW.visibility='shared'
BEGIN
  INSERT OR IGNORE INTO chronicle_session_entry(session_id,entry_id,linked_at_ms)
  SELECT session_id,NEW.entry_id,coalesce(NEW.approved_at_ms,NEW.created_at_ms)
  FROM chronicle_session WHERE guild_id=NEW.source_guild_id AND state='open';
  INSERT OR IGNORE INTO chronicle_session_participant(session_id,user_id,joined_at_ms)
  SELECT se.session_id,cp.user_id,coalesce(NEW.approved_at_ms,NEW.created_at_ms)
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
  SELECT se.session_id,NEW.user_id,e.created_at_ms FROM chronicle_session_entry se
  JOIN chronicle_session s ON s.session_id=se.session_id AND s.state='open'
  JOIN chronicle_entry e ON e.entry_id=se.entry_id
  JOIN user_preference p ON p.user_id=NEW.user_id AND p.chronicle_opt_in=1
  WHERE se.entry_id=NEW.entry_id;
END;
CREATE TRIGGER chronicle_session_bound_entry_after_insert
AFTER INSERT ON chronicle_session_entry
BEGIN
  DELETE FROM chronicle_session_entry WHERE session_id=NEW.session_id AND entry_id IN(
    SELECT se.entry_id FROM chronicle_session_entry se
    JOIN chronicle_entry e ON e.entry_id=se.entry_id WHERE se.session_id=NEW.session_id
    ORDER BY CASE WHEN e.status='canon' AND e.visibility='shared' THEN 0 ELSE 1 END,
      se.linked_at_ms DESC,se.entry_id DESC LIMIT -1 OFFSET 50);
END;
CREATE TRIGGER chronicle_entry_fts_after_insert AFTER INSERT ON chronicle_entry
BEGIN INSERT INTO chronicle_entry_fts(rowid,title,body,source_text)
VALUES(NEW.rowid,NEW.title,NEW.body,NEW.source_text); END;
CREATE TRIGGER chronicle_entry_fts_after_delete AFTER DELETE ON chronicle_entry
BEGIN INSERT INTO chronicle_entry_fts(chronicle_entry_fts,rowid,title,body,source_text)
VALUES('delete',OLD.rowid,OLD.title,OLD.body,OLD.source_text); END;
CREATE TRIGGER chronicle_entry_fts_after_update AFTER UPDATE ON chronicle_entry
BEGIN
  INSERT INTO chronicle_entry_fts(chronicle_entry_fts,rowid,title,body,source_text)
  VALUES('delete',OLD.rowid,OLD.title,OLD.body,OLD.source_text);
  INSERT INTO chronicle_entry_fts(rowid,title,body,source_text)
  VALUES(NEW.rowid,NEW.title,NEW.body,NEW.source_text);
END;
CREATE TRIGGER appearance_chronicle_withdrawal_cancel
AFTER UPDATE OF status,visibility,revision ON chronicle_entry
WHEN OLD.status='canon' AND (NEW.status<>'canon' OR NEW.visibility<>'shared' OR NEW.revision<>OLD.revision)
BEGIN
  UPDATE outbox_message SET state='cancelled',lease_owner=NULL,lease_token=NULL,
    lease_until_ms=NULL,terminal_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.created_at_ms),created_at_ms,updated_at_ms),
    updated_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.created_at_ms),created_at_ms,updated_at_ms),
    last_error_code='appearance_chronicle_withdrawn'
  WHERE outbox_id IN(
    SELECT r.outbox_id FROM appearance_budget_reservation r
    JOIN appearance_candidate_source cs ON cs.candidate_id=r.candidate_id
    LEFT JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
    WHERE (cs.source_kind='chronicle_entry' AND cs.source_id=NEW.entry_id)
      OR (o.event_type IN ('chronicle.entry_canonized.v1','chronicle.anniversary_delivered.v1') AND o.aggregate_id=NEW.entry_id))
    AND first_attempt_at_ms IS NULL
    AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;
CREATE TRIGGER ai_prompt_attempt_chronicle_context_no_update
BEFORE UPDATE ON ai_prompt_attempt_chronicle_context
BEGIN SELECT RAISE(ABORT,'prompt Chronicle context is immutable'); END;
CREATE TRIGGER ai_prompt_attempt_chronicle_context_no_delete
BEFORE DELETE ON ai_prompt_attempt_chronicle_context
BEGIN SELECT RAISE(ABORT,'prompt Chronicle context is immutable'); END;
CREATE TRIGGER chronicle_appearance_source_no_update BEFORE UPDATE ON chronicle_appearance_source
BEGIN SELECT RAISE(ABORT,'Chronicle appearance provenance is immutable'); END;
CREATE TRIGGER chronicle_appearance_source_no_delete BEFORE DELETE ON chronicle_appearance_source
BEGIN SELECT RAISE(ABORT,'Chronicle appearance provenance is immutable'); END;
CREATE VIEW appearance_candidate_source_user AS
SELECT cs.candidate_id,cp.user_id FROM appearance_candidate_source cs
JOIN chronicle_participant cp ON cp.entry_id=cs.source_id
WHERE cs.source_kind='chronicle_entry'
UNION
SELECT cs.candidate_id,cp.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_participant cp ON cp.entry_id=o.aggregate_id
WHERE o.event_type IN ('chronicle.entry_canonized.v1','chronicle.anniversary_delivered.v1')
UNION
SELECT cs.candidate_id,sp.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_session_participant sp ON sp.session_id=o.aggregate_id
WHERE o.event_type IN ('chronicle.session_started.v1','chronicle.session_completed.v1')
UNION
SELECT cs.candidate_id,tg.recipient_user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_title_grant tg ON tg.grant_id=o.aggregate_id
WHERE o.event_type='chronicle.title_awarded.v1';
CREATE TRIGGER appearance_callback_withdrawal_cancel
AFTER UPDATE OF appearance_callback_opt_in ON user_preference
WHEN OLD.appearance_callback_opt_in=1 AND NEW.appearance_callback_opt_in=0
BEGIN
  UPDATE outbox_message SET state='cancelled',lease_owner=NULL,lease_token=NULL,
    lease_until_ms=NULL,terminal_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
    updated_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
    last_error_code='appearance_opt_out'
  WHERE outbox_id IN(
    SELECT r.outbox_id FROM appearance_budget_reservation r
    WHERE EXISTS(SELECT 1 FROM appearance_delivery_participant dp
      WHERE dp.decision_id=r.decision_id AND dp.user_id=NEW.user_id)
    OR EXISTS(SELECT 1 FROM appearance_candidate_actor a
      WHERE a.candidate_id=r.candidate_id AND a.user_id=NEW.user_id)
    OR EXISTS(SELECT 1 FROM appearance_decision_memory dm
      JOIN memory_subject s ON s.memory_id=dm.memory_id
      WHERE dm.decision_id=r.decision_id AND s.subject_type='user' AND s.subject_id=NEW.user_id)
    OR EXISTS(SELECT 1 FROM appearance_candidate_source_user su
      WHERE su.candidate_id=r.candidate_id AND su.user_id=NEW.user_id))
    AND first_attempt_at_ms IS NULL
    AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;
CREATE TRIGGER appearance_chronicle_preference_withdrawal_cancel
AFTER UPDATE OF chronicle_opt_in ON user_preference
WHEN OLD.chronicle_opt_in=1 AND NEW.chronicle_opt_in=0
BEGIN
  UPDATE outbox_message SET state='cancelled',lease_owner=NULL,lease_token=NULL,
    lease_until_ms=NULL,terminal_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
    updated_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
    last_error_code='appearance_opt_out'
  WHERE outbox_id IN(
    SELECT r.outbox_id FROM appearance_budget_reservation r
    WHERE EXISTS(SELECT 1 FROM appearance_decision_memory dm
      JOIN memory_subject s ON s.memory_id=dm.memory_id
      WHERE dm.decision_id=r.decision_id AND s.subject_type='user' AND s.subject_id=NEW.user_id)
    OR EXISTS(SELECT 1 FROM appearance_candidate_source_user su
      JOIN appearance_candidate candidate ON candidate.candidate_id=su.candidate_id
      WHERE su.candidate_id=r.candidate_id AND su.user_id=NEW.user_id
        AND candidate.candidate_type<>'tarot_event'))
    AND first_attempt_at_ms IS NULL
    AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

-- Extend the accepted appearance candidate family with exact Tarot-event
-- identity. Preserve every candidate descendant and nullable back-reference;
-- terminal audit triggers are restored after the count-verified swap.
CREATE TEMP TABLE m13_appearance_candidate AS SELECT * FROM appearance_candidate;
CREATE TEMP TABLE m13_appearance_candidate_source AS SELECT * FROM appearance_candidate_source;
CREATE TEMP TABLE m13_appearance_candidate_actor AS SELECT * FROM appearance_candidate_actor;
CREATE TEMP TABLE m13_appearance_decision AS SELECT * FROM appearance_decision;
CREATE TEMP TABLE m13_appearance_decision_memory AS SELECT * FROM appearance_decision_memory;
CREATE TEMP TABLE m13_appearance_preview AS SELECT * FROM appearance_preview;
CREATE TEMP TABLE m13_appearance_budget_reservation AS SELECT * FROM appearance_budget_reservation;
CREATE TEMP TABLE m13_appearance_delivery_participant AS SELECT * FROM appearance_delivery_participant;
CREATE TEMP TABLE m13_appearance_feedback_control AS SELECT * FROM appearance_feedback_control;
CREATE TEMP TABLE m13_appearance_feedback AS SELECT * FROM appearance_feedback;
CREATE TEMP TABLE m13_chronicle_appearance_source_swap AS SELECT * FROM chronicle_appearance_source;
CREATE TEMP TABLE m13_appearance_activity_candidate AS
  SELECT message_id,consumed_candidate_id FROM appearance_message_activity
  WHERE consumed_candidate_id IS NOT NULL;
CREATE TEMP TABLE m13_appearance_event_candidate AS
  SELECT source_event_id,candidate_id FROM appearance_event_observation
  WHERE candidate_id IS NOT NULL;

DROP TRIGGER appearance_decision_no_delete;
DROP TRIGGER appearance_budget_reservation_no_delete;
DROP TRIGGER appearance_budget_reservation_requires_new_outbox;
DROP TRIGGER appearance_delivery_participant_no_delete;
DROP TRIGGER appearance_feedback_control_no_delete;
DROP TRIGGER appearance_feedback_no_delete;
DROP TRIGGER chronicle_appearance_source_no_delete;

DELETE FROM chronicle_appearance_source;
DELETE FROM appearance_feedback;
DELETE FROM appearance_feedback_control;
DELETE FROM appearance_delivery_participant;
DELETE FROM appearance_budget_reservation;
DELETE FROM appearance_preview;
DELETE FROM appearance_decision_memory;
DELETE FROM appearance_decision;
DELETE FROM appearance_candidate_source;
DELETE FROM appearance_candidate_actor;
UPDATE appearance_message_activity SET consumed_candidate_id=NULL
WHERE consumed_candidate_id IS NOT NULL;
UPDATE appearance_event_observation SET candidate_id=NULL
WHERE candidate_id IS NOT NULL;
DELETE FROM appearance_candidate;
DROP TABLE appearance_candidate;

CREATE TABLE appearance_candidate (
    candidate_id TEXT PRIMARY KEY CHECK (length(candidate_id) = 36),
    candidate_type TEXT NOT NULL CHECK (candidate_type IN
      ('conversation','recurrence','chronicle_entry','session_started',
       'session_completed','title_awarded','anniversary','tarot_event',
       'simulation')),
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
    mode_activated_at_ms INTEGER NOT NULL DEFAULT 0 CHECK (mode_activated_at_ms >= 0),
    CHECK (evaluation_started_at_ms IS NULL OR evaluation_started_at_ms >= created_at_ms)
) STRICT;
CREATE INDEX appearance_candidate_expiry ON appearance_candidate(expires_at_ms,candidate_id);
CREATE INDEX appearance_candidate_context_expiry ON appearance_candidate(context_expires_at_ms,candidate_id);

INSERT INTO appearance_candidate SELECT * FROM m13_appearance_candidate;
INSERT INTO appearance_candidate_source SELECT * FROM m13_appearance_candidate_source;
INSERT INTO appearance_candidate_actor SELECT * FROM m13_appearance_candidate_actor;
INSERT INTO appearance_decision SELECT * FROM m13_appearance_decision;
INSERT INTO appearance_decision_memory SELECT * FROM m13_appearance_decision_memory;
INSERT INTO appearance_preview SELECT * FROM m13_appearance_preview;
INSERT INTO appearance_budget_reservation SELECT * FROM m13_appearance_budget_reservation;
INSERT INTO appearance_delivery_participant SELECT * FROM m13_appearance_delivery_participant;
INSERT INTO appearance_feedback_control SELECT * FROM m13_appearance_feedback_control;
INSERT INTO appearance_feedback SELECT * FROM m13_appearance_feedback;
INSERT INTO chronicle_appearance_source SELECT * FROM m13_chronicle_appearance_source_swap;
UPDATE appearance_message_activity SET consumed_candidate_id=(
  SELECT saved.consumed_candidate_id FROM m13_appearance_activity_candidate saved
  WHERE saved.message_id=appearance_message_activity.message_id)
WHERE message_id IN (SELECT message_id FROM m13_appearance_activity_candidate);
UPDATE appearance_event_observation SET candidate_id=(
  SELECT saved.candidate_id FROM m13_appearance_event_candidate saved
  WHERE saved.source_event_id=appearance_event_observation.source_event_id)
WHERE source_event_id IN (SELECT source_event_id FROM m13_appearance_event_candidate);

CREATE TEMP TABLE m13_appearance_verify(delta INTEGER NOT NULL CHECK(delta=0));
INSERT INTO m13_appearance_verify VALUES
  ((SELECT count(*) FROM appearance_candidate)-(SELECT count(*) FROM m13_appearance_candidate)),
  ((SELECT count(*) FROM appearance_candidate_source)-(SELECT count(*) FROM m13_appearance_candidate_source)),
  ((SELECT count(*) FROM appearance_candidate_actor)-(SELECT count(*) FROM m13_appearance_candidate_actor)),
  ((SELECT count(*) FROM appearance_decision)-(SELECT count(*) FROM m13_appearance_decision)),
  ((SELECT count(*) FROM appearance_decision_memory)-(SELECT count(*) FROM m13_appearance_decision_memory)),
  ((SELECT count(*) FROM appearance_preview)-(SELECT count(*) FROM m13_appearance_preview)),
  ((SELECT count(*) FROM appearance_budget_reservation)-(SELECT count(*) FROM m13_appearance_budget_reservation)),
  ((SELECT count(*) FROM appearance_delivery_participant)-(SELECT count(*) FROM m13_appearance_delivery_participant)),
  ((SELECT count(*) FROM appearance_feedback_control)-(SELECT count(*) FROM m13_appearance_feedback_control)),
  ((SELECT count(*) FROM appearance_feedback)-(SELECT count(*) FROM m13_appearance_feedback)),
  ((SELECT count(*) FROM chronicle_appearance_source)-(SELECT count(*) FROM m13_chronicle_appearance_source_swap)),
  ((SELECT count(*) FROM appearance_message_activity WHERE consumed_candidate_id IS NOT NULL)
    -(SELECT count(*) FROM m13_appearance_activity_candidate)),
  ((SELECT count(*) FROM appearance_event_observation WHERE candidate_id IS NOT NULL)
    -(SELECT count(*) FROM m13_appearance_event_candidate));

DROP TABLE m13_appearance_verify;
DROP TABLE m13_appearance_event_candidate;
DROP TABLE m13_appearance_activity_candidate;
DROP TABLE m13_chronicle_appearance_source_swap;
DROP TABLE m13_appearance_feedback;
DROP TABLE m13_appearance_feedback_control;
DROP TABLE m13_appearance_delivery_participant;
DROP TABLE m13_appearance_budget_reservation;
DROP TABLE m13_appearance_preview;
DROP TABLE m13_appearance_decision_memory;
DROP TABLE m13_appearance_decision;
DROP TABLE m13_appearance_candidate_actor;
DROP TABLE m13_appearance_candidate_source;
DROP TABLE m13_appearance_candidate;

CREATE TRIGGER appearance_decision_no_delete
BEFORE DELETE ON appearance_decision BEGIN
    SELECT RAISE(ABORT, 'appearance decisions are durable audit records');
END;
CREATE TRIGGER appearance_budget_reservation_no_delete
BEFORE DELETE ON appearance_budget_reservation BEGIN
    SELECT RAISE(ABORT, 'appearance budget reservations are immutable');
END;
CREATE TRIGGER appearance_budget_reservation_requires_new_outbox
BEFORE INSERT ON appearance_budget_reservation
WHEN EXISTS (SELECT 1 FROM outbox_message WHERE outbox_id=NEW.outbox_id)
BEGIN
    SELECT RAISE(ABORT, 'appearance reservation cannot adopt an existing outbox');
END;
CREATE TRIGGER appearance_delivery_participant_no_delete
BEFORE DELETE ON appearance_delivery_participant BEGIN
    SELECT RAISE(ABORT, 'appearance delivery participants are immutable');
END;
CREATE TRIGGER appearance_feedback_control_no_delete
BEFORE DELETE ON appearance_feedback_control BEGIN
    SELECT RAISE(ABORT, 'appearance feedback controls are immutable');
END;
CREATE TRIGGER appearance_feedback_no_delete
BEFORE DELETE ON appearance_feedback BEGIN
    SELECT RAISE(ABORT, 'appearance feedback is append-only');
END;
CREATE TRIGGER chronicle_appearance_source_no_delete BEFORE DELETE ON chronicle_appearance_source
BEGIN SELECT RAISE(ABORT,'Chronicle appearance provenance is immutable'); END;

-- Wall clocks may move backwards. Snapshot journal insertion order once and
-- extend it transactionally for every future event so Tarot eligibility,
-- projections, and integration work have one durable chronology.
CREATE TABLE tarot_event_order (
    sequence_id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT
) STRICT;
INSERT INTO tarot_event_order(event_id)
SELECT event_id FROM event_journal ORDER BY rowid;
CREATE TRIGGER tarot_event_order_immutable BEFORE UPDATE ON tarot_event_order
BEGIN SELECT RAISE(ABORT, 'Tarot event order is immutable'); END;
CREATE TRIGGER tarot_event_order_retained BEFORE DELETE ON tarot_event_order
BEGIN SELECT RAISE(ABORT, 'Tarot event order is retained'); END;
CREATE TRIGGER tarot_event_order_after_event
AFTER INSERT ON event_journal
BEGIN
  INSERT INTO tarot_event_order(event_id) VALUES(NEW.event_id);
END;

CREATE TABLE tarot_catalog_snapshot (
    catalog_version TEXT PRIMARY KEY CHECK (length(catalog_version) BETWEEN 3 AND 80),
    catalog_kind TEXT NOT NULL CHECK (catalog_kind IN ('deck','house')),
    canonical_json TEXT NOT NULL CHECK (json_valid(canonical_json) AND length(CAST(canonical_json AS BLOB)) BETWEEN 2 AND 131072),
    checksum TEXT NOT NULL CHECK (length(checksum) BETWEEN 9 AND 96),
    installed_at_ms INTEGER NOT NULL CHECK (installed_at_ms >= 0),
    UNIQUE(catalog_kind,checksum)
) STRICT;

CREATE TRIGGER tarot_catalog_snapshot_immutable
BEFORE UPDATE ON tarot_catalog_snapshot
BEGIN SELECT RAISE(ABORT, 'Tarot catalog snapshots are immutable'); END;
CREATE TRIGGER tarot_catalog_snapshot_retained
BEFORE DELETE ON tarot_catalog_snapshot
BEGIN SELECT RAISE(ABORT, 'Tarot catalog snapshots are retained'); END;

CREATE TABLE tarot_card_definition (
    catalog_version TEXT NOT NULL REFERENCES tarot_catalog_snapshot(catalog_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    ordinal INTEGER NOT NULL CHECK (ordinal BETWEEN 0 AND 21),
    slug TEXT NOT NULL CHECK (length(slug) BETWEEN 3 AND 48),
    name TEXT NOT NULL CHECK (length(CAST(name AS BLOB)) BETWEEN 3 AND 80),
    meaning TEXT NOT NULL CHECK (length(CAST(meaning AS BLOB)) BETWEEN 10 AND 160),
    theme_tag TEXT NOT NULL CHECK (length(theme_tag) BETWEEN 3 AND 32),
    safety_prompt TEXT NOT NULL CHECK (length(CAST(safety_prompt AS BLOB)) BETWEEN 10 AND 240),
    flavor_json TEXT NOT NULL CHECK (json_valid(flavor_json) AND json_type(flavor_json)='array' AND json_array_length(flavor_json) BETWEEN 1 AND 4),
    PRIMARY KEY(catalog_version,ordinal),
    UNIQUE(catalog_version,slug),
    UNIQUE(catalog_version,theme_tag)
) STRICT;

CREATE TRIGGER tarot_card_definition_immutable
BEFORE UPDATE ON tarot_card_definition
BEGIN SELECT RAISE(ABORT, 'Tarot card definitions are immutable'); END;
CREATE TRIGGER tarot_card_definition_retained
BEFORE DELETE ON tarot_card_definition
BEGIN SELECT RAISE(ABORT, 'Tarot card definitions are retained'); END;

CREATE TABLE tarot_house_template_definition (
    catalog_version TEXT NOT NULL REFERENCES tarot_catalog_snapshot(catalog_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    template_slug TEXT NOT NULL CHECK (length(template_slug) BETWEEN 3 AND 48),
    canonical_json TEXT NOT NULL CHECK (json_valid(canonical_json) AND length(CAST(canonical_json AS BLOB)) BETWEEN 2 AND 8192),
    PRIMARY KEY(catalog_version,template_slug)
) STRICT;

CREATE TRIGGER tarot_house_template_definition_immutable
BEFORE UPDATE ON tarot_house_template_definition
BEGIN SELECT RAISE(ABORT, 'House template definitions are immutable'); END;
CREATE TRIGGER tarot_house_template_definition_retained
BEFORE DELETE ON tarot_house_template_definition
BEGIN SELECT RAISE(ABORT, 'House template definitions are retained'); END;

CREATE TABLE tarot_card_draw (
    draw_id TEXT PRIMARY KEY CHECK (length(draw_id)=36),
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    visibility TEXT NOT NULL CHECK (visibility IN ('public','private')),
    catalog_version TEXT NOT NULL REFERENCES tarot_catalog_snapshot(catalog_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    card_ordinal INTEGER NOT NULL CHECK (card_ordinal BETWEEN 0 AND 21),
    flavor_variant INTEGER NOT NULL CHECK (flavor_variant BETWEEN 0 AND 3),
    drawn_at_ms INTEGER NOT NULL CHECK (drawn_at_ms >= 0),
    cooldown_until_ms INTEGER NOT NULL CHECK (cooldown_until_ms > drawn_at_ms),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    FOREIGN KEY(catalog_version,card_ordinal) REFERENCES tarot_card_definition(catalog_version,ordinal) ON UPDATE RESTRICT ON DELETE RESTRICT
) STRICT;
CREATE INDEX tarot_card_draw_cooldown ON tarot_card_draw(user_id,is_test,drawn_at_ms DESC,cooldown_until_ms);
CREATE INDEX tarot_card_draw_public_event ON tarot_card_draw(visibility,is_test,drawn_at_ms,event_id);

CREATE TABLE tarot_draw_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    draw_id TEXT REFERENCES tarot_card_draw(draw_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    visibility TEXT NOT NULL CHECK (visibility IN ('public','private')),
    status TEXT NOT NULL CHECK (status IN ('drawn','cooldown')),
    cooldown_until_ms INTEGER NOT NULL CHECK (cooldown_until_ms >= 0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;
CREATE INDEX tarot_draw_receipt_user ON tarot_draw_receipt(user_id,created_at_ms DESC);

CREATE TRIGGER tarot_card_draw_immutable BEFORE UPDATE ON tarot_card_draw
BEGIN SELECT RAISE(ABORT, 'Tarot draws are immutable'); END;
CREATE TRIGGER tarot_card_draw_retained BEFORE DELETE ON tarot_card_draw
BEGIN SELECT RAISE(ABORT, 'Tarot draws are retained'); END;
CREATE TRIGGER tarot_draw_receipt_immutable BEFORE UPDATE ON tarot_draw_receipt
BEGIN SELECT RAISE(ABORT, 'Tarot draw receipts are immutable'); END;
CREATE TRIGGER tarot_draw_receipt_retained BEFORE DELETE ON tarot_draw_receipt
BEGIN SELECT RAISE(ABORT, 'Tarot draw receipts are retained'); END;

CREATE TABLE tarot_draw_public_delivery (
    draw_id TEXT PRIMARY KEY REFERENCES tarot_card_draw(draw_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;
CREATE TRIGGER tarot_draw_public_delivery_immutable BEFORE UPDATE ON tarot_draw_public_delivery
BEGIN SELECT RAISE(ABORT, 'Tarot draw deliveries are immutable'); END;
CREATE TRIGGER tarot_draw_public_delivery_retained BEFORE DELETE ON tarot_draw_public_delivery
BEGIN SELECT RAISE(ABORT, 'Tarot draw deliveries are retained'); END;

CREATE TABLE tarot_public_outbox_dependency (
    predecessor_outbox_id TEXT NOT NULL REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    successor_outbox_id TEXT NOT NULL REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    dependency_kind TEXT NOT NULL CHECK (dependency_kind IN ('funded_before_terminal','draw_before_terminal')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(predecessor_outbox_id,successor_outbox_id),
    CHECK (predecessor_outbox_id<>successor_outbox_id)
) STRICT;
CREATE INDEX tarot_public_outbox_dependency_successor
ON tarot_public_outbox_dependency(successor_outbox_id);
CREATE TRIGGER tarot_public_outbox_dependency_immutable
BEFORE UPDATE ON tarot_public_outbox_dependency
BEGIN SELECT RAISE(ABORT, 'Tarot public delivery dependencies are immutable'); END;
CREATE TRIGGER tarot_public_outbox_dependency_retained
BEFORE DELETE ON tarot_public_outbox_dependency
BEGIN SELECT RAISE(ABORT, 'Tarot public delivery dependencies are retained'); END;

CREATE TABLE tarot_house_offer (
    offer_id TEXT PRIMARY KEY CHECK (length(offer_id)=36),
    catalog_version TEXT NOT NULL REFERENCES tarot_catalog_snapshot(catalog_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    template_slug TEXT NOT NULL,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    proposition TEXT NOT NULL CHECK (length(CAST(proposition AS BLOB)) BETWEEN 10 AND 300),
    state TEXT NOT NULL CHECK (state IN ('open','closed','skipped')),
    reserved_profit INTEGER NOT NULL CHECK (reserved_profit BETWEEN 0 AND 20),
    opens_at_ms INTEGER NOT NULL CHECK (opens_at_ms >= 0),
    closes_at_ms INTEGER NOT NULL CHECK (closes_at_ms > opens_at_ms),
    resolution_due_at_ms INTEGER NOT NULL CHECK (resolution_due_at_ms >= closes_at_ms),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    skip_reason TEXT CHECK (skip_reason IS NULL OR length(skip_reason) BETWEEN 1 AND 96),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    FOREIGN KEY(catalog_version,template_slug) REFERENCES tarot_house_template_definition(catalog_version,template_slug) ON UPDATE RESTRICT ON DELETE RESTRICT,
    CHECK ((state='skipped')=(skip_reason IS NOT NULL))
) STRICT;
CREATE INDEX tarot_house_offer_exposure ON tarot_house_offer(is_test,state,reserved_profit) WHERE state='open';
CREATE INDEX tarot_house_offer_schedule ON tarot_house_offer(opens_at_ms,state);
CREATE UNIQUE INDEX tarot_house_offer_slot
ON tarot_house_offer(template_slug,opens_at_ms,is_test);

CREATE TABLE tarot_house_wager (
    wager_id TEXT PRIMARY KEY CHECK (length(wager_id)=36),
    offer_id TEXT REFERENCES tarot_house_offer(offer_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    catalog_version TEXT NOT NULL REFERENCES tarot_catalog_snapshot(catalog_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    template_slug TEXT NOT NULL,
    proposition TEXT NOT NULL CHECK (length(CAST(proposition AS BLOB)) BETWEEN 10 AND 300),
    choice_slug TEXT NOT NULL CHECK (length(choice_slug) BETWEEN 1 AND 32),
    choice_label TEXT NOT NULL CHECK (length(CAST(choice_label AS BLOB)) BETWEEN 1 AND 80),
    odds_numerator INTEGER NOT NULL CHECK (odds_numerator BETWEEN 0 AND 20),
    odds_denominator INTEGER NOT NULL CHECK (odds_denominator BETWEEN 1 AND 20),
    stake INTEGER NOT NULL CHECK (stake BETWEEN 0 AND 100),
    profit INTEGER NOT NULL CHECK (profit BETWEEN 0 AND 20),
    visibility TEXT NOT NULL CHECK (visibility IN ('public','private')),
    authority TEXT NOT NULL CHECK (authority IN ('draw','public_draw','owner')),
    state TEXT NOT NULL CHECK (state IN ('accepted_funded','resolved','void_refunded')),
    result TEXT CHECK (result IS NULL OR result IN ('win','loss','void')),
    terminal_reason TEXT CHECK (terminal_reason IS NULL OR length(CAST(terminal_reason AS BLOB)) BETWEEN 1 AND 200),
    accepted_at_ms INTEGER NOT NULL CHECK (accepted_at_ms >= 0),
    outcome_due_at_ms INTEGER NOT NULL CHECK (outcome_due_at_ms > accepted_at_ms),
    terminal_cooldown_ms INTEGER NOT NULL CHECK (terminal_cooldown_ms BETWEEN 0 AND 2678400000),
    cooldown_until_ms INTEGER NOT NULL CHECK (cooldown_until_ms >= accepted_at_ms),
    terminal_at_ms INTEGER CHECK (terminal_at_ms IS NULL OR terminal_at_ms >= accepted_at_ms),
    recovery INTEGER NOT NULL CHECK (recovery IN (0,1)),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    accepted_event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    terminal_event_id TEXT UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision BETWEEN 1 AND 2),
    FOREIGN KEY(catalog_version,template_slug) REFERENCES tarot_house_template_definition(catalog_version,template_slug) ON UPDATE RESTRICT ON DELETE RESTRICT,
    CHECK ((state='accepted_funded' AND result IS NULL AND terminal_at_ms IS NULL
            AND terminal_event_id IS NULL AND cooldown_until_ms=accepted_at_ms)
        OR (state='resolved' AND result IN ('win','loss') AND terminal_at_ms IS NOT NULL
            AND terminal_event_id IS NOT NULL
            AND cooldown_until_ms=terminal_at_ms+terminal_cooldown_ms)
        OR (state='void_refunded' AND result='void' AND terminal_at_ms IS NOT NULL
            AND terminal_event_id IS NOT NULL
            AND cooldown_until_ms=terminal_at_ms+terminal_cooldown_ms)),
    CHECK (recovery=0 OR (stake=0 AND profit=5 AND authority='draw'))
) STRICT;
CREATE INDEX tarot_house_open_exposure ON tarot_house_wager(is_test,state,profit) WHERE state='accepted_funded';
CREATE INDEX tarot_house_user_cooldown ON tarot_house_wager(user_id,template_slug,is_test,cooldown_until_ms DESC);
CREATE UNIQUE INDEX tarot_house_one_active_user_template
ON tarot_house_wager(user_id,template_slug,is_test)
WHERE state='accepted_funded';
CREATE INDEX tarot_house_wager_deadline_index ON tarot_house_wager(state,outcome_due_at_ms);
CREATE INDEX tarot_house_history ON tarot_house_wager(user_id,accepted_at_ms DESC);

CREATE TABLE tarot_house_action (
    action_id TEXT PRIMARY KEY CHECK (length(action_id)=36),
    wager_id TEXT NOT NULL REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    action_kind TEXT NOT NULL CHECK (action_kind IN ('accepted','automatic_observation','owner_resolution','deadline','cleanup')),
    actor_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    expected_revision INTEGER NOT NULL CHECK (expected_revision BETWEEN 1 AND 2),
    reason TEXT CHECK (reason IS NULL OR length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_house_resolution (
    resolution_id TEXT PRIMARY KEY CHECK (length(resolution_id)=36),
    wager_id TEXT NOT NULL UNIQUE REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    result TEXT NOT NULL CHECK (result IN ('win','loss','void')),
    authority TEXT NOT NULL CHECK (authority IN ('draw','public_draw','owner','deadline')),
    actor_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT NOT NULL CHECK (length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_house_transfer (
    transfer_id TEXT PRIMARY KEY CHECK (length(transfer_id)=36),
    wager_id TEXT NOT NULL REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    transfer_kind TEXT NOT NULL CHECK (transfer_kind IN ('fund','payout','refund','test_cleanup')),
    transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE(wager_id,transfer_kind)
) STRICT;

CREATE TABLE tarot_house_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    wager_id TEXT REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    operation TEXT NOT NULL CHECK (length(operation) BETWEEN 1 AND 40),
    status TEXT NOT NULL CHECK (status IN ('applied','replay','not_found','forbidden','ineligible','cooldown','insufficient_funds','exposure_blocked','invalid_state')),
    request_fingerprint TEXT CHECK (request_fingerprint IS NULL OR length(CAST(request_fingerprint AS BLOB)) BETWEEN 1 AND 512),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_house_control (
    token_id TEXT PRIMARY KEY CHECK (length(token_id)=36),
    offer_id TEXT REFERENCES tarot_house_offer(offer_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    wager_id TEXT REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    expected_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    expected_revision INTEGER NOT NULL CHECK (expected_revision BETWEEN 1 AND 2),
    action TEXT NOT NULL CHECK (action IN ('claim','play','history','resolve')),
    state TEXT NOT NULL CHECK (state IN ('active','used','cancelled')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms),
    CHECK ((offer_id IS NULL)<>(wager_id IS NULL))
) STRICT;

CREATE TABLE tarot_house_deadline (
    wager_id TEXT PRIMARY KEY REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    job_id TEXT UNIQUE REFERENCES scheduled_job(job_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    phase TEXT NOT NULL CHECK (phase IN ('outcome','manual_grace')),
    expected_revision INTEGER NOT NULL CHECK (expected_revision BETWEEN 1 AND 2),
    due_at_ms INTEGER NOT NULL CHECK (due_at_ms >= 0)
) STRICT;
CREATE INDEX tarot_house_deadline_due_index ON tarot_house_deadline(due_at_ms);

CREATE TABLE tarot_house_offer_deadline (
    offer_id TEXT PRIMARY KEY REFERENCES tarot_house_offer(offer_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    job_id TEXT NOT NULL UNIQUE REFERENCES scheduled_job(job_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    due_at_ms INTEGER NOT NULL CHECK (due_at_ms >= 0)
) STRICT;
CREATE INDEX tarot_house_offer_deadline_due_index ON tarot_house_offer_deadline(due_at_ms);

CREATE TABLE tarot_house_public_card (
    offer_id TEXT PRIMARY KEY REFERENCES tarot_house_offer(offer_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    create_outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reminder_outbox_id TEXT UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    terminal_edit_outbox_id TEXT UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    outcome_outbox_id TEXT UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_revision INTEGER NOT NULL CHECK (created_revision >= 1)
) STRICT;

CREATE TABLE tarot_house_offer_delivery_failure (
    offer_id TEXT PRIMARY KEY REFERENCES tarot_house_offer(offer_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    terminal_state TEXT NOT NULL CHECK (terminal_state IN ('failed','dead','cancelled')),
    error_code TEXT CHECK (error_code IS NULL OR length(CAST(error_code AS BLOB)) BETWEEN 1 AND 160),
    recorded_at_ms INTEGER NOT NULL CHECK (recorded_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_house_test_cleanup (
    wager_id TEXT NOT NULL REFERENCES tarot_house_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    original_transaction_id TEXT NOT NULL REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reversal_transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT NOT NULL CHECK (length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(wager_id,original_transaction_id)
) STRICT;

CREATE TRIGGER tarot_house_offer_transition
BEFORE UPDATE ON tarot_house_offer
WHEN OLD.offer_id<>NEW.offer_id OR OLD.catalog_version<>NEW.catalog_version OR OLD.template_slug<>NEW.template_slug
  OR OLD.guild_id<>NEW.guild_id OR OLD.channel_id<>NEW.channel_id OR OLD.proposition<>NEW.proposition
  OR OLD.reserved_profit<>NEW.reserved_profit OR OLD.opens_at_ms<>NEW.opens_at_ms OR OLD.closes_at_ms<>NEW.closes_at_ms
  OR OLD.resolution_due_at_ms<>NEW.resolution_due_at_ms OR OLD.is_test<>NEW.is_test OR OLD.created_at_ms<>NEW.created_at_ms
  OR OLD.state<>'open' OR NEW.state NOT IN ('closed','skipped')
BEGIN SELECT RAISE(ABORT, 'House offer transition is invalid'); END;
CREATE TRIGGER tarot_house_offer_retained BEFORE DELETE ON tarot_house_offer
BEGIN SELECT RAISE(ABORT, 'House offers are retained'); END;
CREATE TRIGGER tarot_house_wager_transition
BEFORE UPDATE ON tarot_house_wager
WHEN OLD.wager_id<>NEW.wager_id OR OLD.offer_id IS NOT NEW.offer_id OR OLD.user_id<>NEW.user_id
  OR OLD.guild_id<>NEW.guild_id OR OLD.channel_id<>NEW.channel_id OR OLD.catalog_version<>NEW.catalog_version
  OR OLD.template_slug<>NEW.template_slug OR OLD.proposition<>NEW.proposition OR OLD.choice_slug<>NEW.choice_slug
  OR OLD.choice_label<>NEW.choice_label OR OLD.odds_numerator<>NEW.odds_numerator OR OLD.odds_denominator<>NEW.odds_denominator
  OR OLD.stake<>NEW.stake OR OLD.profit<>NEW.profit OR OLD.visibility<>NEW.visibility OR OLD.authority<>NEW.authority
  OR OLD.accepted_at_ms<>NEW.accepted_at_ms OR OLD.outcome_due_at_ms<>NEW.outcome_due_at_ms
  OR OLD.terminal_cooldown_ms<>NEW.terminal_cooldown_ms
  OR NEW.cooldown_until_ms<>NEW.terminal_at_ms+NEW.terminal_cooldown_ms
  OR OLD.recovery<>NEW.recovery OR OLD.is_test<>NEW.is_test
  OR OLD.accepted_event_id<>NEW.accepted_event_id OR OLD.state<>'accepted_funded'
  OR NEW.state NOT IN ('resolved','void_refunded') OR NEW.revision<>2
BEGIN SELECT RAISE(ABORT, 'House wager transition is invalid'); END;
CREATE TRIGGER tarot_house_wager_retained BEFORE DELETE ON tarot_house_wager
BEGIN SELECT RAISE(ABORT, 'House wagers are retained'); END;
CREATE TRIGGER tarot_house_transfer_immutable BEFORE UPDATE ON tarot_house_transfer
BEGIN SELECT RAISE(ABORT, 'House transfers are immutable'); END;
CREATE TRIGGER tarot_house_transfer_retained BEFORE DELETE ON tarot_house_transfer
BEGIN SELECT RAISE(ABORT, 'House transfers are retained'); END;
CREATE TRIGGER tarot_house_resolution_immutable BEFORE UPDATE ON tarot_house_resolution
BEGIN SELECT RAISE(ABORT, 'House resolutions are immutable'); END;
CREATE TRIGGER tarot_house_resolution_retained BEFORE DELETE ON tarot_house_resolution
BEGIN SELECT RAISE(ABORT, 'House resolutions are retained'); END;
CREATE TRIGGER tarot_house_action_immutable BEFORE UPDATE ON tarot_house_action
BEGIN SELECT RAISE(ABORT, 'House actions are immutable'); END;
CREATE TRIGGER tarot_house_action_retained BEFORE DELETE ON tarot_house_action
BEGIN SELECT RAISE(ABORT, 'House actions are retained'); END;
CREATE TRIGGER tarot_house_receipt_immutable BEFORE UPDATE ON tarot_house_receipt
BEGIN SELECT RAISE(ABORT, 'House receipts are immutable'); END;
CREATE TRIGGER tarot_house_receipt_retained BEFORE DELETE ON tarot_house_receipt
BEGIN SELECT RAISE(ABORT, 'House receipts are retained'); END;
CREATE TRIGGER tarot_house_control_transition
BEFORE UPDATE ON tarot_house_control
WHEN OLD.token_id<>NEW.token_id OR OLD.offer_id IS NOT NEW.offer_id
  OR OLD.wager_id IS NOT NEW.wager_id
  OR OLD.expected_user_id IS NOT NEW.expected_user_id
  OR OLD.expected_revision<>NEW.expected_revision OR OLD.action<>NEW.action
  OR OLD.created_at_ms<>NEW.created_at_ms OR OLD.expires_at_ms<>NEW.expires_at_ms
  OR OLD.state<>'active' OR NEW.state NOT IN ('used','cancelled')
BEGIN SELECT RAISE(ABORT, 'House control transition is invalid'); END;
CREATE TRIGGER tarot_house_control_retained BEFORE DELETE ON tarot_house_control
BEGIN SELECT RAISE(ABORT, 'House controls are retained'); END;
CREATE TRIGGER tarot_house_deadline_immutable BEFORE UPDATE ON tarot_house_deadline
BEGIN SELECT RAISE(ABORT, 'House deadlines are immutable'); END;
CREATE TRIGGER tarot_house_deadline_retained BEFORE DELETE ON tarot_house_deadline
BEGIN SELECT RAISE(ABORT, 'House deadlines are retained'); END;
CREATE TRIGGER tarot_house_offer_deadline_immutable BEFORE UPDATE ON tarot_house_offer_deadline
BEGIN SELECT RAISE(ABORT, 'House offer deadlines are immutable'); END;
CREATE TRIGGER tarot_house_offer_deadline_retained BEFORE DELETE ON tarot_house_offer_deadline
BEGIN SELECT RAISE(ABORT, 'House offer deadlines are retained'); END;
CREATE TRIGGER tarot_house_public_card_append_only
BEFORE UPDATE ON tarot_house_public_card
WHEN OLD.offer_id<>NEW.offer_id OR OLD.create_outbox_id<>NEW.create_outbox_id
  OR OLD.created_revision<>NEW.created_revision
  OR (OLD.reminder_outbox_id IS NOT NULL AND OLD.reminder_outbox_id IS NOT NEW.reminder_outbox_id)
  OR (OLD.terminal_edit_outbox_id IS NOT NULL AND OLD.terminal_edit_outbox_id IS NOT NEW.terminal_edit_outbox_id)
  OR (OLD.outcome_outbox_id IS NOT NULL AND OLD.outcome_outbox_id IS NOT NEW.outcome_outbox_id)
BEGIN SELECT RAISE(ABORT, 'House public-card audit is append-only'); END;
CREATE TRIGGER tarot_house_public_card_retained BEFORE DELETE ON tarot_house_public_card
BEGIN SELECT RAISE(ABORT, 'House public cards are retained'); END;
CREATE TRIGGER tarot_house_offer_delivery_failure_immutable
BEFORE UPDATE ON tarot_house_offer_delivery_failure
BEGIN SELECT RAISE(ABORT, 'House offer delivery failures are immutable'); END;
CREATE TRIGGER tarot_house_offer_delivery_failure_retained
BEFORE DELETE ON tarot_house_offer_delivery_failure
BEGIN SELECT RAISE(ABORT, 'House offer delivery failures are retained'); END;
CREATE TRIGGER tarot_house_test_cleanup_immutable BEFORE UPDATE ON tarot_house_test_cleanup
BEGIN SELECT RAISE(ABORT, 'House test cleanup is immutable'); END;
CREATE TRIGGER tarot_house_test_cleanup_retained BEFORE DELETE ON tarot_house_test_cleanup
BEGIN SELECT RAISE(ABORT, 'House test cleanup is retained'); END;

CREATE TABLE tarot_player_stats (
    user_id TEXT PRIMARY KEY REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    wins INTEGER NOT NULL DEFAULT 0 CHECK (wins >= 0),
    losses INTEGER NOT NULL DEFAULT 0 CHECK (losses >= 0),
    current_win_streak INTEGER NOT NULL DEFAULT 0 CHECK (current_win_streak >= 0),
    current_loss_streak INTEGER NOT NULL DEFAULT 0 CHECK (current_loss_streak >= 0),
    settled_house_wagers INTEGER NOT NULL DEFAULT 0 CHECK (settled_house_wagers >= 0),
    last_event_id TEXT REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    rebuilt_at_ms INTEGER NOT NULL CHECK (rebuilt_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_player_event (
    source_event_id TEXT NOT NULL REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    result TEXT NOT NULL CHECK (result IN ('win','loss','void')),
    wager_kind TEXT NOT NULL CHECK (wager_kind IN ('peer','house')),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    baseline INTEGER NOT NULL DEFAULT 0 CHECK (baseline IN (0,1)),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0),
    PRIMARY KEY(source_event_id,user_id)
) STRICT;
CREATE INDEX tarot_player_event_rebuild ON tarot_player_event(user_id,source_event_id);

-- Accepted M12 results seed the rebuildable projection as baseline only. The
-- integration observation trigger is created below, so this copy cannot emit
-- retroactive Chronicle, relationship, appearance, narration, or title work.
INSERT INTO tarot_player_event(source_event_id,user_id,result,wager_kind,is_test,
  baseline,occurred_at_ms)
SELECT resolution.event_id,wager.creator_user_id,
  CASE resolution.result WHEN 'creator' THEN 'win'
       WHEN 'target' THEN 'loss' ELSE 'void' END,
  'peer',0,1,resolution.created_at_ms
FROM tarot_wager_resolution resolution
JOIN tarot_wager wager ON wager.wager_id=resolution.wager_id
WHERE wager.is_test=0
UNION ALL
SELECT resolution.event_id,wager.target_user_id,
  CASE resolution.result WHEN 'target' THEN 'win'
       WHEN 'creator' THEN 'loss' ELSE 'void' END,
  'peer',0,1,resolution.created_at_ms
FROM tarot_wager_resolution resolution
JOIN tarot_wager wager ON wager.wager_id=resolution.wager_id
WHERE wager.is_test=0;

WITH ordered AS (
  SELECT player.user_id,player.result,player.source_event_id,
    player.occurred_at_ms,event_order.sequence_id,
    row_number() OVER(PARTITION BY user_id
      ORDER BY event_order.sequence_id DESC) AS recent_rank
  FROM tarot_player_event player
  JOIN tarot_event_order event_order ON event_order.event_id=player.source_event_id
  WHERE player.result IN ('win','loss')
), decisive_totals AS (
  SELECT user_id,
    sum(result='win') AS wins,
    sum(result='loss') AS losses,
    max(CASE WHEN recent_rank=1 THEN result END) AS latest_result,
    COALESCE(min(CASE WHEN result='loss' THEN recent_rank END)-1,count(*))
      AS leading_wins,
    COALESCE(min(CASE WHEN result='win' THEN recent_rank END)-1,count(*))
      AS leading_losses
  FROM ordered GROUP BY user_id
), all_totals AS (
  SELECT player.user_id,sum(player.wager_kind='house') AS settled_house_wagers,
    max(player.occurred_at_ms) AS rebuilt_at_ms
  FROM tarot_player_event player GROUP BY player.user_id
)
INSERT INTO tarot_player_stats(user_id,wins,losses,current_win_streak,
  current_loss_streak,settled_house_wagers,last_event_id,rebuilt_at_ms)
SELECT all_totals.user_id,COALESCE(decisive_totals.wins,0),
  COALESCE(decisive_totals.losses,0),
  CASE decisive_totals.latest_result WHEN 'win'
    THEN decisive_totals.leading_wins ELSE 0 END,
  CASE decisive_totals.latest_result WHEN 'loss'
    THEN decisive_totals.leading_losses ELSE 0 END,
  all_totals.settled_house_wagers,
  (SELECT latest.source_event_id FROM tarot_player_event latest
   JOIN tarot_event_order latest_order
     ON latest_order.event_id=latest.source_event_id
   WHERE latest.user_id=all_totals.user_id
   ORDER BY latest_order.sequence_id DESC LIMIT 1),
  all_totals.rebuilt_at_ms
FROM all_totals LEFT JOIN decisive_totals
  ON decisive_totals.user_id=all_totals.user_id;

CREATE TABLE tarot_title_source (
    source_event_id TEXT NOT NULL REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    title_name TEXT NOT NULL CHECK (title_name IN ('Favored of the Cast Die','Bearer of the Returning Dawn','Keeper of the Last Standard')),
    title_definition_id TEXT UNIQUE,
    state TEXT NOT NULL CHECK (state IN ('proposed','approved','rejected','revoked','suppressed')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(source_event_id,user_id,title_name),
    UNIQUE(user_id,title_name)
) STRICT;

CREATE TRIGGER tarot_title_source_after_grant_state
AFTER UPDATE OF state ON chronicle_title_grant
WHEN OLD.state<>NEW.state
BEGIN
  UPDATE tarot_title_source SET state=CASE NEW.state
    WHEN 'proposed' THEN 'proposed'
    WHEN 'active' THEN 'approved'
    WHEN 'rejected' THEN 'rejected'
    WHEN 'revoked' THEN 'revoked'
  END
  WHERE title_definition_id=NEW.definition_id;
END;

CREATE TABLE tarot_integration_observation (
    source_event_id TEXT PRIMARY KEY REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_type TEXT NOT NULL CHECK (length(event_type) BETWEEN 1 AND 96),
    visibility TEXT NOT NULL CHECK (visibility IN ('public','private','sealed')),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    state TEXT NOT NULL CHECK (state IN ('pending','processing','complete','suppressed','failed')),
    attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    next_attempt_at_ms INTEGER NOT NULL CHECK (next_attempt_at_ms >= 0),
    last_error TEXT CHECK (last_error IS NULL OR length(last_error) BETWEEN 1 AND 500),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    processed_at_ms INTEGER CHECK (processed_at_ms IS NULL OR processed_at_ms >= created_at_ms)
) STRICT;
CREATE INDEX tarot_integration_work ON tarot_integration_observation(state,next_attempt_at_ms,created_at_ms);

CREATE TRIGGER tarot_integration_observe_peer_settlement
AFTER INSERT ON event_journal
WHEN NEW.event_type IN ('tarot.wager_resolved.v1','tarot.wager_voided.v1')
BEGIN
  INSERT INTO tarot_integration_observation(source_event_id,event_type,visibility,
    is_test,state,attempts,next_attempt_at_ms,last_error,created_at_ms,processed_at_ms)
  SELECT NEW.event_id,NEW.event_type,
    CASE wager.visibility WHEN 'public' THEN 'public' ELSE 'sealed' END,
    wager.is_test,'pending',0,NEW.recorded_at_ms,NULL,NEW.recorded_at_ms,NULL
  FROM tarot_wager wager WHERE wager.wager_id=NEW.aggregate_id;
END;

CREATE TABLE tarot_integration_effect_receipt (
    source_event_id TEXT NOT NULL REFERENCES tarot_integration_observation(source_event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    sink_kind TEXT NOT NULL CHECK (sink_kind IN ('stats','title','chronicle','relationship','appearance','vox_intent')),
    sink_key TEXT NOT NULL CHECK (length(sink_key) BETWEEN 1 AND 160),
    sink_reference TEXT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(source_event_id,sink_kind,sink_key)
) STRICT;

CREATE TABLE tarot_vox_narration_intent (
    intent_id TEXT PRIMARY KEY CHECK (length(intent_id)=36),
    source_event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    public_safe_text TEXT NOT NULL CHECK (length(CAST(public_safe_text AS BLOB)) BETWEEN 1 AND 500),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','expired')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms=created_at_ms+86400000)
) STRICT;
CREATE INDEX tarot_vox_intent_expiry ON tarot_vox_narration_intent(state,expires_at_ms);

CREATE TABLE tarot_chronicle_proposal (
    proposal_id TEXT PRIMARY KEY CHECK (length(proposal_id)=36),
    source_event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    proposer_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    title TEXT NOT NULL CHECK (length(CAST(title AS BLOB)) BETWEEN 1 AND 100),
    body TEXT NOT NULL CHECK (length(CAST(body AS BLOB)) BETWEEN 1 AND 1000),
    status TEXT NOT NULL DEFAULT 'submitted' CHECK (status IN ('submitted','approved','rejected')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

-- The accepted Chronicle approval workflow remains authoritative. This table
-- is an integration audit/projection only, so it follows the existing approval
-- row instead of introducing a second decision path.
CREATE TRIGGER tarot_chronicle_proposal_after_approval
AFTER UPDATE OF state ON chronicle_approval
WHEN NEW.state='approved'
  AND EXISTS(SELECT 1 FROM tarot_chronicle_proposal
             WHERE proposal_id=NEW.entry_id AND status='submitted')
BEGIN
  UPDATE tarot_chronicle_proposal SET status='approved'
  WHERE proposal_id=NEW.entry_id AND status='submitted';
END;

CREATE TRIGGER tarot_chronicle_proposal_after_decline
AFTER UPDATE OF state ON chronicle_approval
WHEN NEW.state='declined'
  AND EXISTS(SELECT 1 FROM tarot_chronicle_proposal
             WHERE proposal_id=NEW.entry_id AND status='submitted')
BEGIN
  UPDATE tarot_chronicle_proposal SET status='rejected'
  WHERE proposal_id=NEW.entry_id AND status='submitted';
END;

CREATE TABLE tarot_appearance_candidate (
    candidate_id TEXT PRIMARY KEY CHECK (length(candidate_id)=36),
    source_event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    candidate_type TEXT NOT NULL CHECK (candidate_type='tarot_event'),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    safe_summary TEXT NOT NULL CHECK (length(CAST(safe_summary AS BLOB)) BETWEEN 1 AND 500),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','suppressed','consumed')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms=created_at_ms+86400000)
) STRICT;
CREATE INDEX tarot_appearance_candidate_work ON tarot_appearance_candidate(state,expires_at_ms);
CREATE TRIGGER tarot_appearance_candidate_transition_only
BEFORE UPDATE ON tarot_appearance_candidate
WHEN OLD.candidate_id<>NEW.candidate_id
  OR OLD.source_event_id<>NEW.source_event_id
  OR OLD.candidate_type<>NEW.candidate_type
  OR OLD.actor_user_id<>NEW.actor_user_id
  OR OLD.guild_id<>NEW.guild_id
  OR OLD.channel_id<>NEW.channel_id
  OR OLD.safe_summary<>NEW.safe_summary
  OR OLD.is_test<>NEW.is_test
  OR OLD.created_at_ms<>NEW.created_at_ms
  OR OLD.expires_at_ms<>NEW.expires_at_ms
  OR OLD.state<>'pending'
  OR NEW.state NOT IN ('suppressed','consumed')
BEGIN
  SELECT RAISE(ABORT, 'Tarot appearance candidate transition is invalid');
END;
CREATE TRIGGER tarot_appearance_candidate_no_delete
BEFORE DELETE ON tarot_appearance_candidate BEGIN
  SELECT RAISE(ABORT, 'Tarot appearance candidate audit is immutable');
END;

-- Consent withdrawal and final delivery gates must cover every subject whose
-- Chronicle or Tarot context grounds an appearance, not only recent speakers.
DROP VIEW appearance_candidate_source_user;
CREATE VIEW appearance_candidate_source_user AS
SELECT cs.candidate_id,cp.user_id FROM appearance_candidate_source cs
JOIN chronicle_participant cp ON cp.entry_id=cs.source_id
WHERE cs.source_kind='chronicle_entry'
UNION
SELECT cs.candidate_id,cp.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_participant cp ON cp.entry_id=o.aggregate_id
WHERE o.event_type IN ('chronicle.entry_canonized.v1','chronicle.anniversary_delivered.v1')
UNION
SELECT cs.candidate_id,sp.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_session_participant sp ON sp.session_id=o.aggregate_id
WHERE o.event_type IN ('chronicle.session_started.v1','chronicle.session_completed.v1')
UNION
SELECT cs.candidate_id,tg.recipient_user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN chronicle_title_grant tg ON tg.grant_id=o.aggregate_id
WHERE o.event_type='chronicle.title_awarded.v1'
UNION
SELECT cs.candidate_id,draw.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN tarot_card_draw draw ON draw.draw_id=o.aggregate_id
WHERE o.event_type='tarot.draw_created.v1'
UNION
SELECT cs.candidate_id,wager.user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN tarot_house_wager wager ON wager.wager_id=o.aggregate_id
WHERE o.event_type IN ('tarot.house_resolved.v1','tarot.house_voided.v1')
UNION
SELECT cs.candidate_id,wager.creator_user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN tarot_wager wager ON wager.wager_id=o.aggregate_id
WHERE o.event_type IN ('tarot.wager_resolved.v1','tarot.wager_voided.v1')
UNION
SELECT cs.candidate_id,wager.target_user_id FROM appearance_candidate_source cs
JOIN appearance_event_observation o ON cs.source_kind='event' AND o.source_event_id=cs.source_id
JOIN tarot_wager wager ON wager.wager_id=o.aggregate_id
WHERE o.event_type IN ('tarot.wager_resolved.v1','tarot.wager_voided.v1');

-- Tarot events are no longer categorically unsupported by the appearance
-- subsystem. Integration applies the privacy/consent policy before recording
-- an appearance effect.
DROP TRIGGER appearance_audit_unsupported_event;
CREATE TRIGGER appearance_audit_unsupported_vox_event
AFTER INSERT ON event_journal
WHEN NEW.event_type LIKE 'vox.%'
BEGIN
    INSERT INTO appearance_event_observation
      (source_event_id,event_type,aggregate_type,aggregate_id,guild_id,channel_id,
       actor_user_id,occurred_at_ms,recorded_at_ms,extraction_result,processed_at_ms)
    VALUES
      (NEW.event_id,NEW.event_type,NEW.aggregate_type,NEW.aggregate_id,NEW.guild_id,
       NEW.channel_id,NEW.actor_user_id,NEW.occurred_at_ms,NEW.recorded_at_ms,
       'source_not_enabled',NEW.recorded_at_ms);
END;
CREATE TRIGGER appearance_audit_unsupported_tarot_event
AFTER INSERT ON event_journal
WHEN NEW.event_type LIKE 'tarot.%'
 AND NEW.event_type NOT IN ('tarot.draw_created.v1','tarot.house_resolved.v1',
                            'tarot.house_voided.v1','tarot.wager_resolved.v1',
                            'tarot.wager_voided.v1')
BEGIN
    INSERT INTO appearance_event_observation
      (source_event_id,event_type,aggregate_type,aggregate_id,guild_id,channel_id,
       actor_user_id,occurred_at_ms,recorded_at_ms,extraction_result,processed_at_ms)
    VALUES
      (NEW.event_id,NEW.event_type,NEW.aggregate_type,NEW.aggregate_id,NEW.guild_id,
       NEW.channel_id,NEW.actor_user_id,NEW.occurred_at_ms,NEW.recorded_at_ms,
       'source_not_enabled',NEW.recorded_at_ms);
END;

-- Widen the accepted v10 ledger seal without creating House-only transaction
-- types. A WAGER_* transaction must link to exactly one peer or House owner and
-- satisfy that owner's transfer shape before it can commit.
DROP TRIGGER tarot_transaction_seal;
CREATE TRIGGER tarot_transaction_seal
BEFORE UPDATE ON tarot_transaction
WHEN OLD.state='prepared' AND NEW.state='committed'
BEGIN
    SELECT CASE WHEN NEW.transaction_id<>OLD.transaction_id
      OR NEW.ledger_sequence<>OLD.ledger_sequence
      OR NEW.transaction_type<>OLD.transaction_type
      OR NEW.expected_posting_count<>OLD.expected_posting_count
      OR NEW.event_id<>OLD.event_id OR NEW.idempotency_key<>OLD.idempotency_key
      OR NEW.actor_user_id IS NOT OLD.actor_user_id OR NEW.reason IS NOT OLD.reason
      OR NEW.is_test<>OLD.is_test
      OR NEW.reversal_of_transaction_id IS NOT OLD.reversal_of_transaction_id
      OR NEW.created_at_ms<>OLD.created_at_ms
      THEN RAISE(ABORT,'tarot transaction metadata is immutable') END;
    SELECT CASE WHEN (SELECT count(*) FROM tarot_posting
      WHERE transaction_id=OLD.transaction_id)<>OLD.expected_posting_count
      THEN RAISE(ABORT,'tarot posting count mismatch') END;
    SELECT CASE WHEN (SELECT total(amount) FROM tarot_posting
      WHERE transaction_id=OLD.transaction_id)<>0.0
      THEN RAISE(ABORT,'tarot transaction is unbalanced') END;
    SELECT CASE WHEN OLD.transaction_type NOT LIKE 'WAGER_%'
      AND OLD.transaction_type<>'TEST_REVERSAL' AND OLD.expected_posting_count<>2
      THEN RAISE(ABORT,'tarot transaction shape is invalid') END;
    SELECT CASE WHEN OLD.transaction_type='WAGER_ESCROW_FUND'
      AND OLD.expected_posting_count NOT IN (2,3)
      THEN RAISE(ABORT,'wager funding posting count is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('TEST_ADJUSTMENT','TEST_REVERSAL')
      AND (OLD.actor_user_id IS NULL OR OLD.reason IS NULL
        OR length(trim(OLD.reason,char(9)||char(10)||char(11)||char(12)||char(13)||' '))=0
        OR NOT EXISTS(
          SELECT 1 FROM event_journal event
          JOIN tarot_posting human_post ON human_post.transaction_id=OLD.transaction_id
          JOIN tarot_account human_account ON human_account.account_id=human_post.account_id
          WHERE event.event_id=OLD.event_id
            AND event.event_type=CASE OLD.transaction_type
              WHEN 'TEST_ADJUSTMENT' THEN 'tarot.admin_adjusted.v1'
              ELSE 'tarot.transaction_reversed.v1' END
            AND event.aggregate_type='tarot_transaction'
            AND event.aggregate_id=OLD.transaction_id
            AND event.actor_user_id=OLD.actor_user_id
            AND human_account.account_kind='HUMAN'
            AND human_account.user_id=OLD.actor_user_id)
        AND NOT (OLD.transaction_type='TEST_REVERSAL' AND EXISTS(
          SELECT 1 FROM tarot_house_test_cleanup cleanup
          JOIN tarot_house_wager wager ON wager.wager_id=cleanup.wager_id
          WHERE cleanup.reversal_transaction_id=OLD.transaction_id
            AND wager.user_id=OLD.actor_user_id)))
      THEN RAISE(ABORT,'tarot test transaction audit provenance is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('STARTING_GRANT','GRACE','TRIAL')
      AND (OLD.expected_posting_count<>2 OR NOT EXISTS(
        SELECT 1 FROM tarot_posting human_post
        JOIN tarot_account human_account ON human_account.account_id=human_post.account_id
        JOIN tarot_posting mint_post ON mint_post.transaction_id=human_post.transaction_id
          AND mint_post.account_id<>human_post.account_id
        JOIN tarot_account mint_account ON mint_account.account_id=mint_post.account_id
        WHERE human_post.transaction_id=OLD.transaction_id
          AND human_account.account_kind='HUMAN' AND mint_account.account_kind='MINT'
          AND human_post.amount>0 AND mint_post.amount=-human_post.amount))
      THEN RAISE(ABORT,'tarot grant shape is invalid') END;
    SELECT CASE WHEN OLD.transaction_type='STARTING_GRANT' AND EXISTS(
      SELECT 1 FROM tarot_posting candidate
      JOIN tarot_account account ON account.account_id=candidate.account_id
      JOIN tarot_posting prior ON prior.account_id=candidate.account_id
        AND prior.transaction_id<>candidate.transaction_id
      JOIN tarot_transaction prior_tx ON prior_tx.transaction_id=prior.transaction_id
      WHERE candidate.transaction_id=OLD.transaction_id
        AND account.account_kind='HUMAN'
        AND prior_tx.transaction_type='STARTING_GRANT' AND prior_tx.state='committed')
      THEN RAISE(ABORT,'tarot starting grant already exists') END;
    SELECT CASE WHEN OLD.transaction_type='TEST_ADJUSTMENT' AND NOT EXISTS(
      SELECT 1 FROM tarot_posting human_post
      JOIN tarot_account human_account ON human_account.account_id=human_post.account_id
      JOIN tarot_posting system_post ON system_post.transaction_id=human_post.transaction_id
        AND system_post.account_id<>human_post.account_id
      JOIN tarot_account system_account ON system_account.account_id=system_post.account_id
      WHERE human_post.transaction_id=OLD.transaction_id
        AND human_account.account_kind='HUMAN'
        AND ((human_post.amount>0 AND system_account.account_kind='MINT')
          OR (human_post.amount<0 AND system_account.account_kind='BURN'))
        AND system_post.amount=-human_post.amount)
      THEN RAISE(ABORT,'tarot adjustment shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type='TEST_REVERSAL'
      AND ((SELECT state FROM tarot_transaction
            WHERE transaction_id=OLD.reversal_of_transaction_id)<>'committed'
        OR (SELECT is_test FROM tarot_transaction
            WHERE transaction_id=OLD.reversal_of_transaction_id)<>1
        OR (SELECT transaction_type FROM tarot_transaction
            WHERE transaction_id=OLD.reversal_of_transaction_id) IN ('STARTING_GRANT','TEST_REVERSAL')
        OR ((SELECT transaction_type FROM tarot_transaction
             WHERE transaction_id=OLD.reversal_of_transaction_id)<>'TEST_ADJUSTMENT'
          AND NOT EXISTS(SELECT 1 FROM tarot_wager_test_cleanup
            WHERE reversal_transaction_id=OLD.transaction_id
              AND original_transaction_id=OLD.reversal_of_transaction_id)
          AND NOT EXISTS(SELECT 1 FROM tarot_house_test_cleanup
            WHERE reversal_transaction_id=OLD.transaction_id
              AND original_transaction_id=OLD.reversal_of_transaction_id))
        OR OLD.expected_posting_count<>(SELECT expected_posting_count
          FROM tarot_transaction WHERE transaction_id=OLD.reversal_of_transaction_id)
        OR (SELECT count(*) FROM tarot_posting original
          JOIN tarot_posting inverse ON inverse.transaction_id=OLD.transaction_id
            AND inverse.account_id=original.account_id
            AND inverse.amount=-original.amount
          WHERE original.transaction_id=OLD.reversal_of_transaction_id)
            <>OLD.expected_posting_count)
      THEN RAISE(ABORT,'tarot reversal shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type LIKE 'WAGER_%'
      AND ((EXISTS(SELECT 1 FROM tarot_wager_transfer
                    WHERE transaction_id=OLD.transaction_id)
            + EXISTS(SELECT 1 FROM tarot_house_transfer
                      WHERE transaction_id=OLD.transaction_id))<>1)
      THEN RAISE(ABORT,'wager transfer owner linkage is invalid') END;

    SELECT CASE WHEN OLD.transaction_type LIKE 'WAGER_%'
      AND NOT EXISTS(
        SELECT 1 FROM tarot_wager_transfer transfer
        JOIN tarot_wager wager ON wager.wager_id=transfer.wager_id
        JOIN event_journal event ON event.event_id=OLD.event_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND transfer.transfer_kind=CASE OLD.transaction_type
            WHEN 'WAGER_ESCROW_FUND' THEN 'fund'
            WHEN 'WAGER_PAYOUT' THEN 'payout' ELSE 'refund' END
          AND wager.is_test=OLD.is_test AND event.aggregate_type='tarot_wager'
          AND event.aggregate_id=wager.wager_id
          AND event.event_type=CASE OLD.transaction_type
            WHEN 'WAGER_ESCROW_FUND' THEN 'tarot.wager_funded.v1'
            WHEN 'WAGER_PAYOUT' THEN 'tarot.wager_resolved.v1'
            ELSE 'tarot.wager_voided.v1' END)
      AND NOT EXISTS(
        SELECT 1 FROM tarot_house_transfer transfer
        JOIN tarot_house_wager wager ON wager.wager_id=transfer.wager_id
        JOIN event_journal event ON event.event_id=OLD.event_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND transfer.transfer_kind=CASE OLD.transaction_type
            WHEN 'WAGER_ESCROW_FUND' THEN 'fund'
            WHEN 'WAGER_PAYOUT' THEN 'payout' ELSE 'refund' END
          AND wager.is_test=OLD.is_test
          AND event.aggregate_type='tarot_house_wager'
          AND event.aggregate_id=wager.wager_id
          AND event.event_type=CASE OLD.transaction_type
            WHEN 'WAGER_ESCROW_FUND' THEN 'tarot.house_funded.v1'
            WHEN 'WAGER_PAYOUT' THEN 'tarot.house_resolved.v1'
            ELSE 'tarot.house_voided.v1' END)
      THEN RAISE(ABORT,'wager transfer audit linkage is invalid') END;

    SELECT CASE WHEN OLD.transaction_type='WAGER_ESCROW_FUND'
      AND EXISTS(SELECT 1 FROM tarot_wager_transfer
                 WHERE transaction_id=OLD.transaction_id)
      AND NOT EXISTS(
        SELECT 1 FROM tarot_wager_transfer transfer
        JOIN tarot_wager wager ON wager.wager_id=transfer.wager_id
        JOIN tarot_posting escrow_post ON escrow_post.transaction_id=OLD.transaction_id
        JOIN tarot_account escrow_account ON escrow_account.account_id=escrow_post.account_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND escrow_account.account_kind='ESCROW'
          AND escrow_post.amount=2*wager.stake
          AND ((wager.is_test=0 AND OLD.expected_posting_count=3
            AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                AND a.user_id IN (wager.creator_user_id,wager.target_user_id)
                AND p.amount=-wager.stake)=2)
          OR (wager.is_test=1 AND wager.creator_user_id=wager.target_user_id
            AND OLD.expected_posting_count=2
            AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                AND a.user_id=wager.creator_user_id AND p.amount=-2*wager.stake)=1)))
      THEN RAISE(ABORT,'wager funding shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('WAGER_PAYOUT','WAGER_REFUND')
      AND EXISTS(SELECT 1 FROM tarot_wager_transfer
                 WHERE transaction_id=OLD.transaction_id)
      AND NOT EXISTS(
        SELECT 1 FROM tarot_wager_transfer transfer
        JOIN tarot_wager wager ON wager.wager_id=transfer.wager_id
        JOIN tarot_posting escrow_post ON escrow_post.transaction_id=OLD.transaction_id
        JOIN tarot_account escrow_account ON escrow_account.account_id=escrow_post.account_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND escrow_account.account_kind='ESCROW' AND escrow_post.amount=-2*wager.stake
          AND ((OLD.transaction_type='WAGER_PAYOUT' AND OLD.expected_posting_count=2
            AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                AND a.user_id IN (wager.creator_user_id,wager.target_user_id)
                AND p.amount=2*wager.stake)=1)
          OR (OLD.transaction_type='WAGER_REFUND'
            AND ((wager.is_test=0 AND OLD.expected_posting_count=3
              AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
                WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                  AND a.user_id IN (wager.creator_user_id,wager.target_user_id)
                  AND p.amount=wager.stake)=2)
            OR (wager.is_test=1 AND wager.creator_user_id=wager.target_user_id
              AND OLD.expected_posting_count=2
              AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
                WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                  AND a.user_id=wager.creator_user_id AND p.amount=2*wager.stake)=1)))))
      THEN RAISE(ABORT,'wager settlement shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type='WAGER_ESCROW_FUND'
      AND EXISTS(SELECT 1 FROM tarot_house_transfer
                 WHERE transaction_id=OLD.transaction_id)
      AND NOT EXISTS(
        SELECT 1 FROM tarot_house_transfer transfer
        JOIN tarot_house_wager wager ON wager.wager_id=transfer.wager_id
        JOIN tarot_posting escrow_post ON escrow_post.transaction_id=OLD.transaction_id
        JOIN tarot_account escrow_account ON escrow_account.account_id=escrow_post.account_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND escrow_account.account_kind='ESCROW'
          AND escrow_post.amount=wager.stake+wager.profit
          AND ((wager.recovery=1 AND OLD.expected_posting_count=2
            AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='MINT'
                AND p.amount=-wager.profit))
          OR (wager.recovery=0 AND OLD.expected_posting_count=3
            AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                AND a.user_id=wager.user_id AND p.amount=-wager.stake)
            AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HOUSE'
                AND p.amount=-wager.profit))))
      THEN RAISE(ABORT,'House wager funding shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('WAGER_PAYOUT','WAGER_REFUND')
      AND EXISTS(SELECT 1 FROM tarot_house_transfer
                 WHERE transaction_id=OLD.transaction_id)
      AND NOT EXISTS(
        SELECT 1 FROM tarot_house_transfer transfer
        JOIN tarot_house_wager wager ON wager.wager_id=transfer.wager_id
        JOIN tarot_house_resolution resolution ON resolution.wager_id=wager.wager_id
          AND resolution.transaction_id=OLD.transaction_id
        JOIN tarot_posting escrow_post ON escrow_post.transaction_id=OLD.transaction_id
        JOIN tarot_account escrow_account ON escrow_account.account_id=escrow_post.account_id
        WHERE transfer.transaction_id=OLD.transaction_id
          AND escrow_account.account_kind='ESCROW'
          AND escrow_post.amount=-(wager.stake+wager.profit)
          AND ((OLD.transaction_type='WAGER_PAYOUT' AND OLD.expected_posting_count=2
            AND ((resolution.result='win' AND EXISTS(
              SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                AND a.user_id=wager.user_id AND p.amount=wager.stake+wager.profit))
            OR (resolution.result='loss' AND EXISTS(
              SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
              WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HOUSE'
                AND p.amount=wager.stake+wager.profit))))
          OR (OLD.transaction_type='WAGER_REFUND' AND resolution.result='void'
            AND ((wager.recovery=1 AND OLD.expected_posting_count=2
              AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
                WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='MINT'
                  AND p.amount=wager.profit))
            OR (wager.recovery=0 AND OLD.expected_posting_count=3
              AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
                WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HUMAN'
                  AND a.user_id=wager.user_id AND p.amount=wager.stake)
              AND EXISTS(SELECT 1 FROM tarot_posting p JOIN tarot_account a ON a.account_id=p.account_id
                WHERE p.transaction_id=OLD.transaction_id AND a.account_kind='HOUSE'
                  AND p.amount=wager.profit))))))
      THEN RAISE(ABORT,'House wager settlement shape is invalid') END;

    SELECT CASE WHEN EXISTS(
      SELECT 1 FROM tarot_posting pending
      WHERE pending.transaction_id=OLD.transaction_id
        AND ((pending.amount>0 AND COALESCE((SELECT sum(prior.amount)
          FROM tarot_posting prior JOIN tarot_transaction prior_tx
            ON prior_tx.transaction_id=prior.transaction_id
          WHERE prior.account_id=pending.account_id AND prior_tx.state='committed'),0)
            >9223372036854775807-pending.amount)
        OR (pending.amount<0 AND COALESCE((SELECT sum(prior.amount)
          FROM tarot_posting prior JOIN tarot_transaction prior_tx
            ON prior_tx.transaction_id=prior.transaction_id
          WHERE prior.account_id=pending.account_id AND prior_tx.state='committed'),0)
            <-9223372036854775808-pending.amount)))
      THEN RAISE(ABORT,'tarot account balance overflow') END;
    SELECT CASE WHEN EXISTS(
      SELECT 1 FROM tarot_posting pending
      JOIN tarot_account account ON account.account_id=pending.account_id
      WHERE pending.transaction_id=OLD.transaction_id
        AND account.account_kind='HUMAN'
        AND COALESCE((SELECT sum(prior.amount) FROM tarot_posting prior
          JOIN tarot_transaction tx ON tx.transaction_id=prior.transaction_id
          WHERE prior.account_id=pending.account_id AND tx.state='committed'),0)
          +pending.amount<0)
      THEN RAISE(ABORT,'tarot human balance cannot be negative') END;
END;

CREATE TRIGGER tarot_house_transfer_guard
BEFORE INSERT ON tarot_house_transfer
BEGIN
  SELECT CASE WHEN NOT EXISTS(
    SELECT 1 FROM tarot_house_wager wager
    JOIN tarot_transaction tx ON tx.transaction_id=NEW.transaction_id
    JOIN event_journal event ON event.event_id=tx.event_id
    WHERE wager.wager_id=NEW.wager_id AND tx.state='prepared'
      AND tx.is_test=wager.is_test AND event.aggregate_type='tarot_house_wager'
      AND event.aggregate_id=wager.wager_id
      AND ((NEW.transfer_kind='fund' AND wager.state='accepted_funded'
        AND tx.transaction_type='WAGER_ESCROW_FUND'
        AND event.event_type='tarot.house_funded.v1')
      OR (NEW.transfer_kind='payout' AND wager.state='accepted_funded'
        AND tx.transaction_type='WAGER_PAYOUT'
        AND event.event_type='tarot.house_resolved.v1')
      OR (NEW.transfer_kind='refund' AND wager.state='accepted_funded'
        AND tx.transaction_type='WAGER_REFUND'
        AND event.event_type='tarot.house_voided.v1')))
    THEN RAISE(ABORT,'House transfer linkage is invalid') END;
END;
