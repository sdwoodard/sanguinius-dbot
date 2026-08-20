-- Milestone 10: live unsolicited appearances, durable reservations, feedback,
-- and server-wide safety controls.

DROP TRIGGER appearance_dry_run_outbox_guard;

ALTER TABLE appearance_mode_state RENAME TO appearance_mode_state_v7;
CREATE TABLE appearance_mode_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    mode TEXT NOT NULL CHECK (mode IN ('off', 'dry_run', 'live')),
    activated_at_ms INTEGER NOT NULL CHECK (activated_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= activated_at_ms)
) STRICT;
INSERT INTO appearance_mode_state(singleton,mode,activated_at_ms,updated_at_ms)
SELECT singleton,mode,updated_at_ms,updated_at_ms FROM appearance_mode_state_v7;
DROP TABLE appearance_mode_state_v7;

ALTER TABLE appearance_candidate
ADD COLUMN mode_activated_at_ms INTEGER NOT NULL DEFAULT 0
    CHECK (mode_activated_at_ms >= 0);
UPDATE appearance_candidate
SET mode_activated_at_ms=(
    SELECT activated_at_ms FROM appearance_mode_state WHERE singleton=1
);

DROP TRIGGER appearance_decision_transition_only;
DROP TRIGGER appearance_decision_no_delete;

CREATE TABLE appearance_decision_v8 (
    decision_id TEXT PRIMARY KEY CHECK (length(decision_id) = 36),
    candidate_id TEXT NOT NULL UNIQUE REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    policy_version TEXT NOT NULL REFERENCES appearance_policy_snapshot(policy_version) ON UPDATE RESTRICT ON DELETE RESTRICT,
    application_instance_id TEXT NOT NULL CHECK (length(application_instance_id) BETWEEN 1 AND 128),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision BETWEEN 1 AND 2),
    state TEXT NOT NULL CHECK (state IN ('model_pending','final')),
    action TEXT CHECK (action IS NULL OR action IN ('reject','hypothetical','live_queued')),
    reason TEXT CHECK (reason IS NULL OR (length(reason) BETWEEN 1 AND 96 AND reason NOT GLOB '*[^a-z0-9_.-]*')),
    gate_json TEXT NOT NULL CHECK (json_valid(gate_json) AND length(gate_json) <= 8192),
    score_json TEXT NOT NULL CHECK (json_valid(score_json) AND length(score_json) <= 8192),
    score INTEGER NOT NULL CHECK (score BETWEEN 0 AND 100),
    human_message_count INTEGER NOT NULL CHECK (human_message_count >= 0),
    model_status TEXT NOT NULL CHECK (length(model_status) BETWEEN 1 AND 96),
    serious_categories_json TEXT NOT NULL DEFAULT '[]'
      CHECK (json_valid(serious_categories_json)
             AND json_type(serious_categories_json) = 'array'
             AND json_array_length(serious_categories_json) <= 7
             AND length(serious_categories_json) <= 512),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    finalized_at_ms INTEGER,
    CHECK ((state = 'final') = (action IS NOT NULL AND reason IS NOT NULL AND finalized_at_ms IS NOT NULL))
) STRICT;

