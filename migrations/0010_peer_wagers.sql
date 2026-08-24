-- Milestone 12: two-party Fate wagers.  The ledger parent and every table
-- carrying a foreign key to it are rebuilt together so schema-v9 rows retain
-- their sequence, identity, and referential integrity while the set of legal
-- transaction types is widened.

DROP TRIGGER tarot_transaction_insert_prepared;
DROP TRIGGER tarot_posting_insert_only_prepared;
DROP TRIGGER tarot_posting_no_update;
DROP TRIGGER tarot_posting_no_delete;
DROP TRIGGER tarot_transaction_seal;
DROP TRIGGER tarot_transaction_update_guard;
DROP TRIGGER tarot_transaction_no_delete;
DROP TRIGGER tarot_recovery_transaction_seal_link;
DROP TRIGGER tarot_recovery_transition_guard;
DROP TRIGGER tarot_recovery_cancel_terminal_tokens;
DROP TRIGGER tarot_recovery_privacy_tighten;
DROP TRIGGER tarot_recovery_no_delete;
DROP TRIGGER tarot_interaction_receipt_no_update;
DROP TRIGGER tarot_interaction_receipt_no_delete;

DROP INDEX tarot_posting_account_history;
DROP INDEX tarot_transaction_idempotency;
DROP INDEX tarot_transaction_reversal;
DROP INDEX tarot_recovery_active;
DROP INDEX tarot_recovery_cooldown;
DROP INDEX tarot_recovery_expiry;
DROP INDEX tarot_interaction_receipt_account;

ALTER TABLE tarot_history_item RENAME TO tarot_history_item_v9;
ALTER TABLE tarot_interaction_receipt RENAME TO tarot_interaction_receipt_v9;
ALTER TABLE tarot_recovery_claim RENAME TO tarot_recovery_claim_v9;
ALTER TABLE tarot_posting RENAME TO tarot_posting_v9;
ALTER TABLE tarot_transaction RENAME TO tarot_transaction_v9;

