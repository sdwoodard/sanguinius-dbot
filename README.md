# Sanguinius

Sanguinius is a Discord bot built with modern C++ and [D++](https://dpp.dev/).
It supports guild-scoped slash commands and sealed notices, preserves two
public prefix commands, answers messages that begin with a bot mention through
the OpenAI Responses API, and writes every visible guild message-create event
to an append-only text log. Typed configuration fixes the bot's feature
boundary to one guild, one primary text channel, and one owner.

## Commands

| Command | Description |
| --- | --- |
| `!help` | List the supported commands. |
| `!repo` | Link to this source repository. |

The configured guild also receives command catalog version 1:

| Command | Visibility and behavior |
| --- | --- |
| `/sanguinius status` | Ephemeral readiness, feature-mode, and unopened-notice summary. |
| `/sanguinius inbox` | Ephemerally opens the oldest pending sealed notice. Duplicate Discord interaction IDs replay the same result. |
| `/sanguinius privacy` | Ephemeral identity/preference, voice-input, no-DM, and raw-voice-retention summary. |
| `/sang-admin health` | Ephemeral owner-only health; registered only when admin commands are enabled. |
| `/sang-admin test-notice` | Owner-only, test-mode-gated creation of a fixed, self-targeted 24-hour notice. |

Command names are case-insensitive. Messages written by bots are logged but are
not treated as commands.

When owner administration is explicitly enabled, the transitional
`!sang-admin health` command continues to provide a public but strictly
redacted health snapshot in the configured primary channel. It is not listed
by `!help` and is silent for other users or channels. Prefer the ephemeral
slash-command form for routine use.

Feature interactions are accepted only in the configured guild and primary
channel. Ordinary slash responses are ephemeral. Discord cannot proactively
send an arbitrary user an ephemeral message, and Sanguinius never uses DMs;
private proactive content is stored as a pending notice. A neutral public card
may mention only its target and contains no private title/body or database
identifier. The target retrieves the content by clicking its opaque,
expiring button or by running `/sanguinius inbox`.

To talk to the AI persona, mention the bot at the start of a message:

```text
@sanguinius What is your favorite color?
```

The bot supplies the replied-to message, when present, plus up to eight recent
messages from the same channel as optional context. The persona is defined in
[`config/persona.txt`](config/persona.txt). Two AI workers process requests in
parallel, while bounded 64-item application and AI queues prevent an influx of
messages from consuming unlimited memory. Message logging, command routing, and
AI work run outside D++ gateway callbacks.

## Requirements

- A C++20 compiler
- CMake 3.25 or newer
- Ninja (for the supplied presets)
- D++ with its CMake package configuration installed
- libcurl
- nlohmann-json
- SQLite 3.51.3 or newer, or the fixed 3.50.7/3.44.6 backport
- Catch2 v3 when building tests

On CachyOS/Arch Linux, install the dependencies supplied by the distribution:

```bash
sudo pacman -S --needed cmake ninja gcc curl nlohmann-json sqlite catch2
```

D++ must still be installed separately, as described above. CMake links the
well-established libcurl HTTP client and header-only nlohmann-json parser; no
OpenAI-specific third-party SDK is required.

The locally built D++ 10.1.7 installation under `/usr/local` is discovered by
its exported `dpp::dpp` CMake target. If D++ is installed under a different
prefix, configure with `-DCMAKE_PREFIX_PATH=/path/to/prefix`.

Catch2 is a development-only dependency. CMake requires it only when
`BUILD_TESTING=ON`; a prebuilt production executable does not require Catch2 on
the runtime host.

The Discord application must have the **Message Content Intent** enabled in the
Developer Portal. The gateway requests only guild, guild-message, and message
content intents; direct-message and voice-state intents are deliberately not
requested. The bot also needs permission to view channels, read message
history, and send messages wherever it is expected to operate. Voice-state
intent and permissions remain deferred to the Vox milestone.

## Configure

Provide the Discord bot token through one of these environment variables:

```bash
export SANGUINIUS_TOKEN='your-token'
# Or keep the token in a permission-restricted file:
export SANGUINIUS_TOKEN_FILE="$HOME/.config/sanguinius/bot.token"
```

The start script uses `$HOME/.config/sanguinius/bot.token` automatically when
neither variable is set. The file should be readable only by its owner.

The following canonical decimal Discord IDs are required. Values must be
nonzero, contain digits only, have no leading zero, and fit losslessly in an
unsigned 64-bit snowflake. Keep real IDs in local or production configuration,
not in Git:

```bash
export SANGUINIUS_GUILD_ID='replace-locally'
export SANGUINIUS_PRIMARY_CHANNEL_ID='replace-locally'
export SANGUINIUS_OWNER_USER_ID='replace-locally'
```

### OpenAI API key and billing

ChatGPT subscriptions and OpenAI API usage are
[billed separately](https://help.openai.com/en/articles/8156019); a ChatGPT
Plus/Pro subscription does not fund API calls. Create a secret key from the
[OpenAI API keys page](https://platform.openai.com/api-keys), configure API
billing, and store the key locally:

```bash
install -d -m 700 "$HOME/.config/sanguinius"
read -rsp 'OpenAI API key: ' sanguinius_openai_key
printf '\n'
printf '%s\n' "${sanguinius_openai_key}" > "$HOME/.config/sanguinius/openai.key"
unset sanguinius_openai_key
chmod 600 "$HOME/.config/sanguinius/openai.key"
```

Do not use a ChatGPT browser/session token. By default, the start script sets
`SANGUINIUS_OPENAI_API_KEY_FILE` to
`$HOME/.config/sanguinius/openai.key`; typed configuration reads the file
directly without exporting its contents as `OPENAI_API_KEY`. You may instead
provide the key directly for the current process:

```bash
export OPENAI_API_KEY='your-api-key'
```

The default model is `gpt-5.4-nano`, selected for low API cost. It does not have
a free API tier. Review its [current model pricing and rate
limits](https://developers.openai.com/api/docs/models/gpt-5.4-nano) and set usage
limits in the OpenAI platform before running the bot.

Optional settings are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `SANGUINIUS_LOG_FILE` | `logs/messages.log` | Message log path. |
| `SANGUINIUS_COMMAND_PREFIX` | `!` | Command prefix (1–8 non-space characters). |
| `SANGUINIUS_OPENAI_API_KEY_FILE` | Set by start script | OpenAI API key file. |
| `SANGUINIUS_OPENAI_MODEL` | `gpt-5.4-nano` | Responses API model. |
| `SANGUINIUS_PERSONA_FILE` | `config/persona.txt` | Plaintext persona instructions. |
| `SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS` | `10` | Discord REST timeout, from 1 through 300 seconds. |
| `SANGUINIUS_DATABASE_FILE` | `state/sanguinius.sqlite3` | SQLite state file. Production should use an absolute path outside release directories. |
| `SANGUINIUS_ADMIN_COMMANDS_ENABLED` | `false` | Register owner slash controls and enable the transitional prefix health command. |
| `SANGUINIUS_TEST_MODE` | `false` | Enable auditable owner test controls such as self-targeted `test-notice`. |
| `SANGUINIUS_CHRONICLE_ENABLED` | `false` | Configured Chronicle mode; no Chronicle behavior exists yet. |
| `SANGUINIUS_TAROT_ENABLED` | `false` | Configured Tarot mode; no Tarot behavior exists yet. |
| `SANGUINIUS_APPEARANCES_MODE` | `off` | Configured `off`, `dry_run`, or `live` intent; no appearance service exists yet. |
| `SANGUINIUS_VOX_ENABLED` | `false` | Configured Vox mode; no voice connection exists yet. |
| `SANGUINIUS_VOICE_INPUT_ENABLED` | `false` | Reserved privacy gate; voice input remains unavailable. |

Boolean variables accept only the exact lowercase values `true` and `false`.
Every explicitly supplied variable must have a nonempty value; omit an
optional variable to select its default. Empty values do not silently fall
back to the command prefix, model, persona, path, or credential defaults.
The sample [configuration environment](config/sanguinius.env.example) contains
all fields without real IDs or credentials.

Validate the complete configuration without starting D++, connecting to
Discord, or constructing the OpenAI client:

```bash
./build/release/sanguinius --check-config
```

The report shows version, revision, configured/default origin for every
defaultable setting, and feature modes without printing tokens, API keys,
Discord IDs, paths, or persona content. Configuration and message-log startup
failures likewise identify fields rather than configured paths. The start
script runs this check before creating the background process.

Do not commit tokens or environment files. A token exposed in source control
must be regenerated in the Discord Developer Portal.

## Database maintenance

Database maintenance is deliberately offline and needs only
`SANGUINIUS_DATABASE_FILE`; it does not construct D++, read Discord/OpenAI
credentials, or contact the network:

```bash
./build/release/sanguinius db status
./build/release/sanguinius db migrate
./build/release/sanguinius db check
./build/release/sanguinius db integrity
./build/release/sanguinius db backup /restricted/backup/sanguinius.sqlite3
```

`db status` is safe for absent, uninitialized, pending, current, and
incompatible databases; it prints an incompatible status but returns failure
so automation cannot mistake it for a usable schema. `db check` requires the
exact embedded schema and WAL mode. `db migrate` is the only command allowed to
create a database, enable WAL, or apply forward migrations; stop the bot first
because migration takes the exclusive database sidecar lock. Normal startup
never creates or upgrades the schema.

Migration `0001_core_foundation` contains only shared identity and
configuration state: migration history, application instances, Discord users,
the one-guild scope, and user preferences. Migration
`0002_discord_interactions` adds pending notices, scoped opaque interaction
tokens, durable reveal-attempt/idempotency records, state/timestamp constraints,
and lookup/expiry indexes. A notice remains pending until Discord confirms its
private interaction response; failed or interrupted delivery leaves it
retrievable through the inbox. The readable SQL for each ordered migration is
embedded independently with its SHA-256
checksum. Applied history must be ordered, contiguous, and byte-for-byte
checksum compatible with the running binary. The effective `sqlite_schema`
must also match the schema reconstructed from every applied embedded
migration, including table constraints, defaults, foreign keys, indexes,
views, and triggers. Final schema validation occurs before the migration
transaction commits.

### Guild command management

Normal startup compares a canonical projection of the configured guild's
commands and bulk-reconciles the catalog only when it differs. It never creates
global commands. The following explicit operator modes use only the Discord
token, guild ID, REST timeout, and admin-command flag; they do not open SQLite,
construct OpenAI, or start the normal application:

```bash
./build/release/sanguinius discord commands sync
./build/release/sanguinius discord commands clear --confirm
```

The catalog is authoritative for this bot application in its one configured
guild. `clear` deliberately requires the exact `--confirm` guard and is the
command-registration portion of rollback.

Backups use SQLite's online backup API, so `db backup` may run while the bot is
active under a shared lock. A destination and its SQLite sidecar names must not
already exist. The completed copy is converted to `DELETE` journal mode,
reopened read-only, and checked for both database integrity and foreign-key
violations before success is reported. Backup output is mode `0600`; keep its
parent directory restricted as well.

## Build and run

```bash
./scripts/test_bot.bash --clean
./build/release/sanguinius db migrate
./build/release/sanguinius
```

The build script performs an incremental release build by default. Pass
`--clean` to clean the selected preset before compiling everything again. It
always prints the complete CMake configuration and verbose compiler/linker
output.

```bash
./scripts/build_bot.bash
./scripts/build_bot.bash --clean
./scripts/build_bot.bash debug
./scripts/build_bot.bash --clean debug
```

`test_bot.bash` builds and runs the individually discovered Catch2/CTest cases
in both debug and release by default. Pass `debug` or `release` to limit it to
one configuration.

For routine background operation, the helper scripts preserve the process ID
and stop the bot cleanly:

```bash
./scripts/start_bot.bash
./scripts/stop_bot.bash
```

`start_bot.bash` expects the release binary, the two default credential files,
and the three required scope IDs described above, unless their environment
overrides are set. It runs the offline configuration and exact-schema checks
before starting; it never runs a migration.
Console output goes to `logs/console.log`.

Discord gateway callbacks translate messages and interactions into
project-owned values. Messages enter a bounded 64-item application queue; a
single worker preserves log-before-routing order and keeps filesystem work out
of the gateway callback. Interactions apply inexpensive scope/authorization,
acknowledge ephemerally, and enter a separate bounded single-worker queue for
identity and notice database work. Saturated deferred interactions are edited
with an ephemeral overload response. When the message queue is full,
actionable commands or mentions receive the normal overload reply; ordinary
and bot-authored messages are dropped with a diagnostic to avoid public spam.

AI mentions use a separate two-thread pool with its own bounded 64-item queue.
Each OpenAI request has a connection timeout and an overall timeout, and
responses are truncated safely below Discord's message-size limit.
Reply-context lookup requires the bot's **Read Message History** permission. If
context retrieval fails, the bot still answers using the triggering message and
does not expose API credentials or raw API responses. Responses API requests
set `store` to `false` so generated responses are not retained for later
retrieval through the API.

The application owns all long-lived services. SIGINT or SIGTERM first detaches
Discord message and interaction intake, then stops the interaction, message,
and AI workers before shutting down D++. Pending work cancelled solely because
of shutdown does not emit a misleading failure reply. Callback fences wait for
already-running interaction, gateway, REST-delivery, and command-registration
callbacks while permanently suppressing late completions, so D++ teardown
cannot access destroyed application state. An interrupted notice reveal remains
pending and is recovered on the next startup.

The application enforces owner health requests through one reusable server
scope policy: configured guild, then primary channel, then owner identity.
Rejected requests generate no Discord response. Existing `!help`, `!repo`, and
leading-mention behavior deliberately retains its triggering-channel behavior
during the staged interaction migration.

## Architecture

`sanguinius_core` contains application services and project-owned interfaces;
it has no SQLite, D++, curl, or JSON dependency. `sanguinius_persistence`
contains the move-only SQLite RAII layer, transaction/migration/backup support,
and concrete core repositories. `sanguinius_runtime` contains the D++ gateway
and OpenAI adapters plus the production composition root. Tests use temporary
SQLite files and deterministic fakes, so ordinary CTest runs need no Discord or
OpenAI credentials and make no network calls.

## Message log

Each message is flushed as one line containing its receipt time in the
`America/New_York` timezone, the author's username, and the message text. The
timezone automatically follows daylight-saving transitions. Control characters
are escaped so one Discord message always occupies one physical line. The bot
restricts the message log to its owning user. Example:

```text
2026-07-17T10:22:05-04:00 author="user" message="hello\nworld"
```

The log contains user-generated content and usernames, and AI prompts send a
small amount of recent Discord conversation to OpenAI. Restrict access,
establish a retention policy, and disclose both practices to server members as
required by your policies and applicable law.
