-- Milestone 16: durable, post-commit Vox narration.  The cursor begins at the
-- migration-time journal head so upgrading can never narrate historical work.

CREATE TABLE voice_narration_cursor (
    singleton INTEGER PRIMARY KEY CHECK (singleton=1),
    last_event_rowid INTEGER NOT NULL CHECK (last_event_rowid>=0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms>=0)
) STRICT;
INSERT INTO voice_narration_cursor(singleton,last_event_rowid,updated_at_ms)
VALUES(1,COALESCE((SELECT max(rowid) FROM event_journal),0),0);

-- A journal-row watermark is immune to wall-clock rollback.  New feature
-- events must be committed after the Vox session was created.
ALTER TABLE voice_session ADD COLUMN narration_event_rowid_floor INTEGER
    NOT NULL DEFAULT 0 CHECK (narration_event_rowid_floor>=0);
CREATE TRIGGER voice_session_narration_floor_immutable
BEFORE UPDATE OF narration_event_rowid_floor ON voice_session
WHEN NEW.narration_event_rowid_floor<>OLD.narration_event_rowid_floor
BEGIN SELECT RAISE(ABORT,'voice session narration floor is immutable'); END;

DROP TRIGGER speech_item_transition_no_update;
DROP INDEX speech_item_transition_history;
ALTER TABLE speech_item_transition RENAME TO speech_item_transition_v13;
DROP TRIGGER speech_item_guard_update;
DROP INDEX speech_item_queue;
DROP INDEX speech_item_terminal;
ALTER TABLE speech_item RENAME TO speech_item_v13;

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
    narration_rank INTEGER NOT NULL DEFAULT 0 CHECK (narration_rank BETWEEN 0 AND 100),
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
    CHECK ((priority=200)=(narration_rank>0)),
    CHECK (state NOT IN ('pending','synthesizing') OR text IS NOT NULL),
    CHECK (state NOT IN ('played','failed','expired','cancelled') OR text IS NULL),
    CHECK ((state IN ('played','failed','expired','cancelled')) = (terminal_at_ms IS NOT NULL)),
    CHECK ((cache_key IS NULL) = (cache_checksum IS NULL)),
    CHECK (state NOT IN ('ready','playing','played') OR
        (cache_key IS NOT NULL AND duration_ms IS NOT NULL)),
    CHECK (state <> 'playing' OR marker IS NOT NULL),
    CHECK (state <> 'played' OR marker IS NOT NULL)
) STRICT;
INSERT INTO speech_item(
    speech_id,voice_session_id,source_event_id,source_kind,text,text_hash,
    scalar_count,provider,model,voice_id,priority,narration_rank,state,
    state_version,earliest_at_ms,expires_at_ms,interruptible,deduplication_key,
    provider_request_id,cache_key,cache_checksum,marker,duration_ms,
    attempt_count,created_at_ms,terminal_at_ms,last_error_code)
SELECT speech_id,voice_session_id,source_event_id,source_kind,text,text_hash,
       scalar_count,provider,model,voice_id,priority,
       CASE WHEN priority=200 THEN 1 ELSE 0 END,state,state_version,
       earliest_at_ms,expires_at_ms,interruptible,deduplication_key,
       provider_request_id,cache_key,cache_checksum,marker,duration_ms,
       attempt_count,created_at_ms,terminal_at_ms,last_error_code
FROM speech_item_v13;
CREATE INDEX speech_item_queue
    ON speech_item(voice_session_id,state,priority DESC,narration_rank DESC,
                   earliest_at_ms,created_at_ms,speech_id);
CREATE INDEX speech_item_terminal ON speech_item(terminal_at_ms)
    WHERE terminal_at_ms IS NOT NULL;
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
                     OR NEW.narration_rank <> OLD.narration_rank
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
        (OLD.state='synthesizing' AND NEW.state IN ('pending','ready','failed','cancelled','expired')) OR
        (OLD.state='ready' AND NEW.state IN ('pending','playing','failed','cancelled','expired')) OR
        (OLD.state='playing' AND NEW.state IN ('played','failed','cancelled')))
        THEN RAISE(ABORT, 'invalid speech transition') END;