CREATE TABLE tarot_transaction (
    ledger_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    transaction_id TEXT NOT NULL UNIQUE
        CHECK (length(transaction_id) = 36
               AND substr(transaction_id, 9, 1) = '-'
               AND substr(transaction_id, 14, 1) = '-'
               AND substr(transaction_id, 19, 1) = '-'
               AND substr(transaction_id, 24, 1) = '-'
               AND length(replace(transaction_id, '-', '')) = 32
               AND transaction_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(transaction_id, 15, 1) = '4'
               AND substr(transaction_id, 20, 1) IN ('8', '9', 'a', 'b')),
    transaction_type TEXT NOT NULL
        CHECK (transaction_type IN ('STARTING_GRANT', 'GRACE', 'TRIAL',
                                    'TEST_ADJUSTMENT', 'TEST_REVERSAL',
                                    'WAGER_ESCROW_FUND', 'WAGER_PAYOUT',
                                    'WAGER_REFUND')),
    state TEXT NOT NULL CHECK (state IN ('prepared', 'committed')),
    expected_posting_count INTEGER NOT NULL
        CHECK (expected_posting_count BETWEEN 2 AND 16),
    event_id TEXT NOT NULL UNIQUE
        REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    actor_user_id TEXT
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT CHECK (reason IS NULL OR length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    is_test INTEGER NOT NULL CHECK (is_test IN (0, 1)),
    reversal_of_transaction_id TEXT UNIQUE
        REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    committed_at_ms INTEGER
        CHECK (committed_at_ms IS NULL OR committed_at_ms >= created_at_ms),
    CHECK ((state = 'committed') = (committed_at_ms IS NOT NULL)),
    CHECK ((transaction_type = 'TEST_REVERSAL') =
           (reversal_of_transaction_id IS NOT NULL)),
    CHECK (transaction_type <> 'STARTING_GRANT' OR is_test = 0),
    CHECK (transaction_type NOT IN ('TEST_ADJUSTMENT', 'TEST_REVERSAL')
           OR is_test = 1)
) STRICT;

INSERT INTO tarot_transaction
    (ledger_sequence, transaction_id, transaction_type, state,
     expected_posting_count, event_id, idempotency_key, actor_user_id, reason,
     is_test, reversal_of_transaction_id, created_at_ms, committed_at_ms)
SELECT ledger_sequence, transaction_id, transaction_type, state,
       expected_posting_count, event_id, idempotency_key, actor_user_id, reason,
       is_test, reversal_of_transaction_id, created_at_ms, committed_at_ms
FROM tarot_transaction_v9 ORDER BY ledger_sequence;

CREATE TABLE tarot_posting (
    posting_id TEXT PRIMARY KEY
        CHECK (length(posting_id) = 36
               AND substr(posting_id, 9, 1) = '-'
               AND substr(posting_id, 14, 1) = '-'
               AND substr(posting_id, 19, 1) = '-'
               AND substr(posting_id, 24, 1) = '-'
               AND length(replace(posting_id, '-', '')) = 32
               AND posting_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(posting_id, 15, 1) = '4'
               AND substr(posting_id, 20, 1) IN ('8', '9', 'a', 'b')),
    transaction_id TEXT NOT NULL
        REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    account_id TEXT NOT NULL
        REFERENCES tarot_account(account_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    amount INTEGER NOT NULL
        CHECK (amount <> 0 AND amount BETWEEN -1000000000 AND 1000000000),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE (transaction_id, account_id)
) STRICT;

INSERT INTO tarot_posting
    (posting_id, transaction_id, account_id, amount, created_at_ms)
SELECT posting_id, transaction_id, account_id, amount, created_at_ms
FROM tarot_posting_v9;

CREATE TABLE tarot_recovery_claim (
    claim_id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL
        REFERENCES tarot_account(account_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    claim_type TEXT NOT NULL CHECK (claim_type IN ('GRACE', 'TRIAL')),
    state TEXT NOT NULL
        CHECK (state IN ('pending', 'completed', 'expired', 'abandoned')),
    visibility TEXT NOT NULL CHECK (visibility IN ('public', 'private')),
    is_test INTEGER NOT NULL CHECK (is_test IN (0, 1)),
    eligibility_threshold INTEGER NOT NULL
        CHECK (eligibility_threshold BETWEEN 1 AND 1000000000),
    grace_target INTEGER
        CHECK (grace_target IS NULL OR grace_target BETWEEN 1 AND 1000000000),
    eligibility_balance INTEGER NOT NULL CHECK (eligibility_balance >= 0),
    reward INTEGER CHECK (reward IS NULL OR reward BETWEEN 1 AND 1000000000),
    draw_id TEXT UNIQUE REFERENCES tarot_draw(draw_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    transaction_id TEXT UNIQUE REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    started_event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_id TEXT REFERENCES event_journal(event_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    outbox_id TEXT REFERENCES outbox_message(outbox_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    start_idempotency_key TEXT NOT NULL UNIQUE
        CHECK (length(start_idempotency_key) BETWEEN 1 AND 160),
    completion_idempotency_key TEXT UNIQUE
        CHECK (completion_idempotency_key IS NULL OR length(completion_idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms),
    completed_at_ms INTEGER CHECK (completed_at_ms IS NULL OR completed_at_ms >= created_at_ms),
    cooldown_until_ms INTEGER CHECK (cooldown_until_ms IS NULL OR cooldown_until_ms >= completed_at_ms),
    CHECK ((claim_type = 'GRACE') = (grace_target IS NOT NULL)),
    CHECK (claim_type <> 'GRACE' OR grace_target > eligibility_threshold),
    CHECK ((claim_type = 'TRIAL') = (draw_id IS NOT NULL)),
    CHECK ((state = 'pending' AND completed_at_ms IS NULL
            AND transaction_id IS NULL AND event_id IS NULL
            AND cooldown_until_ms IS NULL)
           OR (state = 'completed' AND completed_at_ms IS NOT NULL
               AND transaction_id IS NOT NULL AND event_id IS NOT NULL
               AND cooldown_until_ms IS NOT NULL)
           OR (state IN ('expired', 'abandoned')
               AND completed_at_ms IS NOT NULL AND transaction_id IS NULL
               AND event_id IS NOT NULL AND cooldown_until_ms IS NULL))
) STRICT;

INSERT INTO tarot_recovery_claim
SELECT * FROM tarot_recovery_claim_v9;

CREATE TABLE tarot_interaction_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    operation TEXT NOT NULL
        CHECK (operation IN ('standings_visibility', 'recovery_start',
                             'adjust', 'reverse')),
    account_id TEXT NOT NULL REFERENCES tarot_account(account_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    request_json TEXT NOT NULL
        CHECK (json_valid(request_json) AND json_type(request_json) = 'object'
               AND length(CAST(request_json AS BLOB)) BETWEEN 2 AND 2048),
    result_json TEXT NOT NULL
        CHECK (json_valid(result_json) AND json_type(result_json) = 'object'
               AND length(CAST(result_json AS BLOB)) BETWEEN 2 AND 2048),
    claim_id TEXT REFERENCES tarot_recovery_claim(claim_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    transaction_id TEXT REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK (claim_id IS NULL OR operation = 'recovery_start'),
    CHECK (transaction_id IS NULL OR operation IN ('adjust', 'reverse'))
) STRICT;

INSERT INTO tarot_interaction_receipt
SELECT * FROM tarot_interaction_receipt_v9;

CREATE TABLE tarot_history_item (
    cursor_id TEXT NOT NULL REFERENCES tarot_history_cursor(cursor_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 49),
    transaction_id TEXT NOT NULL REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY (cursor_id, position)
) STRICT;

INSERT INTO tarot_history_item SELECT * FROM tarot_history_item_v9;

CREATE TEMP TABLE tarot_v10_copy_verification (
    verified INTEGER NOT NULL CHECK (verified = 1)
) STRICT;
INSERT INTO tarot_v10_copy_verification VALUES (
    (SELECT count(*) FROM tarot_transaction) =
        (SELECT count(*) FROM tarot_transaction_v9)
    AND (SELECT count(*) FROM tarot_posting) =
        (SELECT count(*) FROM tarot_posting_v9)
    AND (SELECT count(*) FROM tarot_recovery_claim) =
        (SELECT count(*) FROM tarot_recovery_claim_v9)
    AND (SELECT count(*) FROM tarot_interaction_receipt) =
        (SELECT count(*) FROM tarot_interaction_receipt_v9)
    AND (SELECT count(*) FROM tarot_history_item) =
        (SELECT count(*) FROM tarot_history_item_v9)
);
DROP TABLE tarot_v10_copy_verification;

DROP TABLE tarot_history_item_v9;
DROP TABLE tarot_interaction_receipt_v9;
DROP TABLE tarot_recovery_claim_v9;
DROP TABLE tarot_posting_v9;
DROP TABLE tarot_transaction_v9;

CREATE INDEX tarot_posting_account_history ON tarot_posting(account_id, transaction_id);
CREATE INDEX tarot_transaction_idempotency ON tarot_transaction(idempotency_key);
CREATE INDEX tarot_transaction_reversal ON tarot_transaction(reversal_of_transaction_id)
    WHERE reversal_of_transaction_id IS NOT NULL;
CREATE UNIQUE INDEX tarot_recovery_active
    ON tarot_recovery_claim(account_id, claim_type, is_test) WHERE state = 'pending';
CREATE INDEX tarot_recovery_cooldown
    ON tarot_recovery_claim(account_id, claim_type, is_test, cooldown_until_ms DESC)
    WHERE state = 'completed';
CREATE INDEX tarot_recovery_expiry
    ON tarot_recovery_claim(expires_at_ms) WHERE state = 'pending';
CREATE INDEX tarot_interaction_receipt_account
    ON tarot_interaction_receipt(account_id, operation, created_at_ms);

CREATE TABLE tarot_wager (
    wager_id TEXT PRIMARY KEY
        CHECK (length(wager_id) = 36
               AND substr(wager_id, 9, 1) = '-'
               AND substr(wager_id, 14, 1) = '-'
               AND substr(wager_id, 19, 1) = '-'
               AND substr(wager_id, 24, 1) = '-'
               AND length(replace(wager_id, '-', '')) = 32
               AND substr(wager_id, 15, 1) = '4'
               AND substr(wager_id, 20, 1) IN ('8', '9', 'a', 'b')
               AND wager_id NOT GLOB '*[^0-9a-f-]*'),
    state TEXT NOT NULL CHECK (state IN
        ('draft', 'offered', 'accepted_funded', 'awaiting_resolution',
         'disputed', 'resolved', 'void_refunded', 'cancelled', 'declined',
         'expired')),
    revision INTEGER NOT NULL CHECK (revision >= 1),
    guild_id TEXT NOT NULL REFERENCES guild_config(guild_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    channel_id TEXT NOT NULL,
    creator_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    target_user_id TEXT NOT NULL REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    judge_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    visibility TEXT NOT NULL CHECK (visibility IN ('public', 'sealed')),
    resolution_policy TEXT NOT NULL CHECK (resolution_policy IN ('mutual', 'designated')),
    proposition TEXT CHECK (proposition IS NULL OR length(CAST(proposition AS BLOB)) BETWEEN 1 AND 500),
    stake INTEGER CHECK (stake IS NULL OR stake BETWEEN 1 AND 100),
    evidence_instructions TEXT CHECK (evidence_instructions IS NULL OR length(CAST(evidence_instructions AS BLOB)) BETWEEN 1 AND 500),
    outcome_window_ms INTEGER NOT NULL CHECK (outcome_window_ms BETWEEN 3600000 AND 604800000),
    resolution_grace_ms INTEGER NOT NULL CHECK (resolution_grace_ms BETWEEN 3600000 AND 604800000),
    offer_duration_ms INTEGER CHECK (offer_duration_ms IS NULL OR
                                     offer_duration_ms BETWEEN 1 AND 31536000000),
    offer_expires_at_ms INTEGER,
    outcome_due_at_ms INTEGER,
    resolution_grace_until_ms INTEGER,
    winner_role TEXT CHECK (winner_role IS NULL OR winner_role IN ('creator', 'target')),
    terminal_reason TEXT CHECK (terminal_reason IS NULL OR length(CAST(terminal_reason AS BLOB)) BETWEEN 1 AND 200),
    judged_by_user_id TEXT REFERENCES discord_user(user_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    fund_transaction_id TEXT UNIQUE REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    settlement_transaction_id TEXT UNIQUE REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    is_test INTEGER NOT NULL CHECK (is_test IN (0, 1)),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms),
    terminal_at_ms INTEGER CHECK (terminal_at_ms IS NULL OR terminal_at_ms >= created_at_ms),
    CHECK ((resolution_policy = 'designated') = (judge_user_id IS NOT NULL)),
    CHECK ((is_test = 1) = (creator_user_id = target_user_id)),
    CHECK (judge_user_id IS NULL OR
           (is_test = 1 AND judge_user_id = creator_user_id) OR
           (is_test = 0 AND judge_user_id <> creator_user_id AND
            judge_user_id <> target_user_id)),
    CHECK ((state IN ('draft', 'cancelled', 'expired')) OR
           (proposition IS NOT NULL AND stake IS NOT NULL)),
    CHECK ((proposition IS NULL AND stake IS NULL AND offer_duration_ms IS NULL) OR
           (proposition IS NOT NULL AND stake IS NOT NULL AND offer_duration_ms IS NOT NULL)),
    CHECK ((state IN ('offered', 'accepted_funded', 'awaiting_resolution', 'disputed',
                      'resolved', 'void_refunded', 'cancelled', 'declined', 'expired')) =
           (offer_expires_at_ms IS NOT NULL)),
    CHECK ((state IN ('accepted_funded', 'awaiting_resolution', 'disputed',
                      'resolved', 'void_refunded')) =
           (fund_transaction_id IS NOT NULL)),
    CHECK ((state IN ('accepted_funded', 'awaiting_resolution', 'disputed',
                      'resolved', 'void_refunded')) =
           (outcome_due_at_ms IS NOT NULL AND resolution_grace_until_ms IS NOT NULL)),
    CHECK ((state IN ('resolved', 'void_refunded')) =
           (settlement_transaction_id IS NOT NULL)),
    CHECK ((state IN ('resolved', 'void_refunded', 'cancelled', 'declined', 'expired')) =
           (terminal_at_ms IS NOT NULL)),
    CHECK ((state = 'resolved') = (winner_role IS NOT NULL)),
    CHECK (state <> 'void_refunded' OR winner_role IS NULL),
    CHECK ((terminal_reason IS NULL) = (judged_by_user_id IS NULL)),
    CHECK (state IN ('resolved', 'void_refunded') OR
           (terminal_reason IS NULL AND judged_by_user_id IS NULL))
) STRICT;

CREATE INDEX tarot_wager_participant_creator ON tarot_wager(creator_user_id, updated_at_ms DESC);
CREATE INDEX tarot_wager_participant_target ON tarot_wager(target_user_id, updated_at_ms DESC);
CREATE INDEX tarot_wager_judge ON tarot_wager(judge_user_id, updated_at_ms DESC) WHERE judge_user_id IS NOT NULL;
CREATE INDEX tarot_wager_state_due ON tarot_wager(state, offer_expires_at_ms, outcome_due_at_ms, resolution_grace_until_ms);

CREATE TRIGGER tarot_wager_human_roles
BEFORE INSERT ON tarot_wager
WHEN EXISTS (
  SELECT 1 FROM discord_user user
  WHERE user.user_id IN (NEW.creator_user_id, NEW.target_user_id,
                         COALESCE(NEW.judge_user_id, NEW.creator_user_id))
    AND user.is_bot = 1)
BEGIN SELECT RAISE(ABORT, 'wager roles require human users'); END;

CREATE TRIGGER tarot_wager_insert_pristine_draft
BEFORE INSERT ON tarot_wager
WHEN NEW.state <> 'draft'
  OR NEW.revision <> 1
  OR NEW.proposition IS NOT NULL
  OR NEW.stake IS NOT NULL
  OR NEW.evidence_instructions IS NOT NULL
  OR NEW.offer_duration_ms IS NOT NULL
  OR NEW.offer_expires_at_ms IS NOT NULL
  OR NEW.outcome_due_at_ms IS NOT NULL
  OR NEW.resolution_grace_until_ms IS NOT NULL
  OR NEW.winner_role IS NOT NULL
  OR NEW.terminal_reason IS NOT NULL
  OR NEW.judged_by_user_id IS NOT NULL
  OR NEW.fund_transaction_id IS NOT NULL
  OR NEW.settlement_transaction_id IS NOT NULL
  OR NEW.updated_at_ms <> NEW.created_at_ms
  OR NEW.terminal_at_ms IS NOT NULL
BEGIN SELECT RAISE(ABORT, 'wagers must begin as pristine drafts'); END;

CREATE TABLE tarot_wager_action (
    action_id TEXT PRIMARY KEY
        CHECK (length(action_id) = 36
               AND substr(action_id, 9, 1) = '-'
               AND substr(action_id, 14, 1) = '-'
               AND substr(action_id, 19, 1) = '-'
               AND substr(action_id, 24, 1) = '-'
               AND length(replace(action_id, '-', '')) = 32
               AND action_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(action_id, 15, 1) = '4'
               AND substr(action_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    wager_revision INTEGER NOT NULL CHECK (wager_revision >= 1),
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_role TEXT NOT NULL CHECK (actor_role IN ('creator', 'target', 'judge', 'owner', 'scheduler')),
    action TEXT NOT NULL CHECK (action IN
        ('drafted', 'previewed', 'confirmed', 'discarded', 'accepted', 'declined',
         'cancelled', 'expired', 'outcome_submitted', 'disputed', 'agreed',
         'void_consented', 'judged', 'reminded', 'outcome_due', 'grace_elapsed',
         'evidence_added', 'test_role_set', 'test_cleaned')),
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT CHECK (reason IS NULL OR length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    occurred_at_ms INTEGER NOT NULL CHECK (occurred_at_ms >= 0)
) STRICT;
CREATE INDEX tarot_wager_action_history ON tarot_wager_action(wager_id, occurred_at_ms, action_id);

CREATE TABLE tarot_wager_outcome (
    submission_id TEXT PRIMARY KEY
        CHECK (length(submission_id) = 36
               AND substr(submission_id, 9, 1) = '-'
               AND substr(submission_id, 14, 1) = '-'
               AND substr(submission_id, 19, 1) = '-'
               AND substr(submission_id, 24, 1) = '-'
               AND length(replace(submission_id, '-', '')) = 32
               AND submission_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(submission_id, 15, 1) = '4'
               AND substr(submission_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_role TEXT NOT NULL CHECK (actor_role IN ('creator', 'target')),
    winner_role TEXT NOT NULL CHECK (winner_role IN ('creator', 'target')),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE (wager_id, actor_role)
) STRICT;

CREATE TABLE tarot_wager_void_consent (
    consent_id TEXT PRIMARY KEY
        CHECK (length(consent_id) = 36
               AND substr(consent_id, 9, 1) = '-'
               AND substr(consent_id, 14, 1) = '-'
               AND substr(consent_id, 19, 1) = '-'
               AND substr(consent_id, 24, 1) = '-'
               AND length(replace(consent_id, '-', '')) = 32
               AND consent_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(consent_id, 15, 1) = '4'
               AND substr(consent_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_role TEXT NOT NULL CHECK (actor_role IN ('creator', 'target')),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE (wager_id, actor_role)
) STRICT;

CREATE TABLE tarot_wager_evidence (
    evidence_id TEXT PRIMARY KEY
        CHECK (length(evidence_id) = 36
               AND substr(evidence_id, 9, 1) = '-'
               AND substr(evidence_id, 14, 1) = '-'
               AND substr(evidence_id, 19, 1) = '-'
               AND substr(evidence_id, 24, 1) = '-'
               AND length(replace(evidence_id, '-', '')) = 32
               AND evidence_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(evidence_id, 15, 1) = '4'
               AND substr(evidence_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_role TEXT NOT NULL CHECK (actor_role IN ('creator', 'target')),
    body TEXT NOT NULL CHECK (length(CAST(body AS BLOB)) BETWEEN 1 AND 1000),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;
CREATE INDEX tarot_wager_evidence_history ON tarot_wager_evidence(wager_id, created_at_ms, evidence_id);

CREATE TABLE tarot_wager_resolution (
    resolution_id TEXT PRIMARY KEY
        CHECK (length(resolution_id) = 36
               AND substr(resolution_id, 9, 1) = '-'
               AND substr(resolution_id, 14, 1) = '-'
               AND substr(resolution_id, 19, 1) = '-'
               AND substr(resolution_id, 24, 1) = '-'
               AND length(replace(resolution_id, '-', '')) = 32
               AND resolution_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(resolution_id, 15, 1) = '4'
               AND substr(resolution_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL UNIQUE REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    result TEXT NOT NULL CHECK (result IN ('creator', 'target', 'void')),
    authority TEXT NOT NULL CHECK (authority IN ('mutual', 'judge', 'owner')),
    actor_user_id TEXT REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT CHECK (reason IS NULL OR length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    event_id TEXT NOT NULL UNIQUE REFERENCES event_journal(event_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((authority = 'mutual') = (actor_user_id IS NULL)),
    CHECK ((authority <> 'mutual') = (reason IS NOT NULL))
) STRICT;

CREATE TABLE tarot_wager_transfer (
    transfer_id TEXT PRIMARY KEY
        CHECK (length(transfer_id) = 36
               AND substr(transfer_id, 9, 1) = '-'
               AND substr(transfer_id, 14, 1) = '-'
               AND substr(transfer_id, 19, 1) = '-'
               AND substr(transfer_id, 24, 1) = '-'
               AND length(replace(transfer_id, '-', '')) = 32
               AND transfer_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(transfer_id, 15, 1) = '4'
               AND substr(transfer_id, 20, 1) IN ('8', '9', 'a', 'b')),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    transfer_kind TEXT NOT NULL CHECK (transfer_kind IN ('fund', 'payout', 'refund', 'test_cleanup')),
    transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    UNIQUE (wager_id, transfer_kind)
) STRICT;

CREATE TABLE tarot_wager_control (
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
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    action TEXT NOT NULL CHECK (action IN
        ('open_form', 'confirm', 'discard', 'accept', 'decline', 'cancel',
         'dispute', 'agree', 'void', 'evidence', 'outcome')),
    expected_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    simulated_role TEXT CHECK (simulated_role IS NULL OR simulated_role IN ('creator', 'target', 'judge', 'owner')),
    expected_revision INTEGER NOT NULL CHECK (expected_revision >= 1),
    state TEXT NOT NULL CHECK (state IN ('active', 'used', 'cancelled')),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms >= created_at_ms),
    used_at_ms INTEGER CHECK (used_at_ms IS NULL OR used_at_ms >= created_at_ms),
    idempotency_key TEXT NOT NULL UNIQUE CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((state = 'used') = (used_at_ms IS NOT NULL))
) STRICT;
CREATE INDEX tarot_wager_control_active ON tarot_wager_control(wager_id, action, state);

CREATE TRIGGER tarot_wager_control_test_role
BEFORE INSERT ON tarot_wager_control
WHEN (NEW.simulated_role IS NOT NULL) <>
     ((SELECT is_test FROM tarot_wager WHERE wager_id = NEW.wager_id) = 1)
BEGIN SELECT RAISE(ABORT, 'simulated wager roles require a test wager'); END;

CREATE TABLE tarot_wager_receipt (
    idempotency_key TEXT PRIMARY KEY CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    wager_id TEXT REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    operation TEXT NOT NULL CHECK (length(operation) BETWEEN 1 AND 40),
    request_fingerprint TEXT NOT NULL CHECK (length(request_fingerprint) BETWEEN 1 AND 512),
    status TEXT NOT NULL CHECK (status IN
        ('applied', 'unchanged', 'forbidden', 'invalid_state', 'expired',
         'insufficient_funds', 'not_found', 'stale')),
    result_json TEXT NOT NULL CHECK (json_valid(result_json) AND length(CAST(result_json AS BLOB)) BETWEEN 2 AND 4096),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TABLE tarot_wager_job (
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    phase TEXT NOT NULL CHECK (phase IN ('draft_expiry', 'offer_expiry', 'reminder', 'outcome_due', 'grace')),
    job_id TEXT NOT NULL UNIQUE REFERENCES scheduled_job(job_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    expected_revision INTEGER NOT NULL CHECK (expected_revision >= 1),
    PRIMARY KEY (wager_id, phase)
) STRICT;

CREATE TABLE tarot_wager_notice (
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    purpose TEXT NOT NULL CHECK (purpose IN ('sealed_offer', 'accepted', 'reminder', 'disputed', 'resolved')),
    target_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    notice_id TEXT NOT NULL UNIQUE REFERENCES pending_notice(notice_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY (wager_id, purpose, target_user_id)
) STRICT;

CREATE TABLE tarot_wager_public_card (
    wager_id TEXT PRIMARY KEY REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    create_outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_revision INTEGER NOT NULL CHECK (created_revision >= 1)
) STRICT;

CREATE TABLE tarot_wager_card_revision (
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    wager_revision INTEGER NOT NULL CHECK (wager_revision >= 1),
    outbox_id TEXT NOT NULL UNIQUE REFERENCES outbox_message(outbox_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY (wager_id, wager_revision)
) STRICT;

CREATE TABLE tarot_wager_history_cursor (
    cursor_id TEXT PRIMARY KEY
        CHECK (length(cursor_id) = 36
               AND substr(cursor_id, 9, 1) = '-'
               AND substr(cursor_id, 14, 1) = '-'
               AND substr(cursor_id, 19, 1) = '-'
               AND substr(cursor_id, 24, 1) = '-'
               AND length(replace(cursor_id, '-', '')) = 32
               AND cursor_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(cursor_id, 15, 1) = '4'
               AND substr(cursor_id, 20, 1) IN ('8', '9', 'a', 'b')),
    user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    item_count INTEGER NOT NULL CHECK (item_count BETWEEN 0 AND 50),
    next_cursor_id TEXT REFERENCES tarot_wager_history_cursor(cursor_id) ON UPDATE RESTRICT ON DELETE SET NULL,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms)
) STRICT;
CREATE TABLE tarot_wager_history_item (
    cursor_id TEXT NOT NULL REFERENCES tarot_wager_history_cursor(cursor_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 49),
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY (cursor_id, position)
) STRICT;

CREATE TABLE tarot_wager_test_cleanup (
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    original_transaction_id TEXT NOT NULL REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reversal_transaction_id TEXT NOT NULL UNIQUE REFERENCES tarot_transaction(transaction_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    actor_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    reason TEXT NOT NULL CHECK (length(CAST(reason AS BLOB)) BETWEEN 1 AND 200),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    PRIMARY KEY (wager_id, original_transaction_id)
) STRICT;

CREATE TABLE tarot_wager_test_role (
    wager_id TEXT NOT NULL REFERENCES tarot_wager(wager_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    owner_user_id TEXT NOT NULL REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    role TEXT NOT NULL CHECK (role IN ('creator', 'target', 'judge', 'owner')),
    revision INTEGER NOT NULL CHECK (revision >= 1),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0),
    PRIMARY KEY (wager_id, owner_user_id)
) STRICT;

CREATE TRIGGER tarot_wager_test_role_guard
BEFORE INSERT ON tarot_wager_test_role
WHEN (SELECT is_test FROM tarot_wager WHERE wager_id = NEW.wager_id) <> 1
BEGIN SELECT RAISE(ABORT, 'test role requires a test wager'); END;

CREATE TRIGGER tarot_transaction_insert_prepared
BEFORE INSERT ON tarot_transaction WHEN NEW.state <> 'prepared'
BEGIN SELECT RAISE(ABORT, 'tarot transactions must begin prepared'); END;

CREATE TRIGGER tarot_posting_insert_only_prepared
BEFORE INSERT ON tarot_posting
BEGIN
    SELECT CASE WHEN (SELECT state FROM tarot_transaction WHERE transaction_id = NEW.transaction_id) <> 'prepared'
      THEN RAISE(ABORT, 'tarot postings require a prepared transaction') END;
    SELECT CASE WHEN (SELECT count(*) FROM tarot_posting WHERE transaction_id = NEW.transaction_id) >=
                     (SELECT expected_posting_count FROM tarot_transaction WHERE transaction_id = NEW.transaction_id)
      THEN RAISE(ABORT, 'tarot posting count exceeded') END;
END;

CREATE TRIGGER tarot_posting_no_update BEFORE UPDATE ON tarot_posting
BEGIN SELECT RAISE(ABORT, 'tarot postings are immutable'); END;
CREATE TRIGGER tarot_posting_no_delete BEFORE DELETE ON tarot_posting
BEGIN SELECT RAISE(ABORT, 'tarot postings are immutable'); END;

CREATE TRIGGER tarot_transaction_seal
BEFORE UPDATE ON tarot_transaction
WHEN OLD.state = 'prepared' AND NEW.state = 'committed'
BEGIN
    SELECT CASE WHEN NEW.transaction_id <> OLD.transaction_id
                      OR NEW.ledger_sequence <> OLD.ledger_sequence
                      OR NEW.transaction_type <> OLD.transaction_type
                      OR NEW.expected_posting_count <> OLD.expected_posting_count
                      OR NEW.event_id <> OLD.event_id
                      OR NEW.idempotency_key <> OLD.idempotency_key
                      OR NEW.actor_user_id IS NOT OLD.actor_user_id
                      OR NEW.reason IS NOT OLD.reason OR NEW.is_test <> OLD.is_test
                      OR NEW.reversal_of_transaction_id IS NOT OLD.reversal_of_transaction_id
                      OR NEW.created_at_ms <> OLD.created_at_ms
      THEN RAISE(ABORT, 'tarot transaction metadata is immutable') END;
    SELECT CASE WHEN (SELECT count(*) FROM tarot_posting WHERE transaction_id = OLD.transaction_id) <> OLD.expected_posting_count
      THEN RAISE(ABORT, 'tarot posting count mismatch') END;
    SELECT CASE WHEN (SELECT total(amount) FROM tarot_posting WHERE transaction_id = OLD.transaction_id) <> 0.0
      THEN RAISE(ABORT, 'tarot transaction is unbalanced') END;
    SELECT CASE WHEN OLD.transaction_type NOT LIKE 'WAGER_%'
                          AND OLD.transaction_type <> 'TEST_REVERSAL'
                          AND OLD.expected_posting_count <> 2
      THEN RAISE(ABORT, 'tarot transaction shape is invalid') END;
    SELECT CASE WHEN OLD.transaction_type = 'WAGER_ESCROW_FUND' AND OLD.expected_posting_count NOT IN (2, 3)
      THEN RAISE(ABORT, 'wager funding posting count is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('TEST_ADJUSTMENT', 'TEST_REVERSAL')
      AND (OLD.actor_user_id IS NULL
           OR OLD.reason IS NULL
           OR length(trim(OLD.reason, char(9) || char(10) || char(11) ||
                                      char(12) || char(13) || ' ')) = 0
           OR NOT EXISTS (
             SELECT 1
             FROM event_journal event
             JOIN tarot_posting human_post
               ON human_post.transaction_id = OLD.transaction_id
             JOIN tarot_account human_account
               ON human_account.account_id = human_post.account_id
             WHERE event.event_id = OLD.event_id
               AND event.event_type = CASE OLD.transaction_type
                     WHEN 'TEST_ADJUSTMENT' THEN 'tarot.admin_adjusted.v1'
                     ELSE 'tarot.transaction_reversed.v1' END
               AND event.aggregate_type = 'tarot_transaction'
               AND event.aggregate_id = OLD.transaction_id
               AND event.actor_user_id = OLD.actor_user_id
               AND human_account.account_kind = 'HUMAN'
               AND human_account.user_id = OLD.actor_user_id))
      THEN RAISE(ABORT, 'tarot test transaction audit provenance is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('STARTING_GRANT', 'GRACE', 'TRIAL')
      AND (OLD.expected_posting_count <> 2 OR NOT EXISTS (
        SELECT 1 FROM tarot_posting human_post
        JOIN tarot_account human_account ON human_account.account_id = human_post.account_id
        JOIN tarot_posting mint_post ON mint_post.transaction_id = human_post.transaction_id AND mint_post.account_id <> human_post.account_id
        JOIN tarot_account mint_account ON mint_account.account_id = mint_post.account_id
        WHERE human_post.transaction_id = OLD.transaction_id
          AND human_account.account_kind = 'HUMAN' AND mint_account.account_kind = 'MINT'
          AND human_post.amount > 0 AND mint_post.amount = -human_post.amount))
      THEN RAISE(ABORT, 'tarot grant shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type = 'STARTING_GRANT' AND EXISTS (
      SELECT 1 FROM tarot_posting candidate
      JOIN tarot_account account ON account.account_id = candidate.account_id
      JOIN tarot_posting prior ON prior.account_id = candidate.account_id AND prior.transaction_id <> candidate.transaction_id
      JOIN tarot_transaction prior_tx ON prior_tx.transaction_id = prior.transaction_id
      WHERE candidate.transaction_id = OLD.transaction_id AND account.account_kind = 'HUMAN'
        AND prior_tx.transaction_type = 'STARTING_GRANT' AND prior_tx.state = 'committed')
      THEN RAISE(ABORT, 'tarot starting grant already exists') END;

    SELECT CASE WHEN OLD.transaction_type = 'TEST_ADJUSTMENT' AND NOT EXISTS (
      SELECT 1 FROM tarot_posting human_post
      JOIN tarot_account human_account ON human_account.account_id = human_post.account_id
      JOIN tarot_posting system_post ON system_post.transaction_id = human_post.transaction_id AND system_post.account_id <> human_post.account_id
      JOIN tarot_account system_account ON system_account.account_id = system_post.account_id
      WHERE human_post.transaction_id = OLD.transaction_id AND human_account.account_kind = 'HUMAN'
        AND ((human_post.amount > 0 AND system_account.account_kind = 'MINT')
          OR (human_post.amount < 0 AND system_account.account_kind = 'BURN'))
        AND system_post.amount = -human_post.amount)
      THEN RAISE(ABORT, 'tarot adjustment shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type = 'TEST_REVERSAL'
      AND ((SELECT state FROM tarot_transaction WHERE transaction_id = OLD.reversal_of_transaction_id) <> 'committed'
        OR (SELECT is_test FROM tarot_transaction WHERE transaction_id = OLD.reversal_of_transaction_id) <> 1
        OR (SELECT transaction_type FROM tarot_transaction WHERE transaction_id = OLD.reversal_of_transaction_id) IN ('STARTING_GRANT', 'TEST_REVERSAL')
        OR ((SELECT transaction_type FROM tarot_transaction WHERE transaction_id = OLD.reversal_of_transaction_id) <> 'TEST_ADJUSTMENT'
            AND NOT EXISTS (SELECT 1 FROM tarot_wager_test_cleanup cleanup
                            WHERE cleanup.reversal_transaction_id = OLD.transaction_id
                              AND cleanup.original_transaction_id = OLD.reversal_of_transaction_id))
        OR OLD.expected_posting_count <> (SELECT expected_posting_count FROM tarot_transaction WHERE transaction_id = OLD.reversal_of_transaction_id)
        OR (SELECT count(*) FROM tarot_posting original
            JOIN tarot_posting inverse ON inverse.transaction_id = OLD.transaction_id
             AND inverse.account_id = original.account_id AND inverse.amount = -original.amount
            WHERE original.transaction_id = OLD.reversal_of_transaction_id) <> OLD.expected_posting_count)
      THEN RAISE(ABORT, 'tarot reversal shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type LIKE 'WAGER_%' AND NOT EXISTS (
      SELECT 1 FROM tarot_wager_transfer transfer
      JOIN tarot_wager wager ON wager.wager_id = transfer.wager_id
      JOIN event_journal event ON event.event_id = OLD.event_id
      WHERE transfer.transaction_id = OLD.transaction_id
        AND transfer.transfer_kind = CASE OLD.transaction_type
              WHEN 'WAGER_ESCROW_FUND' THEN 'fund'
              WHEN 'WAGER_PAYOUT' THEN 'payout' ELSE 'refund' END
        AND wager.is_test = OLD.is_test
        AND event.aggregate_type = 'tarot_wager' AND event.aggregate_id = wager.wager_id
        AND event.event_type = CASE OLD.transaction_type
              WHEN 'WAGER_ESCROW_FUND' THEN 'tarot.wager_funded.v1'
              WHEN 'WAGER_PAYOUT' THEN 'tarot.wager_resolved.v1'
              ELSE 'tarot.wager_voided.v1' END)
      THEN RAISE(ABORT, 'wager transfer audit linkage is invalid') END;

    SELECT CASE WHEN OLD.transaction_type = 'WAGER_ESCROW_FUND' AND NOT EXISTS (
      SELECT 1 FROM tarot_wager_transfer transfer
      JOIN tarot_wager wager ON wager.wager_id = transfer.wager_id
      JOIN tarot_posting escrow_post ON escrow_post.transaction_id = OLD.transaction_id
      JOIN tarot_account escrow_account ON escrow_account.account_id = escrow_post.account_id
      WHERE transfer.transaction_id = OLD.transaction_id AND escrow_account.account_kind = 'ESCROW'
        AND escrow_post.amount = 2 * wager.stake
        AND ((wager.is_test = 0 AND OLD.expected_posting_count = 3
              AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id = p.account_id
                   WHERE p.transaction_id = OLD.transaction_id AND a.account_kind = 'HUMAN'
                     AND a.user_id IN (wager.creator_user_id, wager.target_user_id)
                     AND p.amount = -wager.stake) = 2)
          OR (wager.is_test = 1 AND wager.creator_user_id = wager.target_user_id
              AND OLD.expected_posting_count = 2
              AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id = p.account_id
                   WHERE p.transaction_id = OLD.transaction_id AND a.account_kind = 'HUMAN'
                     AND a.user_id = wager.creator_user_id
                     AND p.amount = -2 * wager.stake) = 1)))
      THEN RAISE(ABORT, 'wager funding shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type IN ('WAGER_PAYOUT', 'WAGER_REFUND') AND NOT EXISTS (
      SELECT 1 FROM tarot_wager_transfer transfer
      JOIN tarot_wager wager ON wager.wager_id = transfer.wager_id
      JOIN tarot_posting escrow_post ON escrow_post.transaction_id = OLD.transaction_id
      JOIN tarot_account escrow_account ON escrow_account.account_id = escrow_post.account_id
      WHERE transfer.transaction_id = OLD.transaction_id AND escrow_account.account_kind = 'ESCROW'
        AND escrow_post.amount = -2 * wager.stake
        AND ((OLD.transaction_type = 'WAGER_PAYOUT' AND OLD.expected_posting_count = 2
              AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id = p.account_id
                   WHERE p.transaction_id = OLD.transaction_id AND a.account_kind = 'HUMAN'
                     AND a.user_id IN (wager.creator_user_id, wager.target_user_id)
                     AND p.amount = 2 * wager.stake) = 1)
          OR (OLD.transaction_type = 'WAGER_REFUND'
              AND ((wager.is_test = 0 AND OLD.expected_posting_count = 3
                    AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id = p.account_id
                         WHERE p.transaction_id = OLD.transaction_id AND a.account_kind = 'HUMAN'
                           AND a.user_id IN (wager.creator_user_id, wager.target_user_id)
                           AND p.amount = wager.stake) = 2)
                OR (wager.is_test = 1 AND wager.creator_user_id = wager.target_user_id AND OLD.expected_posting_count = 2
                    AND (SELECT count(*) FROM tarot_posting p JOIN tarot_account a ON a.account_id = p.account_id
                         WHERE p.transaction_id = OLD.transaction_id AND a.account_kind = 'HUMAN'
                           AND a.user_id = wager.creator_user_id
                           AND p.amount = 2 * wager.stake) = 1)))))
      THEN RAISE(ABORT, 'wager settlement shape is invalid') END;

    SELECT CASE WHEN EXISTS (
      SELECT 1
      FROM tarot_posting pending
      WHERE pending.transaction_id = OLD.transaction_id
        AND ((pending.amount > 0 AND
              COALESCE((SELECT sum(prior.amount)
                        FROM tarot_posting prior
                        JOIN tarot_transaction prior_tx
                          ON prior_tx.transaction_id = prior.transaction_id
                        WHERE prior.account_id = pending.account_id
                          AND prior_tx.state = 'committed'), 0)
                > 9223372036854775807 - pending.amount)
             OR
             (pending.amount < 0 AND
              COALESCE((SELECT sum(prior.amount)
                        FROM tarot_posting prior
                        JOIN tarot_transaction prior_tx
                          ON prior_tx.transaction_id = prior.transaction_id
                        WHERE prior.account_id = pending.account_id
                          AND prior_tx.state = 'committed'), 0)
                < -9223372036854775808 - pending.amount)))
      THEN RAISE(ABORT, 'tarot account balance overflow') END;

    SELECT CASE WHEN EXISTS (
      SELECT 1 FROM tarot_posting pending JOIN tarot_account account ON account.account_id = pending.account_id
      WHERE pending.transaction_id = OLD.transaction_id AND account.account_kind = 'HUMAN'
        AND COALESCE((SELECT sum(prior.amount) FROM tarot_posting prior
                      JOIN tarot_transaction tx ON tx.transaction_id = prior.transaction_id
                      WHERE prior.account_id = pending.account_id AND tx.state = 'committed'), 0) + pending.amount < 0)
      THEN RAISE(ABORT, 'tarot human balance cannot be negative') END;
END;

CREATE TRIGGER tarot_transaction_update_guard BEFORE UPDATE ON tarot_transaction
WHEN NOT (OLD.state = 'prepared' AND NEW.state = 'committed')
BEGIN SELECT RAISE(ABORT, 'tarot transactions are immutable after creation'); END;
CREATE TRIGGER tarot_transaction_no_delete BEFORE DELETE ON tarot_transaction
BEGIN SELECT RAISE(ABORT, 'tarot transactions are immutable'); END;

CREATE TRIGGER tarot_recovery_insert_pending
BEFORE INSERT ON tarot_recovery_claim
WHEN NEW.state <> 'pending'
BEGIN
    SELECT RAISE(ABORT, 'tarot recovery claims must begin pending');
END;

CREATE TRIGGER tarot_recovery_transaction_seal_link
BEFORE UPDATE ON tarot_transaction
WHEN OLD.state = 'prepared' AND NEW.state = 'committed' AND OLD.transaction_type IN ('GRACE', 'TRIAL')
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM event_journal terminal
      JOIN tarot_recovery_claim claim ON claim.claim_id = terminal.aggregate_id
      JOIN tarot_posting human_post ON human_post.transaction_id = OLD.transaction_id AND human_post.account_id = claim.account_id
      JOIN tarot_account human_account ON human_account.account_id = human_post.account_id
      WHERE terminal.event_id = OLD.event_id AND terminal.aggregate_type = 'tarot_recovery_claim'
        AND terminal.event_type = CASE OLD.transaction_type WHEN 'GRACE' THEN 'tarot.grace_completed.v1' ELSE 'tarot.trial_completed.v1' END
        AND claim.state = 'pending' AND claim.claim_type = OLD.transaction_type AND claim.is_test = OLD.is_test
        AND human_account.account_kind = 'HUMAN' AND human_post.amount > 0
        AND (OLD.transaction_type = 'GRACE' OR claim.reward = human_post.amount))
      THEN RAISE(ABORT, 'tarot recovery transaction is not linked to its claim') END;
END;

CREATE TRIGGER tarot_recovery_transition_guard
BEFORE UPDATE ON tarot_recovery_claim WHEN NOT (OLD.state = 'pending' AND NEW.state = 'pending')
BEGIN
    SELECT CASE WHEN OLD.state <> 'pending' THEN RAISE(ABORT, 'terminal tarot claim is immutable') END;
    SELECT CASE WHEN NEW.state NOT IN ('completed', 'expired', 'abandoned') THEN RAISE(ABORT, 'invalid tarot claim transition') END;
    SELECT CASE WHEN NEW.claim_id <> OLD.claim_id OR NEW.account_id <> OLD.account_id
      OR NEW.claim_type <> OLD.claim_type OR NEW.visibility <> OLD.visibility OR NEW.is_test <> OLD.is_test
      OR NEW.eligibility_threshold <> OLD.eligibility_threshold OR NEW.grace_target IS NOT OLD.grace_target
      OR NEW.eligibility_balance <> OLD.eligibility_balance
      OR (NEW.reward IS NOT OLD.reward AND NOT (OLD.claim_type = 'GRACE' AND OLD.reward IS NULL AND NEW.reward IS NOT NULL))
      OR NEW.draw_id IS NOT OLD.draw_id OR NEW.started_event_id <> OLD.started_event_id
      OR NEW.start_idempotency_key <> OLD.start_idempotency_key OR NEW.created_at_ms <> OLD.created_at_ms
      OR NEW.expires_at_ms <> OLD.expires_at_ms
      THEN RAISE(ABORT, 'tarot claim identity is immutable') END;
    SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM event_journal terminal WHERE terminal.event_id = NEW.event_id
      AND terminal.aggregate_type = 'tarot_recovery_claim' AND terminal.aggregate_id = NEW.claim_id
      AND ((NEW.state = 'completed' AND terminal.event_type = CASE NEW.claim_type WHEN 'GRACE' THEN 'tarot.grace_completed.v1' ELSE 'tarot.trial_completed.v1' END)
        OR (NEW.state = 'expired' AND terminal.event_type = 'tarot.recovery_expired.v1')
        OR (NEW.state = 'abandoned'
            AND terminal.event_type = 'tarot.recovery_eligibility_lost.v1')
        OR (NEW.state = 'abandoned' AND NEW.claim_type = 'TRIAL'
            AND terminal.event_type = 'tarot.trial_abandoned.v1')))
      THEN RAISE(ABORT, 'tarot claim terminal event is invalid') END;
    SELECT CASE WHEN NEW.state = 'completed' AND NOT EXISTS (
      SELECT 1 FROM tarot_transaction tx
      JOIN tarot_posting posting ON posting.transaction_id = tx.transaction_id
      JOIN tarot_account account ON account.account_id = posting.account_id
      WHERE tx.transaction_id = NEW.transaction_id AND tx.state = 'committed' AND tx.transaction_type = NEW.claim_type
        AND tx.event_id = NEW.event_id AND tx.is_test = NEW.is_test
        AND posting.account_id = NEW.account_id
        AND account.account_kind = 'HUMAN' AND posting.amount = NEW.reward)
      THEN RAISE(ABORT, 'tarot completed claim does not match its ledger reward') END;
    SELECT CASE WHEN NEW.state = 'completed' AND NEW.visibility = 'public' AND NOT EXISTS (
      SELECT 1 FROM outbox_message outbox JOIN event_journal started ON started.event_id = NEW.started_event_id
      WHERE outbox.outbox_id = NEW.outbox_id AND outbox.kind = 'discord.public.v1'
        AND outbox.aggregate_type = 'tarot_recovery_claim' AND outbox.aggregate_id = NEW.claim_id
        AND outbox.target_guild_id = started.guild_id AND outbox.target_channel_id = started.channel_id AND outbox.target_user_id IS NULL)
      THEN RAISE(ABORT, 'public tarot claim requires its scoped outbox row') END;
    SELECT CASE WHEN (NEW.state = 'completed' AND NEW.visibility = 'private' AND NEW.outbox_id IS NOT NULL)
      OR (NEW.state IN ('expired', 'abandoned') AND NEW.outbox_id IS NOT NULL)
      THEN RAISE(ABORT, 'private or unrewarded tarot claim cannot link outbox') END;
END;

CREATE TRIGGER tarot_recovery_cancel_terminal_tokens AFTER UPDATE ON tarot_recovery_claim
WHEN OLD.state = 'pending' AND NEW.state IN ('completed', 'expired', 'abandoned')
BEGIN UPDATE interaction_token SET state = 'cancelled' WHERE entity_type = 'tarot_recovery_claim' AND entity_id = NEW.claim_id AND state = 'active'; END;

CREATE TRIGGER tarot_recovery_privacy_tighten BEFORE UPDATE ON tarot_recovery_claim
WHEN OLD.state = 'pending' AND NEW.state = 'pending'
BEGIN
    SELECT CASE WHEN OLD.visibility <> 'public' OR NEW.visibility <> 'private'
      OR NEW.claim_id <> OLD.claim_id OR NEW.account_id <> OLD.account_id OR NEW.claim_type <> OLD.claim_type
      OR NEW.is_test <> OLD.is_test OR NEW.eligibility_threshold <> OLD.eligibility_threshold
      OR NEW.grace_target IS NOT OLD.grace_target OR NEW.eligibility_balance <> OLD.eligibility_balance
      OR NEW.reward IS NOT OLD.reward OR NEW.draw_id IS NOT OLD.draw_id OR NEW.transaction_id IS NOT OLD.transaction_id
      OR NEW.started_event_id <> OLD.started_event_id OR NEW.event_id IS NOT OLD.event_id OR NEW.outbox_id IS NOT OLD.outbox_id
      OR NEW.start_idempotency_key <> OLD.start_idempotency_key OR NEW.completion_idempotency_key IS NOT OLD.completion_idempotency_key
      OR NEW.created_at_ms <> OLD.created_at_ms OR NEW.expires_at_ms <> OLD.expires_at_ms
      OR NEW.completed_at_ms IS NOT OLD.completed_at_ms OR NEW.cooldown_until_ms IS NOT OLD.cooldown_until_ms
      THEN RAISE(ABORT, 'pending tarot claims only permit privacy tightening') END;
END;
CREATE TRIGGER tarot_recovery_no_delete BEFORE DELETE ON tarot_recovery_claim
BEGIN SELECT RAISE(ABORT, 'tarot recovery claims are retained'); END;
CREATE TRIGGER tarot_interaction_receipt_no_update BEFORE UPDATE ON tarot_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'tarot interaction receipts are immutable'); END;
CREATE TRIGGER tarot_interaction_receipt_no_delete BEFORE DELETE ON tarot_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'tarot interaction receipts are retained'); END;

CREATE TRIGGER tarot_wager_transition_guard BEFORE UPDATE ON tarot_wager
BEGIN
    SELECT CASE WHEN OLD.state IN ('resolved', 'void_refunded', 'cancelled', 'declined', 'expired')
      THEN RAISE(ABORT, 'terminal wager is immutable') END;
    SELECT CASE WHEN NOT (
      (OLD.state = 'draft' AND NEW.state IN ('draft', 'offered', 'cancelled', 'expired')) OR
      (OLD.state = 'offered' AND NEW.state IN ('accepted_funded', 'declined', 'cancelled', 'expired')) OR
      (OLD.state = 'accepted_funded' AND NEW.state IN ('awaiting_resolution', 'disputed', 'resolved', 'void_refunded')) OR
      (OLD.state = 'awaiting_resolution' AND NEW.state IN ('disputed', 'resolved', 'void_refunded')) OR
      (OLD.state = 'disputed' AND NEW.state IN ('resolved', 'void_refunded')))
      THEN RAISE(ABORT, 'invalid wager transition') END;
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager_action action
      WHERE action.wager_id = OLD.wager_id
        AND action.wager_revision = OLD.revision
        AND ((OLD.state = 'draft' AND NEW.state = 'draft' AND action.action = 'previewed')
          OR (OLD.state = 'draft' AND NEW.state = 'offered' AND action.action = 'confirmed')
          OR (OLD.state = 'draft' AND NEW.state = 'cancelled' AND action.action = 'discarded')
          OR (OLD.state = 'draft' AND NEW.state = 'expired' AND action.action = 'expired')
          OR (OLD.state = 'offered' AND NEW.state = 'accepted_funded' AND action.action = 'accepted')
          OR (OLD.state = 'offered' AND NEW.state = 'declined' AND action.action = 'declined')
          OR (OLD.state = 'offered' AND NEW.state = 'cancelled' AND action.action = 'cancelled')
          OR (OLD.state = 'offered' AND NEW.state = 'expired' AND action.action = 'expired')
          OR (OLD.state = 'accepted_funded' AND NEW.state = 'awaiting_resolution'
              AND action.action IN ('outcome_submitted', 'outcome_due'))
          OR (OLD.state IN ('accepted_funded', 'awaiting_resolution') AND NEW.state = 'disputed'
              AND action.action IN ('disputed', 'grace_elapsed'))
          OR (OLD.state IN ('accepted_funded', 'awaiting_resolution', 'disputed')
              AND NEW.state IN ('resolved', 'void_refunded')
              AND action.action IN ('agreed', 'judged')))
      ) THEN RAISE(ABORT, 'wager transition lacks authorized audit action') END;
    SELECT CASE WHEN NEW.wager_id <> OLD.wager_id OR NEW.guild_id <> OLD.guild_id OR NEW.channel_id <> OLD.channel_id
      OR NEW.creator_user_id <> OLD.creator_user_id OR NEW.target_user_id <> OLD.target_user_id
      OR NEW.judge_user_id IS NOT OLD.judge_user_id OR NEW.visibility <> OLD.visibility
      OR NEW.resolution_policy <> OLD.resolution_policy OR NEW.outcome_window_ms <> OLD.outcome_window_ms
      OR NEW.resolution_grace_ms <> OLD.resolution_grace_ms
      OR NEW.is_test <> OLD.is_test OR NEW.created_at_ms <> OLD.created_at_ms
      OR ((OLD.state <> 'draft' OR NEW.state <> 'draft')
          AND (NEW.proposition IS NOT OLD.proposition OR NEW.stake IS NOT OLD.stake
               OR NEW.evidence_instructions IS NOT OLD.evidence_instructions
               OR NEW.offer_duration_ms IS NOT OLD.offer_duration_ms))
      OR (OLD.state = 'draft' AND NEW.state = 'draft' AND OLD.proposition IS NOT NULL
          AND (NEW.proposition IS NOT OLD.proposition OR NEW.stake IS NOT OLD.stake
               OR NEW.evidence_instructions IS NOT OLD.evidence_instructions
               OR NEW.offer_duration_ms IS NOT OLD.offer_duration_ms))
      OR (OLD.state <> 'draft' AND NEW.offer_expires_at_ms IS NOT OLD.offer_expires_at_ms)
      OR (OLD.state NOT IN ('draft','offered')
          AND (NEW.fund_transaction_id IS NOT OLD.fund_transaction_id
               OR NEW.outcome_due_at_ms IS NOT OLD.outcome_due_at_ms
               OR NEW.resolution_grace_until_ms IS NOT OLD.resolution_grace_until_ms))
      OR NEW.revision <> OLD.revision + 1 OR NEW.updated_at_ms < OLD.updated_at_ms
      THEN RAISE(ABORT, 'wager identity, terms, or revision is invalid') END;
    SELECT CASE WHEN OLD.state = 'offered' AND NEW.state = 'accepted_funded'
      AND (NEW.outcome_due_at_ms <> NEW.updated_at_ms + NEW.outcome_window_ms
           OR NEW.resolution_grace_until_ms <> NEW.outcome_due_at_ms + NEW.resolution_grace_ms)
      THEN RAISE(ABORT, 'funded wager deadlines do not match confirmed terms') END;
    SELECT CASE WHEN NEW.state = 'accepted_funded' AND NOT EXISTS (
      SELECT 1 FROM tarot_transaction tx JOIN tarot_wager_transfer tr ON tr.transaction_id = tx.transaction_id
      WHERE tx.transaction_id = NEW.fund_transaction_id AND tx.state = 'committed'
        AND tx.transaction_type = 'WAGER_ESCROW_FUND' AND tr.wager_id = NEW.wager_id AND tr.transfer_kind = 'fund')
      THEN RAISE(ABORT, 'funded wager lacks committed escrow transfer') END;
    SELECT CASE WHEN NEW.state IN ('resolved', 'void_refunded') AND NOT EXISTS (
      SELECT 1 FROM tarot_wager_resolution resolution JOIN tarot_transaction tx ON tx.transaction_id = resolution.transaction_id
      WHERE resolution.wager_id = NEW.wager_id AND resolution.transaction_id = NEW.settlement_transaction_id
        AND resolution.result = CASE NEW.state WHEN 'resolved' THEN NEW.winner_role ELSE 'void' END
        AND NEW.terminal_reason IS resolution.reason
        AND NEW.judged_by_user_id IS resolution.actor_user_id
        AND tx.state = 'committed' AND tx.transaction_type = CASE NEW.state WHEN 'resolved' THEN 'WAGER_PAYOUT' ELSE 'WAGER_REFUND' END
        AND (NEW.state <> 'resolved' OR EXISTS (
          SELECT 1 FROM tarot_posting payout
          JOIN tarot_account recipient ON recipient.account_id = payout.account_id
          WHERE payout.transaction_id = tx.transaction_id
            AND recipient.account_kind = 'HUMAN'
            AND recipient.user_id = CASE NEW.winner_role
                  WHEN 'creator' THEN NEW.creator_user_id ELSE NEW.target_user_id END
            AND payout.amount = 2 * NEW.stake)))
      THEN RAISE(ABORT, 'terminal wager lacks matching resolution transfer') END;
END;

CREATE TRIGGER tarot_wager_no_delete BEFORE DELETE ON tarot_wager
BEGIN SELECT RAISE(ABORT, 'wagers are retained'); END;
CREATE TRIGGER tarot_wager_cancel_terminal_controls AFTER UPDATE ON tarot_wager
WHEN NEW.state IN ('resolved', 'void_refunded', 'cancelled', 'declined', 'expired')
BEGIN
    UPDATE tarot_wager_control SET state = 'cancelled'
      WHERE wager_id = NEW.wager_id AND state = 'active';
    UPDATE interaction_token SET state = 'cancelled'
      WHERE entity_type = 'pending_notice' AND state = 'active'
        AND entity_id IN (
          SELECT notice_id FROM tarot_wager_notice
          WHERE wager_id = NEW.wager_id AND purpose = 'sealed_offer');
    UPDATE pending_notice SET state = 'cancelled'
      WHERE state IN ('pending', 'opened') AND notice_id IN (
        SELECT notice_id FROM tarot_wager_notice
        WHERE wager_id = NEW.wager_id AND purpose = 'sealed_offer');
END;

CREATE TRIGGER tarot_wager_action_immutable BEFORE UPDATE ON tarot_wager_action BEGIN SELECT RAISE(ABORT, 'wager actions are immutable'); END;
CREATE TRIGGER tarot_wager_action_retained BEFORE DELETE ON tarot_wager_action BEGIN SELECT RAISE(ABORT, 'wager actions are retained'); END;
CREATE TRIGGER tarot_wager_outcome_immutable BEFORE UPDATE ON tarot_wager_outcome BEGIN SELECT RAISE(ABORT, 'wager outcomes are immutable'); END;
CREATE TRIGGER tarot_wager_outcome_retained BEFORE DELETE ON tarot_wager_outcome BEGIN SELECT RAISE(ABORT, 'wager outcomes are retained'); END;
CREATE TRIGGER tarot_wager_void_immutable BEFORE UPDATE ON tarot_wager_void_consent BEGIN SELECT RAISE(ABORT, 'wager void consent is immutable'); END;
CREATE TRIGGER tarot_wager_void_retained BEFORE DELETE ON tarot_wager_void_consent BEGIN SELECT RAISE(ABORT, 'wager void consent is retained'); END;
CREATE TRIGGER tarot_wager_evidence_immutable BEFORE UPDATE ON tarot_wager_evidence BEGIN SELECT RAISE(ABORT, 'wager evidence is immutable'); END;
CREATE TRIGGER tarot_wager_evidence_retained BEFORE DELETE ON tarot_wager_evidence BEGIN SELECT RAISE(ABORT, 'wager evidence is retained'); END;
CREATE TRIGGER tarot_wager_resolution_immutable BEFORE UPDATE ON tarot_wager_resolution BEGIN SELECT RAISE(ABORT, 'wager resolutions are immutable'); END;
CREATE TRIGGER tarot_wager_resolution_retained BEFORE DELETE ON tarot_wager_resolution BEGIN SELECT RAISE(ABORT, 'wager resolutions are retained'); END;
CREATE TRIGGER tarot_wager_transfer_guard
BEFORE INSERT ON tarot_wager_transfer
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN tarot_transaction tx ON tx.transaction_id = NEW.transaction_id
      JOIN event_journal event ON event.event_id = tx.event_id
      WHERE wager.wager_id = NEW.wager_id
        AND tx.state = 'prepared'
        AND tx.is_test = wager.is_test
        AND event.aggregate_type = 'tarot_wager'
        AND event.aggregate_id = wager.wager_id
        AND ((NEW.transfer_kind = 'fund'
              AND wager.state = 'offered'
              AND tx.transaction_type = 'WAGER_ESCROW_FUND'
              AND event.event_type = 'tarot.wager_funded.v1')
          OR (NEW.transfer_kind = 'payout'
              AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
              AND tx.transaction_type = 'WAGER_PAYOUT'
              AND event.event_type = 'tarot.wager_resolved.v1')
          OR (NEW.transfer_kind = 'refund'
              AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
              AND tx.transaction_type = 'WAGER_REFUND'
              AND event.event_type = 'tarot.wager_voided.v1')))
      THEN RAISE(ABORT, 'wager transfer linkage is invalid') END;
END;
CREATE TRIGGER tarot_wager_transfer_immutable BEFORE UPDATE ON tarot_wager_transfer BEGIN SELECT RAISE(ABORT, 'wager transfers are immutable'); END;
CREATE TRIGGER tarot_wager_transfer_retained BEFORE DELETE ON tarot_wager_transfer BEGIN SELECT RAISE(ABORT, 'wager transfers are retained'); END;
CREATE TRIGGER tarot_wager_receipt_immutable BEFORE UPDATE ON tarot_wager_receipt BEGIN SELECT RAISE(ABORT, 'wager receipts are immutable'); END;
CREATE TRIGGER tarot_wager_receipt_retained BEFORE DELETE ON tarot_wager_receipt BEGIN SELECT RAISE(ABORT, 'wager receipts are retained'); END;
CREATE TRIGGER tarot_wager_test_cleanup_guard
BEFORE INSERT ON tarot_wager_test_cleanup
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN guild_config guild ON guild.guild_id = wager.guild_id
      JOIN tarot_wager_transfer transfer
        ON transfer.wager_id = wager.wager_id
       AND transfer.transaction_id = NEW.original_transaction_id
       AND transfer.transfer_kind IN ('fund', 'payout', 'refund')
      JOIN tarot_transaction original
        ON original.transaction_id = transfer.transaction_id
      JOIN tarot_transaction reversal
        ON reversal.transaction_id = NEW.reversal_transaction_id
      WHERE wager.wager_id = NEW.wager_id
        AND wager.is_test = 1
        AND wager.state IN ('resolved', 'void_refunded', 'cancelled', 'declined', 'expired')
        AND guild.owner_user_id = NEW.actor_user_id
        AND original.state = 'committed' AND original.is_test = 1
        AND original.transaction_type IN ('WAGER_ESCROW_FUND', 'WAGER_PAYOUT', 'WAGER_REFUND')
        AND reversal.state = 'prepared' AND reversal.transaction_type = 'TEST_REVERSAL'
        AND reversal.reversal_of_transaction_id = original.transaction_id
        AND reversal.expected_posting_count = original.expected_posting_count
        AND reversal.is_test = 1 AND reversal.actor_user_id = NEW.actor_user_id
        AND reversal.reason = NEW.reason)
      THEN RAISE(ABORT, 'wager cleanup reversal linkage is invalid') END;
END;
CREATE TRIGGER tarot_wager_test_cleanup_immutable BEFORE UPDATE ON tarot_wager_test_cleanup BEGIN SELECT RAISE(ABORT, 'wager cleanup audit is immutable'); END;
CREATE TRIGGER tarot_wager_test_cleanup_retained BEFORE DELETE ON tarot_wager_test_cleanup BEGIN SELECT RAISE(ABORT, 'wager cleanup audit is retained'); END;

CREATE TRIGGER tarot_wager_action_authorization
BEFORE INSERT ON tarot_wager_action
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      WHERE wager.wager_id = NEW.wager_id
        AND wager.revision = NEW.wager_revision
        AND ((NEW.actor_role = 'creator' AND NEW.actor_user_id = wager.creator_user_id)
          OR (NEW.actor_role = 'target' AND NEW.actor_user_id = wager.target_user_id)
          OR (NEW.actor_role = 'judge' AND NEW.actor_user_id = wager.judge_user_id
              AND wager.resolution_policy = 'designated'
              AND wager.state IN ('accepted_funded','awaiting_resolution')
              AND NEW.action = 'judged')
          OR (NEW.actor_role = 'owner' AND NEW.actor_user_id =
              (SELECT owner_user_id FROM guild_config WHERE guild_id = wager.guild_id))
          OR (NEW.actor_role = 'scheduler'
              AND NEW.actor_user_id = wager.creator_user_id
              AND NEW.action IN ('expired','reminded','outcome_due','grace_elapsed'))))
      THEN RAISE(ABORT, 'wager action actor is unauthorized') END;
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN guild_config guild ON guild.guild_id = wager.guild_id
      WHERE wager.wager_id = NEW.wager_id
        AND ((NEW.action IN ('drafted','previewed','confirmed','discarded')
              AND wager.state = 'draft' AND NEW.actor_role = 'creator')
          OR (NEW.action IN ('accepted','declined')
              AND wager.state = 'offered' AND NEW.actor_role = 'target')
          OR (NEW.action = 'cancelled'
              AND wager.state = 'offered' AND NEW.actor_role = 'creator')
          OR (NEW.action = 'expired' AND wager.state IN ('draft','offered')
              AND NEW.actor_role IN ('creator','target','scheduler'))
          OR (NEW.action = 'outcome_submitted'
              AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
              AND NEW.actor_role IN ('creator','target'))
          OR (NEW.action = 'disputed'
              AND wager.state IN ('accepted_funded','awaiting_resolution')
              AND NEW.actor_role IN ('creator','target'))
          OR (NEW.action IN ('agreed','void_consented','evidence_added')
              AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
              AND NEW.actor_role IN ('creator','target'))
          OR (NEW.action = 'judged'
              AND ((wager.state IN ('accepted_funded','awaiting_resolution')
                    AND wager.resolution_policy = 'designated'
                    AND NEW.actor_role = 'judge')
                OR (wager.state = 'disputed' AND NEW.actor_role = 'owner'
                    AND NEW.actor_user_id = guild.owner_user_id)))
          OR (NEW.action = 'reminded'
              AND wager.state IN ('accepted_funded','awaiting_resolution')
              AND NEW.actor_role = 'scheduler')
          OR (NEW.action = 'outcome_due'
              AND wager.state = 'accepted_funded'
              AND NEW.actor_role = 'scheduler')
          OR (NEW.action = 'grace_elapsed'
              AND wager.state IN ('accepted_funded','awaiting_resolution')
              AND NEW.actor_role = 'scheduler')
          OR (NEW.action = 'test_role_set' AND wager.is_test = 1
              AND NEW.actor_role = 'owner'
              AND NEW.actor_user_id = guild.owner_user_id)
          OR (NEW.action = 'test_cleaned' AND wager.is_test = 1
              AND wager.state IN ('resolved','void_refunded','cancelled','declined','expired')
              AND NEW.actor_role = 'owner'
              AND NEW.actor_user_id = guild.owner_user_id)))
      THEN RAISE(ABORT, 'wager action is invalid for current state or role') END;
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM event_journal event
      WHERE event.event_id = NEW.event_id
        AND event.aggregate_type = 'tarot_wager'
        AND event.aggregate_id = NEW.wager_id
        AND event.actor_user_id = NEW.actor_user_id
        AND event.event_type IN (
          CASE NEW.action
            WHEN 'drafted' THEN 'tarot.wager_drafted.v1'
            WHEN 'previewed' THEN 'tarot.wager_previewed.v1'
            WHEN 'confirmed' THEN 'tarot.wager_offered.v1'
            WHEN 'discarded' THEN 'tarot.wager_cancelled.v1'
            WHEN 'accepted' THEN 'tarot.wager_funded.v1'
            WHEN 'declined' THEN 'tarot.wager_declined.v1'
            WHEN 'cancelled' THEN 'tarot.wager_cancelled.v1'
            WHEN 'expired' THEN 'tarot.wager_expired.v1'
            WHEN 'outcome_submitted' THEN 'tarot.wager_outcome_submitted.v1'
            WHEN 'disputed' THEN 'tarot.wager_disputed.v1'
            WHEN 'agreed' THEN 'tarot.wager_resolved.v1'
            WHEN 'void_consented' THEN 'tarot.wager_void_consent.v1'
            WHEN 'judged' THEN 'tarot.wager_resolved.v1'
            WHEN 'reminded' THEN 'tarot.wager_reminded.v1'
            WHEN 'outcome_due' THEN 'tarot.wager_outcome_due.v1'
            WHEN 'grace_elapsed' THEN 'tarot.wager_disputed.v1'
            WHEN 'evidence_added' THEN 'tarot.wager_evidence_added.v1'
            WHEN 'test_role_set' THEN 'tarot.wager_test_role_set.v1'
            WHEN 'test_cleaned' THEN 'tarot.wager_test_cleaned.v1'
          END,
          CASE WHEN NEW.action IN ('agreed','judged')
               THEN 'tarot.wager_voided.v1' END)
    ) THEN RAISE(ABORT, 'wager action event linkage is invalid') END;
END;

CREATE TRIGGER tarot_wager_resolution_guard
BEFORE INSERT ON tarot_wager_resolution
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN tarot_wager_transfer transfer
        ON transfer.wager_id = wager.wager_id
       AND transfer.transaction_id = NEW.transaction_id
      JOIN tarot_transaction tx ON tx.transaction_id = transfer.transaction_id
      JOIN event_journal event ON event.event_id = NEW.event_id
      WHERE wager.wager_id = NEW.wager_id
        AND tx.state = 'committed'
        AND tx.event_id = NEW.event_id
        AND event.aggregate_type = 'tarot_wager'
        AND event.aggregate_id = wager.wager_id
        AND ((NEW.result = 'void'
              AND transfer.transfer_kind = 'refund'
              AND tx.transaction_type = 'WAGER_REFUND'
              AND event.event_type = 'tarot.wager_voided.v1')
          OR (NEW.result IN ('creator','target')
              AND transfer.transfer_kind = 'payout'
              AND tx.transaction_type = 'WAGER_PAYOUT'
              AND event.event_type = 'tarot.wager_resolved.v1')))
      THEN RAISE(ABORT, 'wager resolution authority or transfer is invalid') END;
    SELECT CASE WHEN NEW.authority = 'judge' AND NOT EXISTS (
      SELECT 1 FROM tarot_wager wager WHERE wager.wager_id = NEW.wager_id
        AND wager.resolution_policy = 'designated'
        AND wager.state IN ('accepted_funded','awaiting_resolution')
        AND NEW.created_at_ms < wager.resolution_grace_until_ms
        AND NEW.actor_user_id = wager.judge_user_id)
      THEN RAISE(ABORT, 'designated wager judgment is unauthorized') END;
    SELECT CASE WHEN NEW.authority = 'owner' AND NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN guild_config guild ON guild.guild_id = wager.guild_id
      WHERE wager.wager_id = NEW.wager_id AND wager.state = 'disputed'
        AND NEW.actor_user_id = guild.owner_user_id)
      THEN RAISE(ABORT, 'owner wager judgment is unauthorized') END;
    SELECT CASE WHEN NEW.authority = 'mutual' AND NEW.result = 'void'
      AND (SELECT count(DISTINCT actor_role)
           FROM tarot_wager_void_consent consent
           WHERE consent.wager_id = NEW.wager_id) <> 2
      THEN RAISE(ABORT, 'mutual void lacks both participant consents') END;
    SELECT CASE WHEN NEW.authority = 'mutual'
      AND NEW.result IN ('creator','target') AND NOT EXISTS (
        SELECT 1 FROM tarot_wager_action agreement
        JOIN tarot_wager_outcome adopted
          ON adopted.wager_id = agreement.wager_id
         AND adopted.actor_role <> agreement.actor_role
         AND adopted.winner_role = NEW.result
        WHERE agreement.wager_id = NEW.wager_id
          AND agreement.action = 'agreed'
          AND agreement.actor_role IN ('creator','target'))
      THEN RAISE(ABORT, 'mutual result lacks participant agreement') END;
END;

CREATE TRIGGER tarot_wager_outcome_authorization
BEFORE INSERT ON tarot_wager_outcome
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager WHERE wager.wager_id = NEW.wager_id
        AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
        AND (wager.resolution_policy = 'mutual' OR wager.state = 'disputed')
        AND ((NEW.actor_role = 'creator' AND NEW.actor_user_id = wager.creator_user_id)
          OR (NEW.actor_role = 'target' AND NEW.actor_user_id = wager.target_user_id)))
      THEN RAISE(ABORT, 'wager outcome actor is unauthorized') END;
END;

CREATE TRIGGER tarot_wager_void_authorization
BEFORE INSERT ON tarot_wager_void_consent
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager WHERE wager.wager_id = NEW.wager_id
        AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
        AND ((NEW.actor_role = 'creator' AND NEW.actor_user_id = wager.creator_user_id)
          OR (NEW.actor_role = 'target' AND NEW.actor_user_id = wager.target_user_id)))
      THEN RAISE(ABORT, 'wager void actor is unauthorized') END;
END;

CREATE TRIGGER tarot_wager_evidence_authorization
BEFORE INSERT ON tarot_wager_evidence
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager WHERE wager.wager_id = NEW.wager_id
        AND wager.state IN ('accepted_funded','awaiting_resolution','disputed')
        AND ((NEW.actor_role = 'creator' AND NEW.actor_user_id = wager.creator_user_id)
          OR (NEW.actor_role = 'target' AND NEW.actor_user_id = wager.target_user_id)))
      THEN RAISE(ABORT, 'wager evidence actor is unauthorized') END;
END;

CREATE TRIGGER tarot_wager_job_guard
BEFORE INSERT ON tarot_wager_job
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN scheduled_job job ON job.job_id = NEW.job_id
      WHERE wager.wager_id = NEW.wager_id
        AND job.job_type = 'tarot.wager-deadline.v1'
        AND job.aggregate_type = 'tarot_wager'
        AND job.aggregate_id = wager.wager_id
        AND json_extract(job.payload_json, '$.wager_id') = wager.wager_id
        AND json_extract(job.payload_json, '$.phase') = NEW.phase
        AND json_extract(job.payload_json, '$.expected_revision') = NEW.expected_revision)
      THEN RAISE(ABORT, 'wager deadline linkage is invalid') END;
END;
CREATE TRIGGER tarot_wager_job_immutable BEFORE UPDATE ON tarot_wager_job
BEGIN SELECT RAISE(ABORT, 'wager deadline links are immutable'); END;
CREATE TRIGGER tarot_wager_job_retained BEFORE DELETE ON tarot_wager_job
BEGIN SELECT RAISE(ABORT, 'wager deadline links are retained'); END;

CREATE TRIGGER tarot_wager_notice_guard
BEFORE INSERT ON tarot_wager_notice
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN pending_notice notice ON notice.notice_id = NEW.notice_id
      WHERE wager.wager_id = NEW.wager_id
        AND notice.notice_type = 'tarot_wager'
        AND notice.source_aggregate_type = 'tarot_wager'
        AND notice.source_aggregate_id = wager.wager_id
        AND notice.target_user_id = NEW.target_user_id
        AND (NEW.purpose <> 'sealed_offer' OR wager.visibility = 'sealed'))
      THEN RAISE(ABORT, 'wager notice linkage is invalid') END;
END;

CREATE VIEW tarot_wager_public_projection AS
SELECT wager_id,
       'Status: ' || state || '.' ||
       CASE WHEN proposition IS NULL THEN ''
            ELSE char(10) || 'Proposition: ' || proposition END ||
       CASE WHEN stake IS NULL THEN ''
            ELSE char(10) || 'Stake: ' || stake || ' Fate each.' END ||
       char(10) || 'Creator: <@' || creator_user_id || '>' ||
       char(10) || 'Target: <@' || target_user_id || '>' ||
       CASE resolution_policy
         WHEN 'mutual' THEN char(10) || 'Resolution: mutual agreement.'
         ELSE char(10) || 'Resolution: designated judge <@' ||
              judge_user_id || '>.' END ||
       CASE WHEN offer_expires_at_ms IS NULL THEN ''
            ELSE char(10) || 'Offer deadline: <t:' ||
                 (offer_expires_at_ms / 1000) || ':F> (<t:' ||
                 (offer_expires_at_ms / 1000) || ':R>).' END ||
       CASE WHEN outcome_due_at_ms IS NOT NULL THEN ''
            ELSE char(10) || 'Outcome window: ' ||
                 (outcome_window_ms / 3600000) ||
                 ' hours after acceptance.' ||
                 char(10) || 'Owner escalation: ' ||
                 (resolution_grace_ms / 3600000) ||
                 ' hours after the outcome deadline.' END ||
       CASE WHEN outcome_due_at_ms IS NULL THEN ''
            ELSE char(10) || 'Outcome deadline: <t:' ||
                 (outcome_due_at_ms / 1000) || ':F> (<t:' ||
                 (outcome_due_at_ms / 1000) || ':R>).' END ||
       CASE WHEN resolution_grace_until_ms IS NULL THEN ''
            ELSE char(10) || 'Owner escalation after: <t:' ||
                 (resolution_grace_until_ms / 1000) || ':F> (<t:' ||
                 (resolution_grace_until_ms / 1000) || ':R>).' END ||
       CASE WHEN state <> 'resolved' THEN ''
            ELSE char(10) || 'Winner: <@' ||
                 CASE winner_role WHEN 'creator' THEN creator_user_id
                                  ELSE target_user_id END || '>' END ||
       CASE WHEN state <> 'void_refunded' THEN ''
            ELSE char(10) || 'Both equal stakes were refunded.' END
       AS expected_description
FROM tarot_wager
WHERE visibility = 'public';

CREATE TRIGGER tarot_wager_outbox_guard
BEFORE INSERT ON outbox_message
WHEN NEW.aggregate_type = 'tarot_wager'
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager WHERE wager.wager_id = NEW.aggregate_id
        AND NEW.kind IN ('discord.public.v1','discord.message_edit.v1')
        AND NEW.target_guild_id = wager.guild_id
        AND NEW.target_channel_id = wager.channel_id
        AND NEW.target_user_id IS NULL)
      THEN RAISE(ABORT, 'wager public delivery scope is invalid') END;
    SELECT CASE WHEN EXISTS (
      SELECT 1 FROM tarot_wager wager
      WHERE wager.wager_id = NEW.aggregate_id
        AND wager.visibility = 'sealed'
        AND COALESCE(
          (NEW.kind = 'discord.public.v1' AND (
            (wager.state = 'offered'
             AND json_extract(NEW.payload_json, '$.content') =
                 CASE wager.is_test WHEN 1 THEN '[TEST] ' ELSE '' END ||
                 '<@' || wager.target_user_id || '>, a sealed Fate wager awaits.'
             AND json_extract(NEW.payload_json, '$.embed.title') = 'A sealed wager awaits'
             AND json_extract(NEW.payload_json, '$.embed.url') = ''
             AND json_extract(NEW.payload_json, '$.embed.description') =
                 'Status: Offered. The terms remain sealed to the addressed participant.'
             AND json_array_length(json_extract(NEW.payload_json, '$.buttons')) = 1
             AND json_extract(NEW.payload_json, '$.buttons[0].label') = 'Open sealed offer'
             AND json_extract(NEW.payload_json, '$.buttons[0].disabled') = 0
             AND length(json_extract(NEW.payload_json, '$.buttons[0].custom_id')) = 41
             AND substr(json_extract(NEW.payload_json, '$.buttons[0].custom_id'), 1, 5) = 'sg:1:'
             AND json_array_length(json_extract(NEW.payload_json, '$.allowed_user_mentions')) = 1
             AND json_extract(NEW.payload_json, '$.allowed_user_mentions[0]') = wager.target_user_id)
            OR
            (wager.state IN ('accepted_funded','awaiting_resolution')
             AND json_extract(NEW.payload_json, '$.content') =
                 CASE wager.is_test WHEN 1 THEN '[TEST] Fate wager reminder'
                                    ELSE 'Fate wager reminder' END
             AND json_extract(NEW.payload_json, '$.embed.title') =
                 'A peer wager awaits resolution'
             AND json_extract(NEW.payload_json, '$.embed.url') = ''
             AND json_extract(NEW.payload_json, '$.embed.description') =
                 'A sealed funded wager is approaching its outcome deadline. Participants should consult their private history.'
             AND json_array_length(json_extract(NEW.payload_json, '$.buttons')) = 0
             AND json_array_length(json_extract(NEW.payload_json, '$.allowed_user_mentions')) = 0)))
          OR
          (NEW.kind = 'discord.message_edit.v1'
           AND wager.state IN ('accepted_funded','awaiting_resolution','disputed',
                               'resolved','void_refunded','cancelled','declined','expired')
           AND json_extract(NEW.payload_json, '$.content') =
               CASE wager.is_test WHEN 1 THEN '[TEST] Sealed Fate wager'
                                  ELSE 'Sealed Fate wager' END
           AND json_extract(NEW.payload_json, '$.embed.title') = 'A sealed wager awaits'
           AND json_extract(NEW.payload_json, '$.embed.url') = ''
           AND json_extract(NEW.payload_json, '$.embed.description') =
               'Status: ' || CASE
                 WHEN wager.state IN ('accepted_funded','awaiting_resolution') THEN 'Funded'
                 WHEN wager.state = 'disputed' THEN 'Disputed'
                 ELSE 'Closed' END ||
               '. The terms remain sealed to the addressed participant.'
           AND json_array_length(json_extract(NEW.payload_json, '$.buttons')) = 0
           AND json_array_length(json_extract(NEW.payload_json, '$.allowed_user_mentions')) = 0),
          0) = 0)
      THEN RAISE(ABORT, 'sealed wager public delivery is not structurally neutral') END;
    SELECT CASE WHEN EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN tarot_wager_public_projection projection
        ON projection.wager_id = wager.wager_id
      WHERE wager.wager_id = NEW.aggregate_id
        AND wager.visibility = 'public'
        AND COALESCE(
          (json_extract(NEW.payload_json, '$.payload_version') = 1
           AND json_extract(NEW.payload_json, '$.guild_id') = wager.guild_id
           AND json_extract(NEW.payload_json, '$.channel_id') = wager.channel_id
           AND json_extract(NEW.payload_json, '$.embed.color') = 9109504
           AND json_extract(NEW.payload_json, '$.embed.url') = ''
           AND json_extract(NEW.payload_json, '$.fail_before_first_send') = 0
           AND (
             (NEW.kind = 'discord.public.v1' AND (
               (wager.state = 'offered'
                AND json_extract(NEW.payload_json, '$.content') =
                    CASE wager.is_test WHEN 1 THEN '[TEST] Peer Fate wager'
                                       ELSE 'Peer Fate wager' END
                AND json_extract(NEW.payload_json, '$.embed.title') =
                    'The Emperor''s Tarot — Peer Wager'
                AND json_extract(NEW.payload_json, '$.embed.description') =
                    projection.expected_description
                AND json_array_length(
                      json_extract(NEW.payload_json, '$.buttons')) = 3
                AND json_extract(NEW.payload_json, '$.buttons[0].label') =
                    'Accept and fund'
                AND json_extract(NEW.payload_json, '$.buttons[0].disabled') = 0
                AND length(json_extract(
                      NEW.payload_json, '$.buttons[0].custom_id')) = 42
                AND substr(json_extract(
                      NEW.payload_json, '$.buttons[0].custom_id'), 1, 6) = 'sgw:1:'
                AND EXISTS (
                  SELECT 1 FROM tarot_wager_control control
                  WHERE control.token_id = substr(json_extract(
                          NEW.payload_json, '$.buttons[0].custom_id'), 7)
                    AND control.wager_id = wager.wager_id
                    AND control.action = 'accept'
                    AND control.expected_user_id = wager.target_user_id
                    AND control.expected_revision = wager.revision
                    AND control.state = 'active')
                AND json_extract(NEW.payload_json, '$.buttons[1].label') =
                    'Decline'
                AND json_extract(NEW.payload_json, '$.buttons[1].disabled') = 0
                AND length(json_extract(
                      NEW.payload_json, '$.buttons[1].custom_id')) = 42
                AND substr(json_extract(
                      NEW.payload_json, '$.buttons[1].custom_id'), 1, 6) = 'sgw:1:'
                AND EXISTS (
                  SELECT 1 FROM tarot_wager_control control
                  WHERE control.token_id = substr(json_extract(
                          NEW.payload_json, '$.buttons[1].custom_id'), 7)
                    AND control.wager_id = wager.wager_id
                    AND control.action = 'decline'
                    AND control.expected_user_id = wager.target_user_id
                    AND control.expected_revision = wager.revision
                    AND control.state = 'active')
                AND json_extract(NEW.payload_json, '$.buttons[2].label') =
                    'Cancel'
                AND json_extract(NEW.payload_json, '$.buttons[2].disabled') = 0
                AND length(json_extract(
                      NEW.payload_json, '$.buttons[2].custom_id')) = 42
                AND substr(json_extract(
                      NEW.payload_json, '$.buttons[2].custom_id'), 1, 6) = 'sgw:1:'
                AND EXISTS (
                  SELECT 1 FROM tarot_wager_control control
                  WHERE control.token_id = substr(json_extract(
                          NEW.payload_json, '$.buttons[2].custom_id'), 7)
                    AND control.wager_id = wager.wager_id
                    AND control.action = 'cancel'
                    AND control.expected_user_id = wager.creator_user_id
                    AND control.expected_revision = wager.revision
                    AND control.state = 'active')
                AND json_array_length(json_extract(
                      NEW.payload_json, '$.allowed_user_mentions')) = 0)
               OR
               (wager.state IN ('accepted_funded','awaiting_resolution')
                AND json_extract(NEW.payload_json, '$.content') =
                    CASE wager.is_test WHEN 1 THEN '[TEST] Fate wager reminder'
                                       ELSE 'Fate wager reminder' END
                AND json_extract(NEW.payload_json, '$.embed.title') =
                    'A peer wager awaits resolution'
                AND json_extract(NEW.payload_json, '$.embed.description') =
                    'Wager `' || substr(wager.wager_id, 1, 8) ||
                    '` is approaching its outcome deadline.'
                AND json_array_length(json_extract(
                      NEW.payload_json, '$.buttons')) = 0
                AND json_array_length(json_extract(
                      NEW.payload_json, '$.allowed_user_mentions')) = 0)))
             OR
             (NEW.kind = 'discord.message_edit.v1'
              AND wager.state IN ('accepted_funded','awaiting_resolution',
                                  'disputed','resolved','void_refunded',
                                  'cancelled','declined','expired')
              AND json_extract(NEW.payload_json, '$.content') =
                  CASE wager.is_test WHEN 1 THEN '[TEST] Peer Fate wager'
                                     ELSE 'Peer Fate wager' END
              AND json_extract(NEW.payload_json, '$.embed.title') =
                  'The Emperor''s Tarot — Peer Wager'
              AND json_extract(NEW.payload_json, '$.embed.description') =
                  projection.expected_description
              AND json_array_length(json_extract(
                    NEW.payload_json, '$.buttons')) = 0
              AND json_array_length(json_extract(
                    NEW.payload_json, '$.allowed_user_mentions')) = 0
              AND json_extract(NEW.payload_json, '$.wager_revision') =
                  wager.revision
              AND json_extract(NEW.payload_json, '$.source_outbox_id') =
                  (SELECT create_outbox_id FROM tarot_wager_public_card
                   WHERE wager_id = wager.wager_id)))),
          0) = 0)
      THEN RAISE(ABORT, 'public wager delivery does not match its safe projection') END;
END;

CREATE TRIGGER tarot_wager_public_card_guard
BEFORE INSERT ON tarot_wager_public_card
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN outbox_message outbox ON outbox.outbox_id = NEW.create_outbox_id
      WHERE wager.wager_id = NEW.wager_id
        AND wager.state = 'offered'
        AND NEW.created_revision = wager.revision
        AND outbox.kind = 'discord.public.v1'
        AND outbox.aggregate_id = wager.wager_id)
      THEN RAISE(ABORT, 'wager public card linkage is invalid') END;
END;

CREATE TRIGGER tarot_wager_card_revision_guard
BEFORE INSERT ON tarot_wager_card_revision
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM tarot_wager wager
      JOIN tarot_wager_public_card card ON card.wager_id = wager.wager_id
      JOIN outbox_message outbox ON outbox.outbox_id = NEW.outbox_id
      WHERE wager.wager_id = NEW.wager_id
        AND NEW.wager_revision = wager.revision
        AND outbox.kind = 'discord.message_edit.v1'
        AND outbox.aggregate_id = wager.wager_id
        AND json_extract(outbox.payload_json, '$.source_outbox_id') = card.create_outbox_id
        AND json_extract(outbox.payload_json, '$.wager_revision') = NEW.wager_revision)
      THEN RAISE(ABORT, 'wager card revision linkage is invalid') END;
END;
