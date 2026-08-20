CREATE TABLE tarot_account (
    account_id TEXT PRIMARY KEY
        CHECK (length(account_id) = 36
               AND substr(account_id, 9, 1) = '-'
               AND substr(account_id, 14, 1) = '-'
               AND substr(account_id, 19, 1) = '-'
               AND substr(account_id, 24, 1) = '-'
               AND length(replace(account_id, '-', '')) = 32
               AND account_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(account_id, 15, 1) = '4'
               AND substr(account_id, 20, 1) IN ('8', '9', 'a', 'b')),
    account_kind TEXT NOT NULL
        CHECK (account_kind IN ('HUMAN', 'MINT', 'HOUSE', 'ESCROW', 'BURN')),
    user_id TEXT UNIQUE
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK ((account_kind = 'HUMAN') = (user_id IS NOT NULL))
) STRICT;

CREATE UNIQUE INDEX tarot_account_system_kind
    ON tarot_account(account_kind) WHERE account_kind <> 'HUMAN';

CREATE TRIGGER tarot_account_no_update
BEFORE UPDATE ON tarot_account
BEGIN
    SELECT RAISE(ABORT, 'tarot accounts are immutable');
END;

CREATE TRIGGER tarot_account_no_delete
BEFORE DELETE ON tarot_account
BEGIN
    SELECT RAISE(ABORT, 'tarot accounts are immutable');
END;

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
                                    'TEST_ADJUSTMENT', 'TEST_REVERSAL')),
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

CREATE INDEX tarot_posting_account_history
    ON tarot_posting(account_id, transaction_id);
CREATE INDEX tarot_transaction_idempotency
    ON tarot_transaction(idempotency_key);
CREATE INDEX tarot_transaction_reversal
    ON tarot_transaction(reversal_of_transaction_id)
    WHERE reversal_of_transaction_id IS NOT NULL;

CREATE TRIGGER tarot_transaction_insert_prepared
BEFORE INSERT ON tarot_transaction
WHEN NEW.state <> 'prepared'
BEGIN
    SELECT RAISE(ABORT, 'tarot transactions must begin prepared');
END;

CREATE TRIGGER tarot_posting_insert_only_prepared
BEFORE INSERT ON tarot_posting
BEGIN
    SELECT CASE
      WHEN (SELECT state FROM tarot_transaction
            WHERE transaction_id = NEW.transaction_id) <> 'prepared'
      THEN RAISE(ABORT, 'tarot postings require a prepared transaction')
    END;
    SELECT CASE
      WHEN (SELECT count(*) FROM tarot_posting
            WHERE transaction_id = NEW.transaction_id) >=
           (SELECT expected_posting_count FROM tarot_transaction
            WHERE transaction_id = NEW.transaction_id)
      THEN RAISE(ABORT, 'tarot posting count exceeded')
    END;
END;

CREATE TRIGGER tarot_posting_no_update
BEFORE UPDATE ON tarot_posting
BEGIN
    SELECT RAISE(ABORT, 'tarot postings are immutable');
END;

CREATE TRIGGER tarot_posting_no_delete
BEFORE DELETE ON tarot_posting
BEGIN
    SELECT RAISE(ABORT, 'tarot postings are immutable');
