# Sanguinius

Sanguinius is a Discord bot built with modern C++ and [D++](https://dpp.dev/).
It supports guild-scoped slash commands, sealed notices, an optional Living
Chronicle, and a persistent unsolicited-appearance candidate engine restricted
to inspection-only dry-run decisions. It
preserves two public prefix commands, answers messages that begin with a bot
mention through the OpenAI Responses API, and writes every visible guild
message-create event to an append-only text log. Typed configuration fixes the
bot's feature boundary to one guild, one primary text channel, and one owner.

## Commands

| Command | Description |
| --- | --- |
| `!help` | List the supported commands. |
| `!repo` | Link to this source repository. |

The configured guild receives command catalog version 6. Chronicle commands
are registered only when `SANGUINIUS_CHRONICLE_ENABLED=true`; owner commands
remain separately gated:

| Command | Visibility and behavior |
| --- | --- |
| `/sanguinius status` | Ephemeral readiness, feature-mode, and unopened-notice summary. |
| `/sanguinius inbox` | Ephemerally opens the oldest pending sealed notice. Duplicate Discord interaction IDs replay the same result. |
| `/sanguinius privacy` | Ephemeral identity/preference, voice-input, no-DM, and raw-voice-retention summary. |
| `/sanguinius appearance-callbacks <on\|off>` | Ephemerally opts the invoking member into or out of public appearance callbacks. This remains available while appearance evaluation is `off`. |
| **Canonize in the Chronicle** | Message context action that privately previews a bounded proposal, then requests sealed participant approval before canon. |
| `/chronicle remember` | Opens a modal and ephemeral confirm/cancel preview for an explicit memory, with up to five optional lowercase topic tags. Unconfirmed drafts are memory-only and disappear on restart. |
| `/chronicle recall [query] [participant] [type] [from] [to]` | Ephemerally searches visible canon with literal FTS5 terms and relational filters; explicit-memory matching remains privacy-checked. Results use invoker-bound five-item pages. |
| `/chronicle timeline [period]` | Publicly starts a shared-canon timeline for `7d`, `30d`, or `all`; later invoker-bound pages are ephemeral. |
| `/chronicle forget [reference]` | With a unique reference, retracts directly without components; otherwise ephemerally lists controllable records with short-lived controls. |
| `/chronicle profile [user]` | Shows qualitative relationship continuity ephemerally for self. Another member's profile is public and contains only shared canon counts/headings. |
| `/chronicle callbacks <on\|off>` | Ephemerally enables or disables relevant confirmed-memory callbacks. Enabling requires Chronicle opt-in; disabling is always allowed. |
| `/chronicle session start\|status\|close` | Opens, inspects, or closes one restart-safe guild session. Only the opener or owner can close it. |
| `/chronicle session edit\|approve\|reject` | Owner-only slash fallbacks for revision-fenced chapter draft review controls. Approval alone creates shared canon and a public card. |
| `/chronicle title propose\|list\|approve\|reject\|feature\|revoke` | Curates persistent title grants. Lists show five retained grants per page with the full mutation reference. Only the owner activates/rejects; recipients control their featured title and may revoke it. |
| `/chronicle anniversaries on\|off` | Controls Chronicle anniversary eligibility for the invoking opted-in member. |
| `/sang-admin health` | Ephemeral owner-only health; registered only when admin commands are enabled. |
| `/sang-admin work-recent` | Ephemeral owner-only inspection of the ten most recent redacted event/job/outbox summaries. |
| `/sang-admin work-dead` | Ephemeral owner-only inspection of the ten most recent dead jobs and failed/dead outbox rows. |
| `/sang-admin test-notice` | Owner-only, test-mode-gated durable queueing of a fixed, self-targeted 24-hour notice. |
| `/sang-admin test-schedule-notice` | Owner-only, test-mode-gated scheduling of the same self-targeted notice for 60 seconds later. |
| `/sang-admin test-public-retry` | Owner-only, test-mode-gated neutral card whose first attempt fails before Discord submission and then retries once. |
| `/sang-admin test-anniversary` | Owner-only, test-mode-gated exactly-once anniversary delivery using the newest eligible owner-test entry. |
| `/sang-admin appearance simulate fixture:<choice>` | Owner-only, test-mode and `dry_run`-gated creation of an idempotent sanitized candidate. Returns immediately with its reference. |
| `/sang-admin appearance preview reference:<uuid>` | Owner-only ephemeral inspection of a stored decision, including gates, score components, model status, shortened memory references, and any retained preview. Available in `off`. |
| `/sang-admin appearance recent` | Owner-only ephemeral inspection of the ten latest redacted decisions, with full references accepted by `preview`, and the appearance-public-outbox invariant. Available in `off`. |

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

Chronicle proposals retain only bounded source text and approved attachment
metadata—never attachment bytes, URLs, embeds, or surrounding history. Shared
canon and retraction cards are public; recall, memory previews, management,
and personal content are ephemeral. Participant-only entries and all explicit
memories produce no public content. Sensitive or personal memories are forced
to self-only visibility. Automatic AI callbacks are separately opt-in and can
use only confirmed, ordinary, shared memories whose sole user subject is the
requester. Relevant successful uses have a rolling seven-day per-memory
cooldown; failed or cancelled model calls do not consume it.

Open Chronicle sessions keep at most 20 opted-in primary-channel excerpts,
500 UTF-8 bytes each and 12 KiB total, solely as transient summary context.
The first retained excerpt creates durable expiry work; while a session remains
open, it removes each excerpt no later than 24 hours after observation and
reschedules itself for the next retained excerpt. Closing moves the same
cleanup boundary to 24 hours after close.
Closing freezes canon associations and always creates a deterministic fallback
draft. Structured model output may replace only the pending draft and propose
titles; deterministic validation and explicit owner approval control canon,
title activation, public delivery, and relationship effects. Transient context
is purged on approval/rejection or by persisted cleanup even when Chronicle
command access is disabled.

Appearance dry-run observes only the configured guild and primary channel
after messages enter the serialized application worker. It retains at most 24
activity rows, 500 UTF-8 bytes per row, and 12 KiB total, and purges activity
and copied candidate excerpts at immutable deadlines set by the policy active
when each row or candidate was created. A later policy cannot extend those
deadlines. Unrelated bots are ignored;
only human messages and Sanguinius's own output enter appearance activity.
Each candidate retains a prose-free summary after excerpt purge. Deterministic scope, expiry,
participation, quiet, consent, sensitivity, cooldown, and hypothetical-budget
gates run before optional structured AI classification and are rechecked after
it, together with current bounded channel activity and last-speaker state.
Serious-context phrase matches can only suppress. Decisions and redacted
audits persist; generated previews expire after 30 days.
Each full transient message is classified before its excerpt is truncated; the
bounded activity row retains only the category and up to 500 UTF-8 bytes. A
separate prose-free message-ID fence preserves gateway-delivery idempotency
after activity excerpts expire.
Each human must explicitly enable appearance callbacks before participating in
a real or simulated candidate. Chronicle-backed candidates additionally
revalidate the current shared source record and every source participant's
Chronicle and appearance consent before model preparation and again before a
final hypothetical decision. Conversation matches retain an authoritative
Chronicle-entry source link, and selected memories retain exact revision
references; revoked, changed, private, or unavailable sources fail closed on
restart and final revalidation.

Milestone 9 accepts only `off` and `dry_run`. There is no live mode, force or
trigger command, delivery dependency, or appearance budget reservation. The
v7 schema rejects any outbox insert whose kind or aggregate identifies an
appearance. A dry-run candidate can therefore produce only `reject` or
`hypothetical`, never a public Discord message. Activity retained under an
older policy version is never reused under a new policy, direct prefix and
leading-mention invocations are excluded from appearance activity, and events
observed while mode is `off` are audited without being replayed after a later
`dry_run` activation.

To talk to the AI persona, mention the bot at the start of a message:

```text
@sanguinius What is your favorite color?
```

The bot compiles immutable persona/privacy instructions, qualitative style,
feature state, up to three deterministically relevant confirmed memories, the
up to eight recent messages, and the replied-to message into separately labeled
layers. Quoted names, memories, and Discord history are untrusted data, never
instructions. Outside the configured primary channel, the same bounded
persona/history compiler runs without Chronicle reads, prompt audits,
callbacks, or relationship writes. The persona is defined in
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
- SQLite must include FTS5, and the C++ standard library must provide the IANA
  time-zone database used by `std::chrono`.
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
| `SANGUINIUS_APPEARANCE_POLICY_FILE` | `config/appearance-policy-v1.json` | Strict versioned appearance policy. Paths are redacted in configuration output. |
| `SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS` | `10` | Discord REST timeout, from 1 through 300 seconds. |
| `SANGUINIUS_TIMEZONE` | `America/New_York` | IANA time zone used for the daily 10:00 Chronicle anniversary scan. |
| `SANGUINIUS_DATABASE_FILE` | `state/sanguinius.sqlite3` | SQLite state file. Production should use an absolute path outside release directories. |
| `SANGUINIUS_ADMIN_COMMANDS_ENABLED` | `false` | Register owner slash controls and enable the transitional prefix health command. |
| `SANGUINIUS_TEST_MODE` | `false` | Enable auditable, self-targeted durable-work test controls. |
| `SANGUINIUS_CHRONICLE_ENABLED` | `false` | Register and enable the Living Chronicle context/slash flows. Durable memory expiry remains safe while UI access is disabled. |
| `SANGUINIUS_TAROT_ENABLED` | `false` | Configured Tarot mode; no Tarot behavior exists yet. |
| `SANGUINIUS_APPEARANCES_MODE` | `off` | Appearance engine mode. Only `off` and inspection-only `dry_run` are valid; `live` is rejected. |
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
./build/release/sanguinius db relationships check
./build/release/sanguinius db relationships rebuild --confirm
./build/release/sanguinius db backup /restricted/backup/sanguinius.sqlite3
```

`db status` is safe for absent, uninitialized, pending, current, and
incompatible databases; it prints an incompatible status but returns failure
so automation cannot mistake it for a usable schema. `db check` requires the
exact embedded schema and WAL mode. `db migrate` is the only command allowed to
create a database, enable WAL, or apply forward migrations; stop the bot first
because migration takes the exclusive database sidecar lock. Normal startup
never creates or upgrades the schema.
`db relationships check` reconstructs the relationship projection from its
append-only event chain and prints only counts/status. The guarded rebuild
replaces only that projection under the offline exclusive database lock and
verifies the result before committing.

Migration `0001_core_foundation` contains only shared identity and
configuration state: migration history, application instances, Discord users,
the one-guild scope, and user preferences. Migration
`0002_discord_interactions` adds pending notices, scoped opaque interaction
tokens, durable reveal-attempt/idempotency records, state/timestamp constraints,
and lookup/expiry indexes. A notice remains pending until Discord confirms its
private interaction response; failed or interrupted delivery leaves it
retrievable through the inbox. Migration `0003_durable_work` adds the immutable
structured event journal, fenced scheduled-job leases, and the transactional
outbox used by pending notices and public Discord messages. Durable payloads
are bounded valid JSON; Discord IDs remain canonical decimal text. Public-send
rows also persist a boot-session identifier and boot-relative elapsed time so
wall-clock correction cannot reopen the nonce retry window. Pending,
lease-expiry, aggregate-history, recent-event, and dead/failed inspection paths
have dedicated indexes, and event updates/deletes are rejected by triggers.
Migration `0004_chronicle` adds bounded Chronicle proposals, participants,
tags, attachment metadata, approvals, explicit memories/subjects, and
revision-fenced interaction tokens. It imports Chronicle consent only for
users already represented by `user_preference`; identities first seen later
remain opted out, and memory callbacks remain off. Source identity stays
unique after retraction, expiry jobs are durable, and private prose is absent
from journal payloads and public outbox rows.
Migration `0005_relationships` adds append-only bounded relationship events,
their rebuildable projection, and privacy-minimal AI prompt-attempt/memory-use
audits. It stores no compiled prompts, Discord context, memory-text copies, or
model output. Startup abandons prior-process reservations, catches up eligible
historical canon sources, and fails closed if projection verification detects
drift.
Migration `0006_chronicle_sessions` rebuilds Chronicle entry constraints for
approved session summaries and title awards, adds session/draft/title/search
and anniversary state, FTS5 synchronization triggers, transient context caps,
and the anniversary preference. It is forward-only; rollback restores a
verified schema-v5 backup and the accepted Milestone 7 artifact rather than
running reverse SQL.
Migration `0007_appearance_dry_run` adds immutable policy snapshots, bounded
policy-versioned message activity, non-prose channel counters for
retention-independent speech and post-hypothetical gates, persistently
mode-fenced event observations, candidates and source actors, revision-fenced
decisions, memory-use audit, separately purgeable previews, prose-free retained
summaries, and recurring scan/purge jobs. Activity and candidate-context rows
retain immutable expiry deadlines so policy upgrades cannot lengthen
previously collected prose retention.
Candidate sources include authoritative Chronicle-entry links in addition to
message, journal-event, and simulation inputs. It deliberately adds no appearance
budget reservation or public-delivery path. Its database trigger rejects
appearance-related inserts into `outbox_message`; rollback restores a verified
schema-v6 backup and the accepted Milestone 8 artifact rather than running
reverse SQL.
The readable SQL for each ordered migration is
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

Durable work uses a 32-item scheduler queue with one handler worker and a
32-item outbox queue with two delivery workers; those workers also cap Discord
REST submissions at two in flight. Polling is once per second, claims are made
inside worker tasks in batches no larger than 16, leases last 60 seconds, and
work defaults to five attempts with capped 5/10/20/40-second backoff. Queue
saturation leaves rows pending. Claims and state transitions use lease-token
fencing, so an expired worker cannot complete a reclaimed attempt. Readiness
loss before submission releases the claim without consuming an attempt. Once a
public Discord request is submitted, its fenced lease is extended through the
receipt-wait deadline plus a reconciliation margin so another worker cannot
reclaim an attempt whose callback is still legitimately in flight.
Delegated Chronicle summaries move to the front of the shared AI queue, extend
their fenced claim for the bounded delegation window, and release queued claims
without consuming an attempt during shutdown. When Chronicle is disabled,
persisted summary and anniversary work is deferred without consuming attempts;
privacy cleanup work continues to run.
Appearance classification/generation shares the same two-worker AI queue and
uses one strict structured attempt. Saturation, timeout, refusal, incomplete or
malformed output, unsafe text, low confidence, privacy changes, and interrupted
prior-instance attempts all become final audited rejections without fallback
prose or retry. The one-minute event scan and policy-bounded retention purge
(never slower than once per minute) are durable; retention continues while
appearance mode is `off`.
Unknown or malformed versioned handler types are retained and dead-lettered
with safe error categories. The current runtime additionally registers
Chronicle session/anniversary handlers and the appearance scan/purge handlers;
no appearance handler can enqueue public delivery.

Public outbox delivery uses one stable 25-character nonce with Discord's
`enforce_nonce` flag. Immediately before network I/O, the current fenced claim
records wall time, boot-relative elapsed time, the boot-session identifier, and
an in-flight submission marker; a confirmed provider message ID is persisted
and clears that marker. Transient or ambiguous outcomes retry with the same
nonce only inside a conservative 90-second boot-relative window. A reboot or
unverifiable elapsed-time relationship fails closed. If a callback is lost,
the surviving marker makes a reclaimed attempt explicitly ambiguous, and an
older attempt is quarantined as failed rather than resent. This quarantine also
takes precedence over ordinary retry exhaustion when the submitted attempt was
the row's final allowed attempt. Known public rows are not claimed until Discord
reports ready. Local pending-notice rows and unknown kinds remain claimable so
local effects can proceed and unsupported versions can dead-letter safely even
while the gateway is unavailable. Health includes both durable queue snapshots,
pending/claimed/dead/failed counts, retry totals, oldest-work lag, and the latest
safe error categories. Inspection and health never render payloads or notice
content.

AI mentions use a separate two-thread pool with its own bounded 64-item queue.
For primary-channel opted-in users, preparation and memory reservation are one
SQLite transaction keyed by the source Discord message. Only a successful
model call can transactionally update memory-use projections and append the
deterministic direct-interaction relationship event. Model output is never
accepted by a relationship or memory mutation API. Relationship dimensions,
thresholds, preferences, and internal IDs are absent from prompts and Discord
output; profiles render fixed qualitative phrases instead.
Each OpenAI request has a connection timeout and an overall timeout, and
responses are truncated safely below Discord's message-size limit.
Reply-context lookup requires the bot's **Read Message History** permission. If
context retrieval fails, the bot still answers using the triggering message and
does not expose API credentials or raw API responses. Responses API requests
set `store` to `false` so generated responses are not retained for later
retrieval through the API.

The application owns all long-lived services. SIGINT or SIGTERM first detaches
Discord message and interaction intake, then stops the interaction, message,
AI, scheduler, and outbox workers before shutting down D++. An acquired local
claim that has not started is released without consuming an attempt. A request
already handed to Discord remains fenced and is reconciled after lease expiry
with the same nonce policy. Pending work cancelled solely because of shutdown
does not emit a misleading failure reply. Callback fences wait for
already-running interaction, gateway, REST-delivery, and command-registration
callbacks while permanently suppressing late completions, so D++ teardown
cannot access destroyed application state. Interrupted notice reveals and
durable work are recovered on the next startup.

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

The log contains user-generated content and usernames. AI prompts send bounded
recent/replied Discord context to OpenAI and, only when the requester has opted
in, may also send a small selection of relevant confirmed ordinary shared
memories. Restrict access, establish a retention policy, and disclose these
practices to server members as required by your policies and applicable law.
