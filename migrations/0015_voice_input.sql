-- Milestone 17: disabled-by-default, short voice-listening windows.
-- This schema stores operational audit metadata only. Audio and transcripts
-- are deliberately absent.

CREATE TABLE voice_input_consent_attestation (
    attestation_id TEXT PRIMARY KEY
        CHECK (length(attestation_id)=36 AND substr(attestation_id,9,1)='-'
          AND substr(attestation_id,14,1)='-' AND substr(attestation_id,19,1)='-'
          AND substr(attestation_id,24,1)='-' AND length(replace(attestation_id,'-',''))=32
          AND attestation_id NOT GLOB '*[^0-9a-f-]*' AND substr(attestation_id,15,1)='4'
          AND substr(attestation_id,20,1) IN ('8','9','a','b')),
    attested INTEGER NOT NULL CHECK(attested IN (0,1)),
    owner_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    recorded_at_ms INTEGER NOT NULL CHECK(recorded_at_ms>=0)
) STRICT;
CREATE INDEX voice_input_consent_history
    ON voice_input_consent_attestation(recorded_at_ms DESC,attestation_id DESC);
CREATE TRIGGER voice_input_consent_no_update BEFORE UPDATE ON voice_input_consent_attestation
BEGIN SELECT RAISE(ABORT,'voice consent attestations are immutable'); END;
CREATE TRIGGER voice_input_consent_no_delete BEFORE DELETE ON voice_input_consent_attestation
BEGIN SELECT RAISE(ABORT,'voice consent attestations are retained'); END;

CREATE TABLE voice_input_control (
    singleton INTEGER PRIMARY KEY CHECK(singleton=1),
    disabled INTEGER NOT NULL CHECK(disabled IN (0,1)),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms>=0)
) STRICT;
INSERT INTO voice_input_control(singleton,disabled,updated_at_ms) VALUES(1,0,0);

CREATE TABLE voice_input_kill_change (
    change_id TEXT PRIMARY KEY
        CHECK (length(change_id)=36 AND substr(change_id,9,1)='-'
          AND substr(change_id,14,1)='-' AND substr(change_id,19,1)='-'
          AND substr(change_id,24,1)='-' AND length(replace(change_id,'-',''))=32
          AND change_id NOT GLOB '*[^0-9a-f-]*' AND substr(change_id,15,1)='4'
          AND substr(change_id,20,1) IN ('8','9','a','b')),
    disabled INTEGER NOT NULL CHECK(disabled IN (0,1)),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    occurred_at_ms INTEGER NOT NULL CHECK(occurred_at_ms>=0)
) STRICT;
CREATE TRIGGER voice_input_kill_change_no_update BEFORE UPDATE ON voice_input_kill_change
BEGIN SELECT RAISE(ABORT,'voice input kill changes are immutable'); END;
CREATE TRIGGER voice_input_kill_change_no_delete BEFORE DELETE ON voice_input_kill_change
BEGIN SELECT RAISE(ABORT,'voice input kill changes are retained'); END;