END;

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
                      OR NEW.reason IS NOT OLD.reason
                      OR NEW.is_test <> OLD.is_test
                      OR NEW.reversal_of_transaction_id IS NOT
                         OLD.reversal_of_transaction_id
                      OR NEW.created_at_ms <> OLD.created_at_ms
                THEN RAISE(ABORT, 'tarot transaction metadata is immutable') END;
    SELECT CASE WHEN (SELECT count(*) FROM tarot_posting
                      WHERE transaction_id = OLD.transaction_id) <>
                     OLD.expected_posting_count
                THEN RAISE(ABORT, 'tarot posting count mismatch') END;
    SELECT CASE WHEN (SELECT total(amount) FROM tarot_posting
                      WHERE transaction_id = OLD.transaction_id) <> 0.0
                THEN RAISE(ABORT, 'tarot transaction is unbalanced') END;
    SELECT CASE WHEN OLD.expected_posting_count <> 2
                THEN RAISE(ABORT, 'tarot transaction shape is invalid') END;

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
      THEN RAISE(ABORT, 'tarot test transaction audit provenance is invalid')
    END;

    SELECT CASE WHEN OLD.transaction_type IN ('STARTING_GRANT', 'GRACE', 'TRIAL')
      AND NOT EXISTS (
        SELECT 1
        FROM tarot_posting human_post
        JOIN tarot_account human_account
          ON human_account.account_id = human_post.account_id
        JOIN tarot_posting mint_post
          ON mint_post.transaction_id = human_post.transaction_id
         AND mint_post.account_id <> human_post.account_id
        JOIN tarot_account mint_account
          ON mint_account.account_id = mint_post.account_id
        WHERE human_post.transaction_id = OLD.transaction_id
          AND human_account.account_kind = 'HUMAN'
          AND mint_account.account_kind = 'MINT'
          AND human_post.amount > 0
          AND mint_post.amount = -human_post.amount)
      THEN RAISE(ABORT, 'tarot grant shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type = 'STARTING_GRANT'
      AND EXISTS (
        SELECT 1
        FROM tarot_posting candidate
        JOIN tarot_account candidate_account
          ON candidate_account.account_id = candidate.account_id
        JOIN tarot_posting prior
          ON prior.account_id = candidate.account_id
         AND prior.transaction_id <> candidate.transaction_id
        JOIN tarot_transaction prior_tx
          ON prior_tx.transaction_id = prior.transaction_id
        WHERE candidate.transaction_id = OLD.transaction_id
          AND candidate_account.account_kind = 'HUMAN'
          AND prior_tx.transaction_type = 'STARTING_GRANT'
          AND prior_tx.state = 'committed')
      THEN RAISE(ABORT, 'tarot starting grant already exists') END;

    SELECT CASE WHEN OLD.transaction_type = 'TEST_ADJUSTMENT'
      AND NOT EXISTS (
        SELECT 1
        FROM tarot_posting human_post
        JOIN tarot_account human_account
          ON human_account.account_id = human_post.account_id
        JOIN tarot_posting system_post
          ON system_post.transaction_id = human_post.transaction_id
         AND system_post.account_id <> human_post.account_id
        JOIN tarot_account system_account
          ON system_account.account_id = system_post.account_id
        WHERE human_post.transaction_id = OLD.transaction_id
          AND human_account.account_kind = 'HUMAN'
          AND ((human_post.amount > 0
                AND system_account.account_kind = 'MINT')
               OR (human_post.amount < 0
                   AND system_account.account_kind = 'BURN'))
          AND system_post.amount = -human_post.amount)
      THEN RAISE(ABORT, 'tarot adjustment shape is invalid') END;

    SELECT CASE WHEN OLD.transaction_type = 'TEST_REVERSAL'
      AND ((SELECT state FROM tarot_transaction
            WHERE transaction_id = OLD.reversal_of_transaction_id) <> 'committed'
           OR (SELECT is_test FROM tarot_transaction
               WHERE transaction_id = OLD.reversal_of_transaction_id) <> 1
           OR (SELECT transaction_type FROM tarot_transaction
               WHERE transaction_id = OLD.reversal_of_transaction_id)
              IN ('STARTING_GRANT', 'TEST_REVERSAL')
           OR (SELECT count(*)
               FROM tarot_posting original
               JOIN tarot_posting inverse
                 ON inverse.transaction_id = OLD.transaction_id
                AND inverse.account_id = original.account_id
                AND inverse.amount = -original.amount
               WHERE original.transaction_id = OLD.reversal_of_transaction_id) <> 2)
      THEN RAISE(ABORT, 'tarot reversal shape is invalid') END;

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
      SELECT 1
      FROM tarot_posting pending
      JOIN tarot_account account ON account.account_id = pending.account_id
      WHERE pending.transaction_id = OLD.transaction_id
        AND account.account_kind = 'HUMAN'
        AND COALESCE((SELECT sum(prior.amount)
                      FROM tarot_posting prior
                      JOIN tarot_transaction prior_tx
                        ON prior_tx.transaction_id = prior.transaction_id
                      WHERE prior.account_id = pending.account_id
                        AND prior_tx.state = 'committed'), 0)
            + pending.amount < 0)
      THEN RAISE(ABORT, 'tarot human balance cannot be negative') END;
END;

CREATE TRIGGER tarot_transaction_update_guard
BEFORE UPDATE ON tarot_transaction
WHEN NOT (OLD.state = 'prepared' AND NEW.state = 'committed')
BEGIN
    SELECT RAISE(ABORT, 'tarot transactions are immutable after creation');
END;

CREATE TRIGGER tarot_transaction_no_delete
BEFORE DELETE ON tarot_transaction
BEGIN
    SELECT RAISE(ABORT, 'tarot transactions are immutable');