END;
CREATE TRIGGER speech_item_narration_rank_insert
BEFORE INSERT ON speech_item
BEGIN
    SELECT CASE WHEN (NEW.priority=200) <> (NEW.narration_rank>0)
      THEN RAISE(ABORT,'narration speech requires an immutable rank') END;
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
INSERT INTO speech_item_transition(
    transition_id,speech_id,from_state,to_state,from_version,to_version,reason,
    idempotency_key,occurred_at_ms)
SELECT transition_id,speech_id,from_state,to_state,from_version,to_version,
       reason,idempotency_key,occurred_at_ms
FROM speech_item_transition_v13;
CREATE INDEX speech_item_transition_history
    ON speech_item_transition(speech_id,to_version);
CREATE TRIGGER speech_item_transition_no_update
BEFORE UPDATE ON speech_item_transition
BEGIN SELECT RAISE(ABORT, 'speech transitions are append-only'); END;

DROP TABLE speech_item_transition_v13;
DROP TABLE speech_item_v13;

CREATE TABLE voice_narration_intent (
    intent_id TEXT PRIMARY KEY CHECK (length(intent_id)=36),
    source_event_id TEXT NOT NULL REFERENCES event_journal(event_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    slot TEXT NOT NULL CHECK (slot IN ('feature','entrance','farewell','legacy')),
    feature TEXT NOT NULL CHECK (feature IN ('chronicle','tarot','appearance','session','legacy')),
    event_type TEXT NOT NULL CHECK (length(event_type) BETWEEN 1 AND 96),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    safe_input TEXT CHECK (safe_input IS NULL OR length(CAST(safe_input AS BLOB)) BETWEEN 1 AND 2000),
    fallback_line TEXT CHECK (fallback_line IS NULL OR length(CAST(fallback_line AS BLOB)) BETWEEN 1 AND 800),
    narration_rank INTEGER NOT NULL CHECK (narration_rank BETWEEN 0 AND 100),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms>=0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms>created_at_ms),
    session_id TEXT REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    counterpart_outbox_id TEXT REFERENCES outbox_message(outbox_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    counterpart_required INTEGER NOT NULL CHECK (counterpart_required IN (0,1)),
    model_status TEXT NOT NULL CHECK (model_status IN
        ('not_requested','generating','generated','fallback','refused','failed','saturated','duplicate')),
    -- Retain the immutable audit identifier after the 30-day speech row purge.
    -- The admission triggers below validate the live link when it is created.
    speech_id TEXT UNIQUE CHECK (speech_id IS NULL OR length(speech_id)=36),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1)),
    state TEXT NOT NULL CHECK (state IN
        ('pending','generating','prepared','queued','played','suppressed','failed','expired','cancelled')),
    state_version INTEGER NOT NULL CHECK (state_version>0),
    lease_owner TEXT CHECK (lease_owner IS NULL OR length(lease_owner) BETWEEN 1 AND 128),
    lease_token TEXT CHECK (lease_token IS NULL OR length(lease_token)=36),
    lease_until_ms INTEGER CHECK (lease_until_ms IS NULL OR lease_until_ms>=0),
    terminal_reason TEXT CHECK (terminal_reason IS NULL OR
        (length(terminal_reason) BETWEEN 1 AND 64 AND terminal_reason NOT GLOB '*[^a-z0-9_.-]*')),
    content_hash TEXT CHECK (content_hash IS NULL OR
        (length(content_hash)=64 AND content_hash NOT GLOB '*[^0-9a-f]*')),
    UNIQUE(source_event_id,slot),
    CHECK ((state='generating')=(lease_owner IS NOT NULL AND lease_token IS NOT NULL AND lease_until_ms IS NOT NULL)),
    CHECK ((state IN ('played','suppressed','failed','expired','cancelled'))=(terminal_reason IS NOT NULL)),
    CHECK (speech_id IS NULL OR (session_id IS NOT NULL AND state IN ('queued','played','failed','expired','cancelled'))),
    CHECK (counterpart_required=0 OR counterpart_outbox_id IS NOT NULL OR state IN ('pending','suppressed','failed','expired','cancelled'))
) STRICT;
CREATE INDEX voice_narration_pending
    ON voice_narration_intent(state,narration_rank DESC,created_at_ms,intent_id);