CREATE TABLE voice_listening_window (
    window_id TEXT PRIMARY KEY
        CHECK (length(window_id)=36 AND substr(window_id,9,1)='-'
          AND substr(window_id,14,1)='-' AND substr(window_id,19,1)='-'
          AND substr(window_id,24,1)='-' AND length(replace(window_id,'-',''))=32
          AND window_id NOT GLOB '*[^0-9a-f-]*' AND substr(window_id,15,1)='4'
          AND substr(window_id,20,1) IN ('8','9','a','b')),
    vox_session_id TEXT NOT NULL REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    text_channel_id TEXT NOT NULL CHECK(length(text_channel_id) BETWEEN 1 AND 20
        AND text_channel_id NOT GLOB '*[^0-9]*' AND text_channel_id<>'0'),
    voice_channel_id TEXT NOT NULL CHECK(length(voice_channel_id) BETWEEN 1 AND 20
        AND voice_channel_id NOT GLOB '*[^0-9]*' AND voice_channel_id<>'0'),
    requester_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    state TEXT NOT NULL CHECK(state IN ('proposed','arming_transport',
        'arming_indicator','active','transcribing','completed','stopped',
        'failed','abandoned')),
    state_version INTEGER NOT NULL CHECK(state_version>0),
    connection_generation INTEGER NOT NULL CHECK(connection_generation>0),
    requested_seconds INTEGER NOT NULL CHECK(requested_seconds IN (5,10,15)),
    initial_human_count INTEGER NOT NULL CHECK(initial_human_count BETWEEN 1 AND 100),
    reserved_micro_usd INTEGER NOT NULL CHECK(reserved_micro_usd=requested_seconds*75),
    provider_attempt_started INTEGER NOT NULL DEFAULT 0
        CHECK(provider_attempt_started IN (0,1)),
    provider_attempt_started_at_ms INTEGER CHECK(
        provider_attempt_started_at_ms IS NULL OR
        provider_attempt_started_at_ms>=created_at_ms),
    reservation_released INTEGER NOT NULL DEFAULT 0 CHECK(reservation_released IN (0,1)),
    reservation_released_at_ms INTEGER CHECK(reservation_released_at_ms IS NULL OR reservation_released_at_ms>=created_at_ms),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms>=0),
    active_at_ms INTEGER CHECK(active_at_ms IS NULL OR active_at_ms>=created_at_ms),
    ended_at_ms INTEGER CHECK(ended_at_ms IS NULL OR ended_at_ms>=created_at_ms),
    public_message_id TEXT CHECK(public_message_id IS NULL OR
        (length(public_message_id) BETWEEN 1 AND 20 AND public_message_id NOT GLOB '*[^0-9]*'
         AND public_message_id<>'0')),
    terminal_reason TEXT CHECK(terminal_reason IS NULL OR
        (length(terminal_reason) BETWEEN 1 AND 64 AND terminal_reason NOT GLOB '*[^a-z0-9_.-]*')),
    interaction_idempotency_key TEXT NOT NULL UNIQUE
        CHECK(length(interaction_idempotency_key) BETWEEN 1 AND 160),
    request_fingerprint TEXT NOT NULL CHECK(length(request_fingerprint) BETWEEN 1 AND 64),
    CHECK((reservation_released=0)=(reservation_released_at_ms IS NULL)),
    CHECK((provider_attempt_started=0)=
          (provider_attempt_started_at_ms IS NULL)),
    CHECK((state IN ('completed','stopped','failed','abandoned'))=(ended_at_ms IS NOT NULL)),
    CHECK((ended_at_ms IS NULL)=(terminal_reason IS NULL))
) STRICT;
CREATE UNIQUE INDEX voice_listening_one_nonterminal
    ON voice_listening_window((1)) WHERE state IN
      ('proposed','arming_transport','arming_indicator','active','transcribing');
CREATE INDEX voice_listening_usage_window
    ON voice_listening_window(created_at_ms DESC,window_id DESC);