END;

CREATE TABLE tarot_draw (
    draw_id TEXT PRIMARY KEY,
    claim_id TEXT NOT NULL UNIQUE,
    prompt_variant INTEGER NOT NULL CHECK (prompt_variant BETWEEN 0 AND 2),
    reward INTEGER NOT NULL CHECK (reward BETWEEN 1 AND 1000000000),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0)
) STRICT;

CREATE TRIGGER tarot_draw_no_update
BEFORE UPDATE ON tarot_draw
BEGIN SELECT RAISE(ABORT, 'tarot draws are immutable'); END;
CREATE TRIGGER tarot_draw_no_delete
BEFORE DELETE ON tarot_draw
BEGIN SELECT RAISE(ABORT, 'tarot draws are immutable'); END;

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
        CHECK (grace_target IS NULL
               OR grace_target BETWEEN 1 AND 1000000000),
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
        CHECK (completion_idempotency_key IS NULL
               OR length(completion_idempotency_key) BETWEEN 1 AND 160),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms),
    completed_at_ms INTEGER
        CHECK (completed_at_ms IS NULL OR completed_at_ms >= created_at_ms),
    cooldown_until_ms INTEGER
        CHECK (cooldown_until_ms IS NULL OR cooldown_until_ms >= completed_at_ms),
    CHECK ((claim_type = 'GRACE') = (grace_target IS NOT NULL)),
    CHECK (claim_type <> 'GRACE'
           OR grace_target > eligibility_threshold),
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

CREATE UNIQUE INDEX tarot_recovery_active
    ON tarot_recovery_claim(account_id, claim_type, is_test)
    WHERE state = 'pending';
CREATE INDEX tarot_recovery_cooldown
    ON tarot_recovery_claim(account_id, claim_type, is_test, cooldown_until_ms DESC)
    WHERE state = 'completed';
CREATE INDEX tarot_recovery_expiry
    ON tarot_recovery_claim(expires_at_ms) WHERE state = 'pending';

CREATE TRIGGER tarot_recovery_insert_pending
BEFORE INSERT ON tarot_recovery_claim
WHEN NEW.state <> 'pending'
BEGIN
    SELECT RAISE(ABORT, 'tarot recovery claims must begin pending');
END;

CREATE TRIGGER tarot_recovery_transaction_seal_link
BEFORE UPDATE ON tarot_transaction
WHEN OLD.state = 'prepared' AND NEW.state = 'committed'
     AND OLD.transaction_type IN ('GRACE', 'TRIAL')
BEGIN
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1
      FROM event_journal terminal
      JOIN tarot_recovery_claim claim
        ON claim.claim_id = terminal.aggregate_id
      JOIN tarot_posting human_post
        ON human_post.transaction_id = OLD.transaction_id
       AND human_post.account_id = claim.account_id
      JOIN tarot_account human_account
        ON human_account.account_id = human_post.account_id
      WHERE terminal.event_id = OLD.event_id
        AND terminal.aggregate_type = 'tarot_recovery_claim'
        AND terminal.event_type = CASE OLD.transaction_type
              WHEN 'GRACE' THEN 'tarot.grace_completed.v1'
              ELSE 'tarot.trial_completed.v1' END
        AND claim.state = 'pending'
        AND claim.claim_type = OLD.transaction_type
        AND claim.is_test = OLD.is_test
        AND human_account.account_kind = 'HUMAN'
        AND human_post.amount > 0
        AND (OLD.transaction_type = 'GRACE'
             OR claim.reward = human_post.amount))
      THEN RAISE(ABORT, 'tarot recovery transaction is not linked to its claim')
    END;
END;