CREATE TABLE appearance_decision_memory_v8 (
    decision_id TEXT NOT NULL REFERENCES appearance_decision_v8(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    memory_id TEXT NOT NULL REFERENCES memory(memory_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    memory_revision INTEGER NOT NULL CHECK (memory_revision > 0),
    selection_rank INTEGER NOT NULL CHECK (selection_rank BETWEEN 0 AND 2),
    used_by_model INTEGER NOT NULL CHECK (used_by_model IN (0,1)),
    PRIMARY KEY(decision_id,memory_id),
    UNIQUE(decision_id,selection_rank)
) STRICT;

CREATE TABLE appearance_preview_v8 (
    decision_id TEXT PRIMARY KEY REFERENCES appearance_decision_v8(decision_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    preview_text TEXT NOT NULL CHECK (length(CAST(preview_text AS BLOB)) BETWEEN 1 AND 2000),
    tone TEXT NOT NULL CHECK (tone IN ('warm','wry','celebratory','reflective','playful')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms)
) STRICT;

INSERT INTO appearance_decision_v8 SELECT * FROM appearance_decision;
INSERT INTO appearance_decision_memory_v8 SELECT * FROM appearance_decision_memory;
INSERT INTO appearance_preview_v8 SELECT * FROM appearance_preview;
CREATE TEMP TABLE appearance_rebuild_verify (
    decision_delta INTEGER NOT NULL CHECK (decision_delta = 0),
    memory_delta INTEGER NOT NULL CHECK (memory_delta = 0),
    preview_delta INTEGER NOT NULL CHECK (preview_delta = 0)
);
INSERT INTO appearance_rebuild_verify
SELECT (SELECT count(*) FROM appearance_decision_v8) -
       (SELECT count(*) FROM appearance_decision),
       (SELECT count(*) FROM appearance_decision_memory_v8) -
       (SELECT count(*) FROM appearance_decision_memory),
       (SELECT count(*) FROM appearance_preview_v8) -
       (SELECT count(*) FROM appearance_preview);
DROP TABLE appearance_rebuild_verify;

DROP TABLE appearance_preview;
DROP TABLE appearance_decision_memory;
DROP TABLE appearance_decision;
ALTER TABLE appearance_decision_v8 RENAME TO appearance_decision;
ALTER TABLE appearance_decision_memory_v8 RENAME TO appearance_decision_memory;
ALTER TABLE appearance_preview_v8 RENAME TO appearance_preview;

CREATE TRIGGER appearance_decision_transition_only
BEFORE UPDATE ON appearance_decision
WHEN NOT (OLD.state='model_pending' AND OLD.revision=1 AND NEW.state='final'
          AND NEW.revision=2 AND NEW.decision_id=OLD.decision_id
          AND NEW.candidate_id=OLD.candidate_id AND NEW.policy_version=OLD.policy_version
          AND NEW.application_instance_id=OLD.application_instance_id
          AND NEW.created_at_ms=OLD.created_at_ms)
BEGIN
    SELECT RAISE(ABORT, 'invalid appearance decision transition');
END;
CREATE TRIGGER appearance_decision_no_delete
BEFORE DELETE ON appearance_decision BEGIN
    SELECT RAISE(ABORT, 'appearance decisions are durable audit records');
END;
CREATE INDEX appearance_decision_recent ON appearance_decision(created_at_ms DESC,decision_id DESC);
CREATE INDEX appearance_decision_action_recent ON appearance_decision(action,finalized_at_ms DESC) WHERE state='final';

CREATE TABLE appearance_budget_reservation (
    reservation_id TEXT PRIMARY KEY CHECK (length(reservation_id) = 36),
    decision_id TEXT NOT NULL UNIQUE REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    candidate_id TEXT NOT NULL UNIQUE REFERENCES appearance_candidate(candidate_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT DEFERRABLE INITIALLY DEFERRED,
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    reserved_at_ms INTEGER NOT NULL CHECK (reserved_at_ms >= 0),
    human_message_count INTEGER NOT NULL CHECK (human_message_count >= 0),
    is_test INTEGER NOT NULL CHECK (is_test IN (0,1))
) STRICT;
CREATE TRIGGER appearance_budget_reservation_no_update
BEFORE UPDATE ON appearance_budget_reservation BEGIN
    SELECT RAISE(ABORT, 'appearance budget reservations are immutable');
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
CREATE INDEX appearance_budget_recent
    ON appearance_budget_reservation(is_test,reserved_at_ms DESC,reservation_id DESC);

CREATE TABLE appearance_delivery_participant (
    decision_id TEXT NOT NULL REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY(decision_id,user_id)
) STRICT;
CREATE TRIGGER appearance_delivery_participant_requires_live_reservation
BEFORE INSERT ON appearance_delivery_participant
WHEN NOT EXISTS (
    SELECT 1 FROM appearance_decision d
    JOIN appearance_budget_reservation r ON r.decision_id=d.decision_id
    WHERE d.decision_id=NEW.decision_id AND d.state='final'
      AND d.action='live_queued'
)
BEGIN
    SELECT RAISE(ABORT, 'appearance delivery participant requires live reservation');
END;
CREATE TRIGGER appearance_delivery_participant_no_update
BEFORE UPDATE ON appearance_delivery_participant BEGIN
    SELECT RAISE(ABORT, 'appearance delivery participants are immutable');
END;
CREATE TRIGGER appearance_delivery_participant_no_delete
BEFORE DELETE ON appearance_delivery_participant BEGIN
    SELECT RAISE(ABORT, 'appearance delivery participants are immutable');
END;

CREATE TABLE appearance_control_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    globally_disabled INTEGER NOT NULL DEFAULT 0 CHECK (globally_disabled IN (0,1)),
    disabled_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    disabled_at_ms INTEGER CHECK (disabled_at_ms IS NULL OR disabled_at_ms >= 0),
    quiet_until_ms INTEGER CHECK (quiet_until_ms IS NULL OR quiet_until_ms >= 0),
    quiet_set_by_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    quiet_reason TEXT CHECK (quiet_reason IS NULL OR quiet_reason IN ('duration','tonight','until','feedback')),
    revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0),
    CHECK ((globally_disabled = 0) = (disabled_by_user_id IS NULL AND disabled_at_ms IS NULL)),
    CHECK ((quiet_until_ms IS NULL) = (quiet_set_by_user_id IS NULL AND quiet_reason IS NULL))
) STRICT;
INSERT INTO appearance_control_state(singleton,updated_at_ms) VALUES(1,0);

CREATE TABLE appearance_feedback_control (
    control_id TEXT PRIMARY KEY CHECK (length(control_id) = 36),
    decision_id TEXT NOT NULL REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    action TEXT NOT NULL CHECK (action IN ('more','less','not_relevant','quiet_tonight')),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms),
    UNIQUE(decision_id,action)
) STRICT;
CREATE TRIGGER appearance_feedback_control_no_update
BEFORE UPDATE ON appearance_feedback_control BEGIN
    SELECT RAISE(ABORT, 'appearance feedback controls are immutable');
END;
CREATE TRIGGER appearance_feedback_control_no_delete
BEFORE DELETE ON appearance_feedback_control BEGIN
    SELECT RAISE(ABORT, 'appearance feedback controls are immutable');
END;

CREATE TABLE appearance_feedback (
    feedback_id TEXT PRIMARY KEY CHECK (length(feedback_id) = 36),
    decision_id TEXT NOT NULL REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    control_id TEXT REFERENCES appearance_feedback_control(control_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    action TEXT NOT NULL CHECK (action IN ('more','less','not_relevant','quiet_tonight')),
    feedback_class TEXT NOT NULL CHECK (feedback_class IN ('sentiment','quiet')),
    interaction_idempotency_key TEXT NOT NULL UNIQUE CHECK (length(interaction_idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE(decision_id,user_id,feedback_class),
    CHECK ((action = 'quiet_tonight') = (feedback_class = 'quiet'))
) STRICT;
CREATE TRIGGER appearance_feedback_no_update
BEFORE UPDATE ON appearance_feedback BEGIN
    SELECT RAISE(ABORT, 'appearance feedback is append-only');
END;
CREATE TRIGGER appearance_feedback_no_delete
BEFORE DELETE ON appearance_feedback BEGIN
    SELECT RAISE(ABORT, 'appearance feedback is append-only');
END;
CREATE INDEX appearance_feedback_recent
    ON appearance_feedback(feedback_class,created_at_ms DESC,feedback_id DESC);

CREATE TABLE chronicle_appearance_source (
    entry_id TEXT PRIMARY KEY REFERENCES chronicle_entry(entry_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    decision_id TEXT NOT NULL UNIQUE REFERENCES appearance_decision(decision_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;
CREATE TRIGGER chronicle_appearance_source_no_update
BEFORE UPDATE ON chronicle_appearance_source BEGIN
    SELECT RAISE(ABORT, 'Chronicle appearance provenance is immutable');
END;
CREATE TRIGGER chronicle_appearance_source_no_delete
BEFORE DELETE ON chronicle_appearance_source BEGIN
    SELECT RAISE(ABORT, 'Chronicle appearance provenance is immutable');
END;

-- Resolve every Chronicle user whose opted-in shared state causally grounded a
-- candidate. Event observations are append-only durable links, so this view
-- covers entry/anniversary, session, and title candidates without counting
-- their source subjects as active conversation humans.
CREATE VIEW appearance_candidate_source_user AS
SELECT cs.candidate_id,cp.user_id
  FROM appearance_candidate_source cs
  JOIN chronicle_participant cp ON cp.entry_id=cs.source_id
 WHERE cs.source_kind='chronicle_entry'
UNION
SELECT cs.candidate_id,cp.user_id
  FROM appearance_candidate_source cs
  JOIN appearance_event_observation o
    ON cs.source_kind='event' AND o.source_event_id=cs.source_id
  JOIN chronicle_participant cp ON cp.entry_id=o.aggregate_id
 WHERE o.event_type IN ('chronicle.entry_canonized.v1',
                        'chronicle.anniversary_delivered.v1')
UNION
SELECT cs.candidate_id,sp.user_id
  FROM appearance_candidate_source cs
  JOIN appearance_event_observation o
    ON cs.source_kind='event' AND o.source_event_id=cs.source_id
  JOIN chronicle_session_participant sp ON sp.session_id=o.aggregate_id
 WHERE o.event_type IN ('chronicle.session_started.v1',
                        'chronicle.session_completed.v1')
UNION
SELECT cs.candidate_id,tg.recipient_user_id
  FROM appearance_candidate_source cs
  JOIN appearance_event_observation o
    ON cs.source_kind='event' AND o.source_event_id=cs.source_id
  JOIN chronicle_title_grant tg ON tg.grant_id=o.aggregate_id
 WHERE o.event_type='chronicle.title_awarded.v1';

CREATE TRIGGER appearance_callback_withdrawal_cancel
AFTER UPDATE OF appearance_callback_opt_in ON user_preference
WHEN OLD.appearance_callback_opt_in=1 AND NEW.appearance_callback_opt_in=0
BEGIN
  UPDATE outbox_message
     SET state='cancelled',lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,
         terminal_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         updated_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         last_error_code='appearance_opt_out'
   WHERE outbox_id IN (
     SELECT r.outbox_id FROM appearance_budget_reservation r
      WHERE EXISTS (SELECT 1 FROM appearance_delivery_participant dp
                    WHERE dp.decision_id=r.decision_id AND dp.user_id=NEW.user_id)
         OR EXISTS (SELECT 1 FROM appearance_candidate_actor a
                    WHERE a.candidate_id=r.candidate_id AND a.user_id=NEW.user_id)
         OR EXISTS (SELECT 1 FROM appearance_decision_memory dm
                    JOIN memory_subject s ON s.memory_id=dm.memory_id
                    WHERE dm.decision_id=r.decision_id
                      AND s.subject_type='user' AND s.subject_id=NEW.user_id)
         OR EXISTS (SELECT 1 FROM appearance_candidate_source_user su
                    WHERE su.candidate_id=r.candidate_id
                      AND su.user_id=NEW.user_id)
   ) AND first_attempt_at_ms IS NULL
     AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

CREATE TRIGGER appearance_memory_callback_withdrawal_cancel
AFTER UPDATE OF memory_callback_opt_in ON user_preference
WHEN OLD.memory_callback_opt_in=1 AND NEW.memory_callback_opt_in=0
BEGIN
  UPDATE outbox_message
     SET state='cancelled',lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,
         terminal_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         updated_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         last_error_code='appearance_opt_out'
   WHERE outbox_id IN (
     SELECT r.outbox_id FROM appearance_budget_reservation r
      WHERE EXISTS (SELECT 1 FROM appearance_decision_memory dm
                    JOIN memory_subject s ON s.memory_id=dm.memory_id
                    WHERE dm.decision_id=r.decision_id
                      AND s.subject_type='user' AND s.subject_id=NEW.user_id)
   ) AND first_attempt_at_ms IS NULL
     AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

CREATE TRIGGER appearance_chronicle_preference_withdrawal_cancel
AFTER UPDATE OF chronicle_opt_in ON user_preference
WHEN OLD.chronicle_opt_in=1 AND NEW.chronicle_opt_in=0
BEGIN
  UPDATE outbox_message
     SET state='cancelled',lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,
         terminal_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         updated_at_ms=max(NEW.updated_at_ms,created_at_ms,updated_at_ms),
         last_error_code='appearance_opt_out'
   WHERE outbox_id IN (
     SELECT r.outbox_id FROM appearance_budget_reservation r
      WHERE EXISTS (SELECT 1 FROM appearance_decision_memory dm
                    JOIN memory_subject s ON s.memory_id=dm.memory_id
                    WHERE dm.decision_id=r.decision_id
                      AND s.subject_type='user' AND s.subject_id=NEW.user_id)
         OR EXISTS (SELECT 1 FROM appearance_candidate_source_user su
                    WHERE su.candidate_id=r.candidate_id
                      AND su.user_id=NEW.user_id)
   ) AND first_attempt_at_ms IS NULL
     AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

CREATE TRIGGER appearance_memory_withdrawal_cancel
AFTER UPDATE OF status,revision ON memory
WHEN OLD.status='confirmed' AND (NEW.status<>'confirmed' OR NEW.revision<>OLD.revision)
BEGIN
  UPDATE outbox_message
     SET state='cancelled',lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,
         terminal_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.expired_at_ms,NEW.confirmed_at_ms),created_at_ms,updated_at_ms),
         updated_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.expired_at_ms,NEW.confirmed_at_ms),created_at_ms,updated_at_ms),
         last_error_code='appearance_memory_withdrawn'
   WHERE outbox_id IN (
     SELECT r.outbox_id FROM appearance_budget_reservation r
     JOIN appearance_decision_memory dm ON dm.decision_id=r.decision_id
     WHERE dm.memory_id=NEW.memory_id
       AND (NEW.status<>'confirmed' OR dm.memory_revision<>NEW.revision)
   ) AND first_attempt_at_ms IS NULL
     AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

CREATE TRIGGER appearance_chronicle_withdrawal_cancel
AFTER UPDATE OF status,visibility,revision ON chronicle_entry
WHEN OLD.status='canon' AND
     (NEW.status<>'canon' OR NEW.visibility<>'shared' OR NEW.revision<>OLD.revision)
BEGIN
  UPDATE outbox_message
     SET state='cancelled',lease_owner=NULL,lease_token=NULL,lease_until_ms=NULL,
         terminal_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.created_at_ms),created_at_ms,updated_at_ms),
         updated_at_ms=max(COALESCE(NEW.retracted_at_ms,NEW.created_at_ms),created_at_ms,updated_at_ms),
         last_error_code='appearance_chronicle_withdrawn'
   WHERE outbox_id IN (
     SELECT r.outbox_id FROM appearance_budget_reservation r
     JOIN appearance_candidate_source cs ON cs.candidate_id=r.candidate_id
     LEFT JOIN appearance_event_observation o
       ON cs.source_kind='event' AND o.source_event_id=cs.source_id
     WHERE (cs.source_kind='chronicle_entry' AND cs.source_id=NEW.entry_id)
        OR (o.event_type IN ('chronicle.entry_canonized.v1',
                             'chronicle.anniversary_delivered.v1')
            AND o.aggregate_id=NEW.entry_id)
   ) AND first_attempt_at_ms IS NULL
     AND (state='pending' OR (state='claimed' AND submission_started_at_ms IS NULL));
END;

CREATE TABLE appearance_alert_state (
    category TEXT PRIMARY KEY CHECK (category IN ('model','delivery')),
    last_alert_at_ms INTEGER CHECK (last_alert_at_ms IS NULL OR last_alert_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
) STRICT;
INSERT INTO appearance_alert_state(category,updated_at_ms) VALUES('model',0),('delivery',0);

-- This is intentionally broader than the application path: any row that
-- names an appearance decision/candidate must satisfy the complete live
-- reservation invariant. Thus off/dry-run and wrong-scope SQL fail closed.
CREATE TRIGGER appearance_live_outbox_guard
BEFORE INSERT ON outbox_message
WHEN NEW.kind LIKE 'appearance.%'
  OR NEW.aggregate_type IN ('appearance','appearance_candidate','appearance_decision')
  OR NEW.aggregate_id IN (SELECT candidate_id FROM appearance_candidate)
  OR NEW.aggregate_id IN (SELECT decision_id FROM appearance_decision)
BEGIN
  SELECT CASE WHEN NOT EXISTS (
    SELECT 1
      FROM appearance_decision d
      JOIN appearance_candidate c ON c.candidate_id=d.candidate_id
      JOIN appearance_budget_reservation r
        ON r.decision_id=d.decision_id AND r.candidate_id=c.candidate_id
       AND r.outbox_id=NEW.outbox_id
      JOIN appearance_mode_state m ON m.singleton=1
      JOIN appearance_control_state ctl ON ctl.singleton=1
      JOIN guild_config g ON g.singleton=1
      JOIN event_journal e
        ON e.event_id=json_extract(NEW.payload_json,'$.causation_event_id')
       AND e.event_type='appearance.live_queued.v1'
       AND e.aggregate_type='appearance_decision'
       AND e.aggregate_id=d.decision_id
     WHERE d.decision_id=NEW.aggregate_id
       AND d.state='final' AND d.action='live_queued'
       AND m.mode='live' AND c.mode_activated_at_ms=m.activated_at_ms
       AND ctl.globally_disabled=0
       AND (ctl.quiet_until_ms IS NULL OR ctl.quiet_until_ms<=NEW.created_at_ms)
       AND NEW.kind='discord.public.v1'
       AND NEW.aggregate_type='appearance_decision'
       AND NEW.target_guild_id=g.guild_id
       AND NEW.target_channel_id=g.primary_channel_id
       AND NEW.target_user_id IS NULL
       AND NEW.max_attempts=5
       AND NEW.idempotency_key='appearance.public:' || d.decision_id
       AND r.idempotency_key='appearance.reservation:' || d.decision_id
       AND NEW.provider_nonce=
           substr(replace(NEW.outbox_id,'-',''),1,12) ||
           substr(replace(NEW.outbox_id,'-',''),20,13)
       AND (SELECT count(*) FROM json_each(NEW.payload_json))=10
       AND json_type(NEW.payload_json,'$.payload_version')='integer'
       AND json_extract(NEW.payload_json,'$.payload_version')=1
       AND json_type(NEW.payload_json,'$.guild_id')='text'
       AND json_type(NEW.payload_json,'$.channel_id')='text'
       AND json_extract(NEW.payload_json,'$.guild_id')=g.guild_id
       AND json_extract(NEW.payload_json,'$.channel_id')=g.primary_channel_id
       AND json_type(NEW.payload_json,'$.content')='text'
       AND length(json_extract(NEW.payload_json,'$.content')) BETWEEN 1 AND 500
       AND instr(json_extract(NEW.payload_json,'$.content'),char(10))=0
       AND instr(json_extract(NEW.payload_json,'$.content'),char(13))=0
       AND json_type(NEW.payload_json,'$.embed')='null'
       AND json_type(NEW.payload_json,'$.allowed_user_mentions')='array'
       AND json_array_length(json_extract(NEW.payload_json,'$.allowed_user_mentions'))=0
       AND json_type(NEW.payload_json,'$.buttons')='array'
       AND json_array_length(json_extract(NEW.payload_json,'$.buttons'))=4
       AND json_type(NEW.payload_json,'$.fail_before_first_send')='false'
       AND json_extract(NEW.payload_json,'$.correlation_id')='appearance-live'
       AND (SELECT count(*) FROM appearance_feedback_control fc
            WHERE fc.decision_id=d.decision_id)=4
       AND NOT EXISTS (
         SELECT 1 FROM json_each(json_extract(NEW.payload_json,'$.buttons')) b
         LEFT JOIN appearance_feedback_control fc
           ON fc.decision_id=d.decision_id
          AND 'sga:1:' || fc.control_id=json_extract(b.value,'$.custom_id')
        WHERE fc.control_id IS NULL
           OR json_type(b.value)<>'object'
           OR (SELECT count(*) FROM json_each(b.value))<>4
           OR json_type(b.value,'$.label')<>'text'
           OR json_extract(b.value,'$.label')<>CASE fc.action
                WHEN 'more' THEN 'More like this'
                WHEN 'less' THEN 'Less like this'
                WHEN 'not_relevant' THEN 'Not relevant'
                WHEN 'quiet_tonight' THEN 'Quiet for tonight'
              END
           OR json_type(b.value,'$.disabled')<>'false'
           OR json_extract(b.value,'$.style')<>'secondary'
       )
       AND (SELECT count(DISTINCT fc.control_id)
              FROM json_each(json_extract(NEW.payload_json,'$.buttons')) b
              JOIN appearance_feedback_control fc
                ON fc.decision_id=d.decision_id
               AND 'sga:1:' || fc.control_id=json_extract(b.value,'$.custom_id'))=4
  ) THEN RAISE(ABORT, 'invalid live appearance public outbox') END;
END;

-- The application inserts a deferred reservation before its outbox row. Keep
-- this small guard separate from the complete live predicate so an unrelated
-- row cannot hide behind that forward order without increasing the runtime's
-- deliberately low SQLite expression-depth limit.
CREATE TRIGGER appearance_reservation_linked_outbox_guard
BEFORE INSERT ON outbox_message
WHEN NEW.outbox_id IN (SELECT outbox_id FROM appearance_budget_reservation)
BEGIN
  SELECT CASE WHEN NOT EXISTS (
    SELECT 1 FROM appearance_budget_reservation r
    JOIN appearance_delivery_participant dp ON dp.decision_id=r.decision_id
    WHERE r.outbox_id=NEW.outbox_id
      AND NEW.kind='discord.public.v1'
      AND NEW.aggregate_type='appearance_decision'
      AND NEW.aggregate_id=r.decision_id
  ) THEN RAISE(ABORT, 'invalid reservation-linked appearance outbox') END;
END;

-- Delivery workers may update only lease, attempt, receipt, and terminal-state
-- columns. The identity and user-visible payload of an appearance row are
-- immutable after the guarded insert, and an unrelated row cannot be changed
-- into an appearance row to bypass the insert guard.
CREATE TRIGGER appearance_live_outbox_identity_immutable
BEFORE UPDATE OF outbox_id,kind,aggregate_type,aggregate_id,target_guild_id,
                 target_channel_id,target_user_id,payload_json,max_attempts,
                 idempotency_key,provider_nonce,created_at_ms
ON outbox_message
WHEN OLD.outbox_id IN (SELECT outbox_id FROM appearance_budget_reservation)
  OR NEW.outbox_id IN (SELECT outbox_id FROM appearance_budget_reservation)
  OR OLD.kind LIKE 'appearance.%' OR NEW.kind LIKE 'appearance.%'
  OR OLD.aggregate_type IN ('appearance','appearance_candidate','appearance_decision')
  OR NEW.aggregate_type IN ('appearance','appearance_candidate','appearance_decision')
  OR OLD.aggregate_id IN (SELECT candidate_id FROM appearance_candidate)
  OR NEW.aggregate_id IN (SELECT candidate_id FROM appearance_candidate)
  OR OLD.aggregate_id IN (SELECT decision_id FROM appearance_decision)
  OR NEW.aggregate_id IN (SELECT decision_id FROM appearance_decision)
BEGIN
  SELECT RAISE(ABORT, 'appearance public outbox identity is immutable');
END;