CREATE TRIGGER voice_listening_window_no_delete BEFORE DELETE ON voice_listening_window
BEGIN SELECT RAISE(ABORT,'voice listening windows are retained'); END;
CREATE TRIGGER voice_listening_window_guard_update
BEFORE UPDATE ON voice_listening_window
WHEN NEW.window_id<>OLD.window_id
  OR NEW.vox_session_id<>OLD.vox_session_id
  OR NEW.guild_id<>OLD.guild_id
  OR NEW.text_channel_id<>OLD.text_channel_id
  OR NEW.voice_channel_id<>OLD.voice_channel_id
  OR NEW.requester_user_id<>OLD.requester_user_id
  OR NEW.connection_generation<>OLD.connection_generation
  OR NEW.requested_seconds<>OLD.requested_seconds
  OR NEW.initial_human_count<>OLD.initial_human_count
  OR NEW.reserved_micro_usd<>OLD.reserved_micro_usd
  OR NEW.created_at_ms<>OLD.created_at_ms
  OR NEW.interaction_idempotency_key<>OLD.interaction_idempotency_key
  OR NEW.request_fingerprint<>OLD.request_fingerprint
  OR NOT ((NEW.state=OLD.state AND NEW.state_version=OLD.state_version)
      OR (NEW.state<>OLD.state AND NEW.state_version=OLD.state_version+1
          AND ((OLD.state IN ('proposed','arming_transport','arming_indicator',
                              'active','transcribing')
                AND NEW.state IN ('failed','stopped','abandoned'))
            OR (OLD.state='proposed' AND NEW.state='arming_transport')
            OR (OLD.state='arming_transport' AND NEW.state='arming_indicator')
            OR (OLD.state='arming_indicator' AND NEW.state='active')
            OR (OLD.state='active' AND NEW.state='transcribing')
            OR (OLD.state='transcribing' AND NEW.state='completed'))))
  OR NOT ((NEW.active_at_ms IS OLD.active_at_ms)
      OR (OLD.active_at_ms IS NULL AND NEW.active_at_ms IS NOT NULL
          AND OLD.state='arming_indicator' AND NEW.state='active'))
  OR NOT ((NEW.ended_at_ms IS OLD.ended_at_ms
           AND NEW.terminal_reason IS OLD.terminal_reason)
      OR (OLD.ended_at_ms IS NULL AND NEW.ended_at_ms IS NOT NULL
          AND OLD.terminal_reason IS NULL AND NEW.terminal_reason IS NOT NULL
          AND NEW.state IN ('completed','stopped','failed','abandoned')))
  OR NOT ((NEW.public_message_id IS OLD.public_message_id)
      OR (OLD.public_message_id IS NULL AND NEW.public_message_id IS NOT NULL
          AND OLD.state IN ('proposed','arming_transport','arming_indicator')))
  OR NOT ((NEW.reservation_released=OLD.reservation_released
           AND NEW.reservation_released_at_ms IS OLD.reservation_released_at_ms)
      OR (OLD.reservation_released=0 AND NEW.reservation_released=1
          AND OLD.reservation_released_at_ms IS NULL
          AND NEW.reservation_released_at_ms IS NOT NULL))
  OR NOT ((NEW.provider_attempt_started=OLD.provider_attempt_started
           AND NEW.provider_attempt_started_at_ms IS
               OLD.provider_attempt_started_at_ms)
      OR (OLD.provider_attempt_started=0 AND NEW.provider_attempt_started=1
          AND OLD.provider_attempt_started_at_ms IS NULL
          AND NEW.provider_attempt_started_at_ms IS NOT NULL
          AND OLD.state='transcribing' AND NEW.state='transcribing'))
BEGIN SELECT RAISE(ABORT,'invalid voice listening window update'); END;