CREATE TRIGGER tarot_recovery_transition_guard
BEFORE UPDATE ON tarot_recovery_claim
WHEN NOT (OLD.state = 'pending' AND NEW.state = 'pending')
BEGIN
    SELECT CASE WHEN OLD.state <> 'pending'
      THEN RAISE(ABORT, 'terminal tarot claim is immutable') END;
    SELECT CASE WHEN NEW.state NOT IN ('completed', 'expired', 'abandoned')
      THEN RAISE(ABORT, 'invalid tarot claim transition') END;
    SELECT CASE WHEN NEW.claim_id <> OLD.claim_id
                      OR NEW.account_id <> OLD.account_id
                      OR NEW.claim_type <> OLD.claim_type
                      OR NEW.visibility <> OLD.visibility
                      OR NEW.is_test <> OLD.is_test
                      OR NEW.eligibility_threshold <> OLD.eligibility_threshold
                      OR NEW.grace_target IS NOT OLD.grace_target
                      OR NEW.eligibility_balance <> OLD.eligibility_balance
                      OR (NEW.reward IS NOT OLD.reward
                          AND NOT (OLD.claim_type = 'GRACE'
                                   AND OLD.reward IS NULL
                                   AND NEW.reward IS NOT NULL))
                      OR NEW.draw_id IS NOT OLD.draw_id
                      OR NEW.started_event_id <> OLD.started_event_id
                      OR NEW.start_idempotency_key <> OLD.start_idempotency_key
                      OR NEW.created_at_ms <> OLD.created_at_ms
                      OR NEW.expires_at_ms <> OLD.expires_at_ms
      THEN RAISE(ABORT, 'tarot claim identity is immutable') END;
    SELECT CASE WHEN NOT EXISTS (
      SELECT 1 FROM event_journal terminal
      WHERE terminal.event_id = NEW.event_id
        AND terminal.aggregate_type = 'tarot_recovery_claim'
        AND terminal.aggregate_id = NEW.claim_id
        AND ((NEW.state = 'completed'
              AND terminal.event_type = CASE NEW.claim_type
                    WHEN 'GRACE' THEN 'tarot.grace_completed.v1'
                    ELSE 'tarot.trial_completed.v1' END)
             OR (NEW.state = 'expired'
                 AND terminal.event_type = 'tarot.recovery_expired.v1')
             OR (NEW.state = 'abandoned'
                 AND terminal.event_type =
                     'tarot.recovery_eligibility_lost.v1')
             OR (NEW.state = 'abandoned' AND NEW.claim_type = 'TRIAL'
                 AND terminal.event_type = 'tarot.trial_abandoned.v1')))
      THEN RAISE(ABORT, 'tarot claim terminal event is invalid') END;
    SELECT CASE WHEN NEW.state = 'completed' AND NOT EXISTS (
      SELECT 1
      FROM tarot_transaction tx
      JOIN tarot_posting posting ON posting.transaction_id = tx.transaction_id
      JOIN tarot_account account ON account.account_id = posting.account_id
      WHERE tx.transaction_id = NEW.transaction_id
        AND tx.state = 'committed'
        AND tx.transaction_type = NEW.claim_type
        AND tx.event_id = NEW.event_id
        AND tx.is_test = NEW.is_test
        AND posting.account_id = NEW.account_id
        AND account.account_kind = 'HUMAN'
        AND posting.amount = NEW.reward)
      THEN RAISE(ABORT, 'tarot completed claim does not match its ledger reward')
    END;
    SELECT CASE WHEN NEW.state = 'completed' AND NEW.visibility = 'public'
      AND NOT EXISTS (
        SELECT 1 FROM outbox_message outbox
        JOIN event_journal started ON started.event_id = NEW.started_event_id
        WHERE outbox.outbox_id = NEW.outbox_id
          AND outbox.kind = 'discord.public.v1'
          AND outbox.aggregate_type = 'tarot_recovery_claim'
          AND outbox.aggregate_id = NEW.claim_id
          AND outbox.target_guild_id = started.guild_id
          AND outbox.target_channel_id = started.channel_id
          AND outbox.target_user_id IS NULL)
      THEN RAISE(ABORT, 'public tarot claim requires its scoped outbox row')
    END;
    SELECT CASE WHEN (NEW.state = 'completed' AND NEW.visibility = 'private'
                           AND NEW.outbox_id IS NOT NULL)
                         OR (NEW.state IN ('expired', 'abandoned')
                             AND NEW.outbox_id IS NOT NULL)
      THEN RAISE(ABORT, 'private or unrewarded tarot claim cannot link outbox')
    END;
END;

CREATE TRIGGER tarot_recovery_cancel_terminal_tokens
AFTER UPDATE ON tarot_recovery_claim
WHEN OLD.state = 'pending' AND NEW.state IN ('completed', 'expired', 'abandoned')
BEGIN
    UPDATE interaction_token
    SET state = 'cancelled'
    WHERE entity_type = 'tarot_recovery_claim'
      AND entity_id = NEW.claim_id
      AND state = 'active';
END;