CREATE INDEX voice_narration_session
    ON voice_narration_intent(session_id,state,narration_rank DESC);
CREATE UNIQUE INDEX voice_narration_feature_slot
    ON voice_narration_intent(session_id,feature)
    WHERE session_id IS NOT NULL AND slot='feature'
      AND state IN ('generating','prepared','queued','played');

CREATE TABLE voice_narration_transition (
    transition_id TEXT PRIMARY KEY CHECK (length(transition_id)=36),
    intent_id TEXT NOT NULL REFERENCES voice_narration_intent(intent_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    from_state TEXT NOT NULL CHECK (from_state IN
        ('pending','generating','prepared','queued','played','suppressed','failed','expired','cancelled')),
    to_state TEXT NOT NULL CHECK (to_state IN
        ('pending','generating','prepared','queued','played','suppressed','failed','expired','cancelled')),
    from_version INTEGER NOT NULL CHECK (from_version>=0),
    to_version INTEGER NOT NULL CHECK (to_version=from_version+1),
    reason TEXT NOT NULL CHECK (length(reason) BETWEEN 1 AND 64
        AND reason NOT GLOB '*[^a-z0-9_.-]*'),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms>=0)
) STRICT;
CREATE INDEX voice_narration_transition_history
    ON voice_narration_transition(intent_id,to_version);
CREATE TRIGGER voice_narration_transition_no_update
BEFORE UPDATE ON voice_narration_transition
BEGIN SELECT RAISE(ABORT,'voice narration transitions are append-only'); END;
CREATE TRIGGER voice_narration_transition_no_delete
BEFORE DELETE ON voice_narration_transition
BEGIN SELECT RAISE(ABORT,'voice narration transitions are retained'); END;

