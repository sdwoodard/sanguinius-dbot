CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY CHECK (version > 0),
    name TEXT NOT NULL UNIQUE
        CHECK (length(name) BETWEEN 1 AND 100),
    checksum TEXT NOT NULL
        CHECK (length(checksum) = 64
               AND checksum NOT GLOB '*[^0-9a-f]*'),
    applied_at_ms INTEGER NOT NULL CHECK (applied_at_ms >= 0),
    application_version TEXT NOT NULL
        CHECK (length(application_version) BETWEEN 1 AND 128)
) STRICT;

CREATE TABLE application_instance (
    instance_id TEXT PRIMARY KEY
        CHECK (length(instance_id) = 36
               AND substr(instance_id, 9, 1) = '-'
               AND substr(instance_id, 14, 1) = '-'
               AND substr(instance_id, 19, 1) = '-'
               AND substr(instance_id, 24, 1) = '-'
               AND length(replace(instance_id, '-', '')) = 32
               AND instance_id NOT GLOB '*[^0-9a-f-]*'
               AND substr(instance_id, 15, 1) = '4'
               AND substr(instance_id, 20, 1) IN ('8', '9', 'a', 'b')),
    application_version TEXT NOT NULL
        CHECK (length(application_version) BETWEEN 1 AND 128),
    git_revision TEXT NOT NULL
        CHECK (length(git_revision) BETWEEN 1 AND 128),
    hostname TEXT NOT NULL
        CHECK (length(hostname) BETWEEN 1 AND 255),
    process_id INTEGER NOT NULL CHECK (process_id > 0),
    started_at_ms INTEGER NOT NULL CHECK (started_at_ms >= 0),
    stopped_at_ms INTEGER
        CHECK (stopped_at_ms IS NULL OR stopped_at_ms >= started_at_ms),
    stop_reason TEXT
        CHECK (stop_reason IS NULL
               OR stop_reason IN ('clean_shutdown', 'startup_failure')),
    CHECK ((stopped_at_ms IS NULL) = (stop_reason IS NULL))
) STRICT;

CREATE TABLE discord_user (
    user_id TEXT PRIMARY KEY
        CHECK (length(user_id) BETWEEN 1 AND 20
               AND user_id NOT GLOB '*[^0-9]*'
               AND user_id <> '0'
               AND (length(user_id) = 1 OR substr(user_id, 1, 1) <> '0')
               AND (length(user_id) < 20
                    OR user_id <= '18446744073709551615')),
    display_name_cache TEXT
        CHECK (display_name_cache IS NULL
               OR length(display_name_cache) BETWEEN 1 AND 128),
    username_cache TEXT
        CHECK (username_cache IS NULL
               OR length(username_cache) BETWEEN 1 AND 128),
    is_bot INTEGER NOT NULL CHECK (is_bot IN (0, 1)),
    first_seen_at_ms INTEGER NOT NULL CHECK (first_seen_at_ms >= 0),
    last_seen_at_ms INTEGER NOT NULL CHECK (last_seen_at_ms >= first_seen_at_ms),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms)
) STRICT;

CREATE TABLE guild_config (
    guild_id TEXT PRIMARY KEY
        CHECK (length(guild_id) BETWEEN 1 AND 20
               AND guild_id NOT GLOB '*[^0-9]*'
               AND guild_id <> '0'
               AND (length(guild_id) = 1 OR substr(guild_id, 1, 1) <> '0')
               AND (length(guild_id) < 20
                    OR guild_id <= '18446744073709551615')),
    singleton INTEGER NOT NULL DEFAULT 1 UNIQUE CHECK (singleton = 1),
    primary_channel_id TEXT NOT NULL
        CHECK (length(primary_channel_id) BETWEEN 1 AND 20
               AND primary_channel_id NOT GLOB '*[^0-9]*'
               AND primary_channel_id <> '0'
               AND (length(primary_channel_id) = 1
                    OR substr(primary_channel_id, 1, 1) <> '0')
               AND (length(primary_channel_id) < 20
                    OR primary_channel_id <= '18446744073709551615')),
    owner_user_id TEXT NOT NULL
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE RESTRICT,
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= created_at_ms)
) STRICT;

CREATE TABLE user_preference (
    user_id TEXT PRIMARY KEY
        REFERENCES discord_user(user_id) ON UPDATE RESTRICT ON DELETE CASCADE,
    chronicle_opt_in INTEGER NOT NULL DEFAULT 0
        CHECK (chronicle_opt_in IN (0, 1)),
    memory_callback_opt_in INTEGER NOT NULL DEFAULT 0
        CHECK (memory_callback_opt_in IN (0, 1)),
    appearance_callback_opt_in INTEGER NOT NULL DEFAULT 0
        CHECK (appearance_callback_opt_in IN (0, 1)),
    voice_input_opt_in INTEGER NOT NULL DEFAULT 0
        CHECK (voice_input_opt_in IN (0, 1)),
    public_tarot_results_opt_in INTEGER NOT NULL DEFAULT 1
        CHECK (public_tarot_results_opt_in IN (0, 1)),
    quiet_until_ms INTEGER CHECK (quiet_until_ms IS NULL OR quiet_until_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
) STRICT;