CREATE TRIGGER tarot_recovery_privacy_tighten
BEFORE UPDATE ON tarot_recovery_claim
WHEN OLD.state = 'pending' AND NEW.state = 'pending'
BEGIN
    SELECT CASE WHEN OLD.visibility <> 'public' OR NEW.visibility <> 'private'
                      OR NEW.claim_id <> OLD.claim_id
                      OR NEW.account_id <> OLD.account_id
                      OR NEW.claim_type <> OLD.claim_type
                      OR NEW.is_test <> OLD.is_test
                      OR NEW.eligibility_threshold <> OLD.eligibility_threshold
                      OR NEW.grace_target IS NOT OLD.grace_target
                      OR NEW.eligibility_balance <> OLD.eligibility_balance
                      OR NEW.reward IS NOT OLD.reward
                      OR NEW.draw_id IS NOT OLD.draw_id
                      OR NEW.transaction_id IS NOT OLD.transaction_id
                      OR NEW.started_event_id <> OLD.started_event_id
                      OR NEW.event_id IS NOT OLD.event_id
                      OR NEW.outbox_id IS NOT OLD.outbox_id
                      OR NEW.start_idempotency_key <> OLD.start_idempotency_key
                      OR NEW.completion_idempotency_key IS NOT
                         OLD.completion_idempotency_key
                      OR NEW.created_at_ms <> OLD.created_at_ms
                      OR NEW.expires_at_ms <> OLD.expires_at_ms
                      OR NEW.completed_at_ms IS NOT OLD.completed_at_ms
                      OR NEW.cooldown_until_ms IS NOT OLD.cooldown_until_ms
      THEN RAISE(ABORT, 'pending tarot claims only permit privacy tightening')
    END;
END;

CREATE TRIGGER tarot_recovery_no_delete
BEFORE DELETE ON tarot_recovery_claim
BEGIN SELECT RAISE(ABORT, 'tarot recovery claims are retained'); END;

CREATE TABLE tarot_interaction_receipt (
    idempotency_key TEXT PRIMARY KEY
        CHECK (length(idempotency_key) BETWEEN 1 AND 160),
    operation TEXT NOT NULL
        CHECK (operation IN ('standings_visibility', 'recovery_start',
                             'adjust', 'reverse')),
    account_id TEXT NOT NULL REFERENCES tarot_account(account_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    request_json TEXT NOT NULL
        CHECK (json_valid(request_json)
               AND json_type(request_json) = 'object'
               AND length(CAST(request_json AS BLOB)) BETWEEN 2 AND 2048),
    result_json TEXT NOT NULL
        CHECK (json_valid(result_json)
               AND json_type(result_json) = 'object'
               AND length(CAST(result_json AS BLOB)) BETWEEN 2 AND 2048),
    claim_id TEXT REFERENCES tarot_recovery_claim(claim_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    transaction_id TEXT REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    CHECK (claim_id IS NULL OR operation = 'recovery_start'),
    CHECK (transaction_id IS NULL OR operation IN ('adjust', 'reverse'))
) STRICT;

CREATE INDEX tarot_interaction_receipt_account
    ON tarot_interaction_receipt(account_id, operation, created_at_ms);

CREATE TRIGGER tarot_interaction_receipt_no_update
BEFORE UPDATE ON tarot_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'tarot interaction receipts are immutable'); END;

CREATE TRIGGER tarot_interaction_receipt_no_delete
BEFORE DELETE ON tarot_interaction_receipt
BEGIN SELECT RAISE(ABORT, 'tarot interaction receipts are retained'); END;

CREATE TABLE tarot_history_cursor (
    cursor_id TEXT PRIMARY KEY,
    account_id TEXT NOT NULL
        REFERENCES tarot_account(account_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    item_count INTEGER NOT NULL CHECK (item_count BETWEEN 0 AND 50),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    expires_at_ms INTEGER NOT NULL CHECK (expires_at_ms > created_at_ms)
) STRICT;

CREATE TABLE tarot_history_item (
    cursor_id TEXT NOT NULL REFERENCES tarot_history_cursor(cursor_id)
        ON UPDATE RESTRICT ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 49),
    transaction_id TEXT NOT NULL REFERENCES tarot_transaction(transaction_id)
        ON UPDATE RESTRICT ON DELETE RESTRICT,
    PRIMARY KEY (cursor_id, position)
) STRICT;

CREATE INDEX tarot_history_cursor_expiry ON tarot_history_cursor(expires_at_ms);
CREATE INDEX tarot_standings_human ON tarot_account(user_id)
    WHERE account_kind = 'HUMAN';