CREATE TRIGGER voice_narration_intent_guard_update
BEFORE UPDATE ON voice_narration_intent
BEGIN
  SELECT CASE WHEN NEW.intent_id<>OLD.intent_id
    OR NEW.source_event_id<>OLD.source_event_id OR NEW.slot<>OLD.slot
    OR NEW.feature<>OLD.feature OR NEW.event_type<>OLD.event_type
    OR NEW.guild_id<>OLD.guild_id OR NEW.channel_id<>OLD.channel_id
    OR NEW.safe_input IS NOT OLD.safe_input OR NEW.fallback_line IS NOT OLD.fallback_line
    OR NEW.narration_rank<>OLD.narration_rank OR NEW.created_at_ms<>OLD.created_at_ms
    OR NEW.expires_at_ms<>OLD.expires_at_ms OR (OLD.session_id IS NOT NULL AND NEW.session_id IS NOT OLD.session_id)
    OR (OLD.counterpart_outbox_id IS NOT NULL AND NEW.counterpart_outbox_id IS NOT OLD.counterpart_outbox_id)
    OR NEW.counterpart_required<>OLD.counterpart_required OR NEW.is_test<>OLD.is_test
    OR (OLD.speech_id IS NOT NULL AND NEW.speech_id IS NOT OLD.speech_id)
    THEN RAISE(ABORT,'voice narration identity and policy are immutable') END;
  SELECT CASE WHEN NEW.state_version<>OLD.state_version+1
    THEN RAISE(ABORT,'voice narration revision must advance once') END;
  SELECT CASE WHEN OLD.state IN ('played','suppressed','failed','expired','cancelled')
    THEN RAISE(ABORT,'terminal voice narration is immutable') END;
  SELECT CASE WHEN OLD.state='generating' AND NEW.state='generating' AND (
    NEW.model_status<>OLD.model_status OR NEW.speech_id IS NOT OLD.speech_id
    OR NEW.terminal_reason IS NOT OLD.terminal_reason
    OR NEW.content_hash IS NOT OLD.content_hash
    OR NEW.lease_owner IS NOT OLD.lease_owner
    OR NEW.lease_token IS OLD.lease_token
    OR NEW.lease_until_ms<OLD.lease_until_ms)
    THEN RAISE(ABORT,'generation start may only renew its lease') END;
  SELECT CASE WHEN NOT (
    (OLD.state='pending' AND NEW.state IN
      ('generating','suppressed','failed','expired','cancelled')) OR
    (OLD.state='generating' AND NEW.state IN
      ('generating','pending','prepared','queued','suppressed','failed','expired','cancelled')) OR
    (OLD.state='prepared' AND NEW.state IN
      ('queued','suppressed','failed','expired','cancelled')) OR
    (OLD.state='queued' AND NEW.state IN
      ('played','failed','expired','cancelled')))
    THEN RAISE(ABORT,'invalid voice narration transition') END;
  SELECT CASE WHEN NOT EXISTS(
    SELECT 1 FROM event_journal event WHERE event.event_id=NEW.source_event_id
      AND event.event_type=NEW.event_type AND event.guild_id=NEW.guild_id
      AND event.channel_id=NEW.channel_id)
    THEN RAISE(ABORT,'narration source event linkage is invalid') END;
  SELECT CASE WHEN NEW.session_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM voice_session s JOIN event_journal event
      ON event.event_id=NEW.source_event_id WHERE s.session_id=NEW.session_id
      AND s.guild_id=NEW.guild_id AND s.text_channel_id=NEW.channel_id
      AND event.rowid>s.narration_event_rowid_floor)
    THEN RAISE(ABORT,'narration session binding is invalid') END;
  SELECT CASE WHEN NEW.counterpart_outbox_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM outbox_message outbox
      WHERE outbox.outbox_id=NEW.counterpart_outbox_id
      AND outbox.target_guild_id=NEW.guild_id
      AND outbox.target_channel_id=NEW.channel_id
      AND outbox.target_user_id IS NULL)
    THEN RAISE(ABORT,'narration counterpart linkage is invalid') END;
  SELECT CASE WHEN NEW.speech_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM speech_item speech WHERE speech.speech_id=NEW.speech_id
      AND speech.source_event_id=NEW.source_event_id
      AND speech.voice_session_id=NEW.session_id
      AND speech.source_kind='vox_feature_narration'
      AND speech.priority=200 AND speech.narration_rank=NEW.narration_rank
      AND speech.expires_at_ms IS NOT NULL
      AND speech.expires_at_ms<=NEW.expires_at_ms)
    THEN RAISE(ABORT,'narration speech linkage is invalid') END;
  SELECT CASE WHEN NEW.state='queued' AND NEW.counterpart_required=1 AND NOT EXISTS(
    SELECT 1 FROM outbox_message outbox WHERE outbox.outbox_id=NEW.counterpart_outbox_id
      AND outbox.state='delivered' AND outbox.delivered_at_ms<=NEW.expires_at_ms)
    THEN RAISE(ABORT,'narration requires confirmed text counterpart') END;
END;
CREATE TRIGGER voice_narration_intent_guard_insert
BEFORE INSERT ON voice_narration_intent
BEGIN
  SELECT CASE WHEN NOT EXISTS(
    SELECT 1 FROM event_journal event WHERE event.event_id=NEW.source_event_id
      AND event.event_type=NEW.event_type AND event.guild_id=NEW.guild_id
      AND event.channel_id=NEW.channel_id)
    THEN RAISE(ABORT,'narration source event linkage is invalid') END;
  SELECT CASE WHEN NEW.session_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM voice_session s JOIN event_journal event
      ON event.event_id=NEW.source_event_id WHERE s.session_id=NEW.session_id
      AND s.guild_id=NEW.guild_id AND s.text_channel_id=NEW.channel_id
      AND event.rowid>s.narration_event_rowid_floor)
    THEN RAISE(ABORT,'narration session binding is invalid') END;
  SELECT CASE WHEN NEW.counterpart_outbox_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM outbox_message outbox
      WHERE outbox.outbox_id=NEW.counterpart_outbox_id
      AND outbox.target_guild_id=NEW.guild_id
      AND outbox.target_channel_id=NEW.channel_id
      AND outbox.target_user_id IS NULL)
    THEN RAISE(ABORT,'narration counterpart linkage is invalid') END;
  SELECT CASE WHEN NEW.speech_id IS NOT NULL AND NOT EXISTS(
    SELECT 1 FROM speech_item speech WHERE speech.speech_id=NEW.speech_id
      AND speech.source_event_id=NEW.source_event_id
      AND speech.voice_session_id=NEW.session_id
      AND speech.source_kind='vox_feature_narration'
      AND speech.priority=200 AND speech.narration_rank=NEW.narration_rank
      AND speech.expires_at_ms IS NOT NULL
      AND speech.expires_at_ms<=NEW.expires_at_ms)
    THEN RAISE(ABORT,'narration speech linkage is invalid') END;
  SELECT CASE WHEN NEW.state='queued' AND NEW.counterpart_required=1 AND NOT EXISTS(
    SELECT 1 FROM outbox_message outbox WHERE outbox.outbox_id=NEW.counterpart_outbox_id
      AND outbox.state='delivered' AND outbox.delivered_at_ms<=NEW.expires_at_ms)
    THEN RAISE(ABORT,'narration requires confirmed text counterpart') END;