CREATE TABLE voice_listening_transition (
    transition_id TEXT PRIMARY KEY CHECK(length(transition_id) BETWEEN 1 AND 100
        AND transition_id NOT GLOB '*[^a-z0-9:_.-]*'),
    window_id TEXT NOT NULL REFERENCES voice_listening_window(window_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_state TEXT NOT NULL CHECK(from_state IN ('none','proposed','arming_transport',
        'arming_indicator','active','transcribing','completed','stopped','failed','abandoned')),
    to_state TEXT NOT NULL CHECK(to_state IN ('proposed','arming_transport',
        'arming_indicator','active','transcribing','completed','stopped','failed','abandoned')),
    from_version INTEGER NOT NULL CHECK(from_version>=0),
    to_version INTEGER NOT NULL CHECK(to_version=from_version+1),
    reason TEXT NOT NULL CHECK(length(reason) BETWEEN 1 AND 64
        AND reason NOT GLOB '*[^a-z0-9_.-]*'),
    actor_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    idempotency_key TEXT NOT NULL UNIQUE CHECK(length(idempotency_key) BETWEEN 1 AND 160),
    occurred_at_ms INTEGER NOT NULL CHECK(occurred_at_ms>=0)
) STRICT;
CREATE UNIQUE INDEX voice_listening_transition_version
    ON voice_listening_transition(window_id,to_version);
CREATE TRIGGER voice_listening_transition_no_update BEFORE UPDATE ON voice_listening_transition
BEGIN SELECT RAISE(ABORT,'voice listening transitions are immutable'); END;
CREATE TRIGGER voice_listening_transition_no_delete BEFORE DELETE ON voice_listening_transition
BEGIN SELECT RAISE(ABORT,'voice listening transitions are retained'); END;

CREATE TABLE voice_transcription_usage (
    window_id TEXT PRIMARY KEY REFERENCES voice_listening_window(window_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    provider TEXT NOT NULL CHECK(provider='openai'),
    model TEXT NOT NULL CHECK(model='gpt-transcribe'),
    provider_request_id TEXT CHECK(provider_request_id IS NULL OR
        (length(CAST(provider_request_id AS BLOB)) BETWEEN 1 AND 256
         AND provider_request_id NOT GLOB '*[^A-Za-z0-9_.:-]*')),
    captured_bytes INTEGER NOT NULL CHECK(captured_bytes BETWEEN 0 AND 2880000),
    captured_duration_ms INTEGER NOT NULL CHECK(captured_duration_ms BETWEEN 0 AND 15000),
    estimated_micro_usd INTEGER NOT NULL CHECK(estimated_micro_usd BETWEEN 0 AND 1125),
    latency_ms INTEGER NOT NULL CHECK(latency_ms>=0),
    result_code TEXT NOT NULL CHECK(length(result_code) BETWEEN 1 AND 64
        AND result_code NOT GLOB '*[^a-z0-9_.-]*'),
    provider_sent INTEGER NOT NULL CHECK(provider_sent IN (0,1)),
    recorded_at_ms INTEGER NOT NULL CHECK(recorded_at_ms>=0),
    CHECK(provider_sent=1 OR provider_request_id IS NULL),
    CHECK(provider_sent=1 OR estimated_micro_usd=0)
) STRICT;
CREATE INDEX voice_transcription_usage_recent
    ON voice_transcription_usage(recorded_at_ms DESC,window_id DESC);
CREATE TRIGGER voice_transcription_usage_requires_attempt
BEFORE INSERT ON voice_transcription_usage
WHEN NEW.provider_sent=1 AND NOT EXISTS(
    SELECT 1 FROM voice_listening_window w
    WHERE w.window_id=NEW.window_id AND w.provider_attempt_started=1)
BEGIN SELECT RAISE(ABORT,'sent transcription requires provider attempt'); END;
CREATE TRIGGER voice_transcription_usage_no_update BEFORE UPDATE ON voice_transcription_usage
BEGIN SELECT RAISE(ABORT,'voice transcription usage is immutable'); END;
CREATE TRIGGER voice_transcription_usage_no_delete BEFORE DELETE ON voice_transcription_usage
BEGIN SELECT RAISE(ABORT,'voice transcription usage is retained'); END;

-- Chronicle entries gain voice-transcript provenance. Rebuild the
-- parent through a fully populated replacement and preserve every direct child
-- in transaction-local copies before the swap. Any count mismatch aborts the
-- whole migration before an accepted table is dropped.
CREATE TABLE chronicle_entry_m17_new (
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
        ('discord_message','session_summary','title_award','tarot_event','voice_transcript')),
    source_event_id TEXT UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    source_voice_window_id TEXT UNIQUE REFERENCES voice_listening_window(window_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    UNIQUE(source_guild_id,source_channel_id,source_message_id),
    CHECK ((approved_at_ms IS NULL)=(approved_by_user_id IS NULL)),
    CHECK ((retracted_at_ms IS NULL)=(retracted_by_user_id IS NULL)),
    CHECK ((status='proposed' AND approved_at_ms IS NULL AND retracted_at_ms IS NULL)
        OR (status='canon' AND approved_at_ms IS NOT NULL AND retracted_at_ms IS NULL)
        OR (status='retracted' AND retracted_at_ms IS NOT NULL)),
    CHECK ((source_kind='discord_message' AND source_message_id IS NOT NULL
            AND source_event_id IS NULL AND source_voice_window_id IS NULL
            AND entry_type NOT IN ('session_summary','title_award'))
        OR (source_kind='session_summary' AND source_message_id IS NULL
            AND source_event_id IS NULL AND source_voice_window_id IS NULL
            AND entry_type='session_summary')
        OR (source_kind='title_award' AND source_message_id IS NULL
            AND source_event_id IS NULL AND source_voice_window_id IS NULL
            AND entry_type='title_award')
        OR (source_kind='tarot_event' AND source_message_id IS NULL
            AND source_event_id IS NOT NULL AND source_voice_window_id IS NULL
            AND entry_type IN ('deed','prediction','incident','custom'))
        OR (source_kind='voice_transcript' AND source_message_id IS NULL
            AND source_event_id IS NULL AND source_voice_window_id IS NOT NULL
            AND entry_type='custom'))
) STRICT;

INSERT INTO chronicle_entry_m17_new(rowid,entry_id,entry_type,title,body,
  visibility,status,occurred_at_ms,created_at_ms,created_by_user_id,
  submitted_at_ms,approved_at_ms,approved_by_user_id,retracted_at_ms,
  retracted_by_user_id,source_guild_id,source_channel_id,source_message_id,
  source_author_user_id,source_text,source_text_truncated,
  source_attachment_count,revision,source_kind,source_event_id,
  source_voice_window_id)
SELECT rowid,entry_id,entry_type,title,body,visibility,status,occurred_at_ms,
  created_at_ms,created_by_user_id,submitted_at_ms,approved_at_ms,
  approved_by_user_id,retracted_at_ms,retracted_by_user_id,source_guild_id,
  source_channel_id,source_message_id,source_author_user_id,source_text,
  source_text_truncated,source_attachment_count,revision,source_kind,
  source_event_id,NULL
FROM chronicle_entry;

CREATE TEMP TABLE m17_chronicle_participant AS SELECT * FROM chronicle_participant;
CREATE TEMP TABLE m17_chronicle_tag AS SELECT * FROM chronicle_tag;
CREATE TEMP TABLE m17_chronicle_attachment AS SELECT * FROM chronicle_attachment;
CREATE TEMP TABLE m17_chronicle_approval AS SELECT * FROM chronicle_approval;
CREATE TEMP TABLE m17_chronicle_session_entry AS SELECT * FROM chronicle_session_entry;
CREATE TEMP TABLE m17_chronicle_summary_draft AS SELECT * FROM chronicle_summary_draft;
CREATE TEMP TABLE m17_chronicle_summary_highlight AS SELECT * FROM chronicle_summary_highlight;
CREATE TEMP TABLE m17_chronicle_title_definition AS SELECT * FROM chronicle_title_definition;
CREATE TEMP TABLE m17_chronicle_title_grant AS SELECT * FROM chronicle_title_grant;
CREATE TEMP TABLE m17_ai_prompt_attempt_chronicle_context AS SELECT * FROM ai_prompt_attempt_chronicle_context;
CREATE TEMP TABLE m17_chronicle_anniversary_delivery AS SELECT * FROM chronicle_anniversary_delivery;
CREATE TEMP TABLE m17_chronicle_appearance_source AS SELECT * FROM chronicle_appearance_source;
CREATE TEMP TABLE m17_chronicle_verify(delta INTEGER NOT NULL CHECK(delta=0));
INSERT INTO m17_chronicle_verify VALUES
  ((SELECT count(*) FROM chronicle_entry_m17_new)-(SELECT count(*) FROM chronicle_entry)),
  ((SELECT count(*) FROM m17_chronicle_participant)-(SELECT count(*) FROM chronicle_participant)),
  ((SELECT count(*) FROM m17_chronicle_tag)-(SELECT count(*) FROM chronicle_tag)),
  ((SELECT count(*) FROM m17_chronicle_attachment)-(SELECT count(*) FROM chronicle_attachment)),
  ((SELECT count(*) FROM m17_chronicle_approval)-(SELECT count(*) FROM chronicle_approval)),
  ((SELECT count(*) FROM m17_chronicle_session_entry)-(SELECT count(*) FROM chronicle_session_entry)),
  ((SELECT count(*) FROM m17_chronicle_summary_draft)-(SELECT count(*) FROM chronicle_summary_draft)),
  ((SELECT count(*) FROM m17_chronicle_summary_highlight)-(SELECT count(*) FROM chronicle_summary_highlight)),
  ((SELECT count(*) FROM m17_chronicle_title_definition)-(SELECT count(*) FROM chronicle_title_definition)),
  ((SELECT count(*) FROM m17_chronicle_title_grant)-(SELECT count(*) FROM chronicle_title_grant)),
  ((SELECT count(*) FROM m17_ai_prompt_attempt_chronicle_context)-(SELECT count(*) FROM ai_prompt_attempt_chronicle_context)),
  ((SELECT count(*) FROM m17_chronicle_anniversary_delivery)-(SELECT count(*) FROM chronicle_anniversary_delivery)),
  ((SELECT count(*) FROM m17_chronicle_appearance_source)-(SELECT count(*) FROM chronicle_appearance_source));

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
ALTER TABLE chronicle_entry_m17_new RENAME TO chronicle_entry;

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

INSERT INTO chronicle_participant SELECT * FROM m17_chronicle_participant;
INSERT INTO chronicle_tag SELECT * FROM m17_chronicle_tag;
INSERT INTO chronicle_attachment SELECT * FROM m17_chronicle_attachment;
INSERT INTO chronicle_approval SELECT * FROM m17_chronicle_approval;
INSERT INTO chronicle_session_entry SELECT * FROM m17_chronicle_session_entry;
INSERT INTO chronicle_summary_draft SELECT * FROM m17_chronicle_summary_draft;
INSERT INTO chronicle_summary_highlight SELECT * FROM m17_chronicle_summary_highlight;
INSERT INTO chronicle_title_definition SELECT * FROM m17_chronicle_title_definition;
INSERT INTO chronicle_title_grant SELECT * FROM m17_chronicle_title_grant;
INSERT INTO ai_prompt_attempt_chronicle_context SELECT * FROM m17_ai_prompt_attempt_chronicle_context;
INSERT INTO chronicle_anniversary_delivery SELECT * FROM m17_chronicle_anniversary_delivery;
INSERT INTO chronicle_appearance_source SELECT * FROM m17_chronicle_appearance_source;

DELETE FROM m17_chronicle_verify;
INSERT INTO m17_chronicle_verify VALUES
  ((SELECT count(*) FROM chronicle_participant)-(SELECT count(*) FROM m17_chronicle_participant)),
  ((SELECT count(*) FROM chronicle_tag)-(SELECT count(*) FROM m17_chronicle_tag)),
  ((SELECT count(*) FROM chronicle_attachment)-(SELECT count(*) FROM m17_chronicle_attachment)),
  ((SELECT count(*) FROM chronicle_approval)-(SELECT count(*) FROM m17_chronicle_approval)),
  ((SELECT count(*) FROM chronicle_session_entry)-(SELECT count(*) FROM m17_chronicle_session_entry)),
  ((SELECT count(*) FROM chronicle_summary_draft)-(SELECT count(*) FROM m17_chronicle_summary_draft)),
  ((SELECT count(*) FROM chronicle_summary_highlight)-(SELECT count(*) FROM m17_chronicle_summary_highlight)),
  ((SELECT count(*) FROM chronicle_title_definition)-(SELECT count(*) FROM m17_chronicle_title_definition)),
  ((SELECT count(*) FROM chronicle_title_grant)-(SELECT count(*) FROM m17_chronicle_title_grant)),
  ((SELECT count(*) FROM ai_prompt_attempt_chronicle_context)-(SELECT count(*) FROM m17_ai_prompt_attempt_chronicle_context)),
  ((SELECT count(*) FROM chronicle_anniversary_delivery)-(SELECT count(*) FROM m17_chronicle_anniversary_delivery)),
  ((SELECT count(*) FROM chronicle_appearance_source)-(SELECT count(*) FROM m17_chronicle_appearance_source));

DROP TABLE m17_chronicle_appearance_source;
DROP TABLE m17_chronicle_anniversary_delivery;
DROP TABLE m17_ai_prompt_attempt_chronicle_context;
DROP TABLE m17_chronicle_title_grant;
DROP TABLE m17_chronicle_title_definition;
DROP TABLE m17_chronicle_summary_highlight;
DROP TABLE m17_chronicle_summary_draft;
DROP TABLE m17_chronicle_session_entry;
DROP TABLE m17_chronicle_approval;
DROP TABLE m17_chronicle_attachment;
DROP TABLE m17_chronicle_tag;
DROP TABLE m17_chronicle_participant;
DROP TABLE m17_chronicle_verify;

CREATE INDEX chronicle_entry_status_time ON chronicle_entry(status,occurred_at_ms DESC,entry_id DESC);
CREATE INDEX chronicle_participant_user ON chronicle_participant(user_id,entry_id);
CREATE INDEX chronicle_tag_lookup ON chronicle_tag(tag,entry_id);
CREATE INDEX chronicle_approval_pending ON chronicle_approval(reviewer_user_id,requested_at_ms,approval_id) WHERE state='pending';
CREATE UNIQUE INDEX chronicle_title_one_featured ON chronicle_title_grant(recipient_user_id) WHERE featured=1;
CREATE UNIQUE INDEX chronicle_title_one_session_recipient ON chronicle_title_grant(recipient_user_id,definition_id);

-- Recreate the Milestone 11 integration triggers that SQLite removes when
-- their Chronicle parent tables are rebuilt above.
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