END;
CREATE TRIGGER voice_narration_intent_no_delete
BEFORE DELETE ON voice_narration_intent
BEGIN SELECT RAISE(ABORT,'voice narration intents are retained'); END;

-- Preserve dormant Milestone 13 rows as terminal audit only.  Their original
-- 24-hour expiry deliberately remains; they can never become speech.
INSERT INTO voice_narration_intent(
  intent_id,source_event_id,slot,feature,event_type,guild_id,channel_id,
  safe_input,fallback_line,narration_rank,created_at_ms,expires_at_ms,
  counterpart_required,model_status,is_test,state,state_version,terminal_reason)
SELECT old.intent_id,old.source_event_id,'legacy','legacy',event.event_type,
  old.guild_id,old.channel_id,old.public_safe_text,NULL,0,old.created_at_ms,
  old.expires_at_ms,0,'not_requested',old.is_test,'suppressed',1,
  'pre_m16_not_replayed'
FROM tarot_vox_narration_intent old
JOIN event_journal event ON event.event_id=old.source_event_id;
INSERT INTO voice_narration_transition(
  transition_id,intent_id,from_state,to_state,from_version,to_version,reason,
  idempotency_key,occurred_at_ms)
SELECT intent_id,intent_id,'pending','suppressed',0,1,'pre_m16_not_replayed',
  'narration:legacy:'||intent_id,created_at_ms
FROM voice_narration_intent WHERE slot='legacy';
DROP TABLE tarot_vox_narration_intent;

DROP TRIGGER voice_interaction_receipt_no_update;
DROP TRIGGER voice_interaction_receipt_no_delete;
DROP INDEX voice_interaction_receipt_actor;
ALTER TABLE voice_interaction_receipt RENAME TO voice_interaction_receipt_v13;
CREATE TABLE voice_interaction_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    operation TEXT NOT NULL CHECK (operation IN
        ('summon','status','leave','test_disconnect','say','mute','voice','speech_test',
         'narration_preview','narration_enqueue')),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL CHECK (length(channel_id) BETWEEN 1 AND 20),
    request_json TEXT NOT NULL CHECK (json_valid(request_json)
        AND json_type(request_json)='object' AND length(CAST(request_json AS BLOB)) BETWEEN 2 AND 2048),
    result_json TEXT NOT NULL CHECK (json_valid(result_json)
        AND json_type(result_json)='object' AND length(CAST(result_json AS BLOB)) BETWEEN 2 AND 4096),
    session_id TEXT REFERENCES voice_session(session_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms>=0)
) STRICT;
INSERT INTO voice_interaction_receipt SELECT * FROM voice_interaction_receipt_v13;
DROP TABLE voice_interaction_receipt_v13;
CREATE INDEX voice_interaction_receipt_actor
    ON voice_interaction_receipt(actor_user_id,created_at_ms DESC);
CREATE TRIGGER voice_interaction_receipt_no_update BEFORE UPDATE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT,'voice interaction receipts are immutable'); END;
CREATE TRIGGER voice_interaction_receipt_no_delete BEFORE DELETE ON voice_interaction_receipt
BEGIN SELECT RAISE(ABORT,'voice interaction receipts are retained'); END;
