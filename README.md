# Sanguinius

Sanguinius is a C++20/D++ Discord bot for one private guild. It combines a
Living Chronicle, persistent relationship continuity, an original Emperor's
Tarot and Fate economy, rare contextual appearances, and Vox voice
participation. Authoritative state is deterministic and persisted in SQLite;
AI supplies bounded prose, never permissions, money, or state transitions.

Current release contract:

- version 2.2.0;
- database schema 16;
- guild command catalog 16;
- D++ 10.1.7 with voice and DAVE support;
- target-compatible production packages capped at x86-64-v3.

## Features

- Chronicle: explicit canon, memories, sessions, reviewed summaries, titles,
  FTS search, anniversaries, callbacks, and private relationship profiles.
- Tarot: immutable double-entry Fate ledger, recovery, draws, peer wagers,
  disputes, House offers, history, standings, and deterministic integration.
- Appearances: off, dry-run, and conservative live modes with deterministic
  hard gates, quiet controls, budgets, consent, feedback, and audit.
- Vox: summon/leave/status, generated speech, approved static fallbacks,
  bounded cache, mute, Chronicle/Tarot/appearance narration, and optional
  consent-attested 5/10/15-second transcription windows.
- Reliability: bounded workers, persistent provider circuits and budgets,
  durable scheduler/outbox, unknown-delivery quarantine, redacted health,
  systemd readiness/watchdog, online backup, and migration-aware rollback.

The bot uses no Discord direct messages. Private results are ephemeral
interaction responses or sealed notices retrieved through the primary channel.

## Member commands

Commands are guild-scoped and accepted only in the configured primary text
channel unless documented otherwise. Member help is generated from the active
catalog and omits owner/admin/test operations.

### Discovery and preferences

| Command | Purpose |
| --- | --- |
| /help [topic] | Ephemeral feature-aware guide. |
| /repo | Ephemeral source-repository card. |
| /sanguinius status | Readiness, feature availability, quiet/mute, and unopened-notice summary. |
| /sanguinius inbox | Open the oldest pending sealed notice ephemerally. |
| /sanguinius privacy | Explain stored data, visibility, voice consent, and controls. |
| /sanguinius appearance-callbacks on or off | Control personal callback eligibility. |
| /sanguinius appearance-feedback | Privately rate an eligible delivered appearance. |
| /sanguinius quiet | Start, extend, or end server-wide quiet; inspect it through status. |

Leading-mention messages use the bounded text-AI path. Exact legacy help/repo
prefix messages are silent; the prefix remains only for the emergency owner
health fallback when enabled.

### Chronicle

| Command | Purpose |
| --- | --- |
| Canonize in the Chronicle | Message context action that starts the approval-gated canon path. |
| /chronicle remember | Create and confirm an explicit memory proposal. |
| /chronicle recall | Search visible canon with literal terms and filters. |
| /chronicle timeline | Show a bounded shared-canon timeline. |
| /chronicle forget | Retract an authorized memory or entry. |
| /chronicle profile | Show private self continuity or a sanitized public member view. |
| /chronicle callbacks | Enable or disable relevant confirmed-memory callbacks. |
| /chronicle session | Members start or inspect sessions; only the opener/owner closes, and edit/approve/reject are owner-admin operations. |
| /chronicle title | Members list titles; recipients feature their own and recipients/owner revoke, while propose/approve/reject are owner-admin operations. |
| /chronicle anniversaries | Control personal anniversary eligibility. |

### Emperor's Tarot

| Command | Purpose |
| --- | --- |
| /tarot balance | Private balance derived from immutable postings. |
| /tarot history | Private paginated ledger history. |
| /tarot standings | Public standings for opted-in accounts. |
| /tarot standings-visibility | Change standings participation. |
| /tarot grace | Guaranteed low-balance recovery when eligible. |
| /tarot trial | Deterministic persisted recovery vows and reward. |
| /tarot draw | Persist and reveal one original deck draw. |
| /tarot wager | Preview and confirm an equal-stake public or sealed peer wager. |
| /tarot wagers | Participant-only list/detail and current controls. |
| /tarot wager-action | Slash fallback for accept/decline/cancel/agree/dispute/void. |
| /tarot outcome | Submit a participant outcome. |
| /tarot evidence | Append private evidence. |
| /tarot judgment | Authorized named-judge or owner decision with reason. |
| /tarot disputes | Show authority-filtered disputes. |
| /tarot house offers | List currently claimable House auguries. |
| /tarot house play | Atomically fund one selected House offer. |
| /tarot house history | Show only the invoking member's House history. |
| /tarot record | Show personal wins, losses, streaks, and related titles. |

Fate has no cash value. Balances may not become negative, all transfers are
double-entry, and wager escrow is atomic and idempotent.

### Vox

| Command | Purpose |
| --- | --- |
| /vox summon | Join the invoker's ordinary voice channel and establish a DAVE session. |
| /vox status | Public-safe session, mute, static-proof, reconnect, elapsed-time, selected-voice, speech-availability, and queue state. |
| /vox say | Owner-requested bounded generated speech. |
| /vox mute | Summoner/owner timed or session mute while preserving connection. |
| /vox voice | Any member inspects the voice; the owner selects an allowed voice. |
| /vox listen-start | Start an optional 5/10/15-second consent-attested anonymous mix window. |
| /vox listen-stop | Immediately stop the active/arming listening window. |
| /vox leave | Leave for the summoner or owner. |

Voice input is disabled by default. It requires Vox, exact OpenAI
gpt-transcribe capability, explicit owner configuration attesting prior consent
for every present and later-joining human, a confirmed public indicator, and
the requester's presence. Raw received PCM is never persisted. A transcript is
returned only through the requester's ephemeral interaction and requires a
separate approval flow before Chronicle use.

## Owner safety and diagnostics

The owner-only /sang-admin safety status/set group remains available when the
broader admin catalog is disabled. It controls appearances, text AI, TTS, Vox
output, and voice input. Enabling clears only the operator kill; it cannot
override configuration, capability, consent, budget, or circuit state.

The remaining owner diagnostics and deterministic test controls require
SANGUINIUS_ADMIN_COMMANDS_ENABLED=true, and test mutation additionally requires
SANGUINIUS_TEST_MODE=true. Both default to false and should remain false in
ordinary production.

## Requirements

- Linux on x86-64
- CMake 3.25 or newer and Ninja
- a C++20 compiler
- D++ 10.1.7 or newer built as a shared library with voice, Opus, OpenSSL,
  zlib, and integrated DAVE/MLS
- libcurl and OpenSSL libcrypto
- nlohmann-json
- SQLite 3.51.3 or later, or the fixed 3.50.7/3.44.6 backport, with FTS5
- FFmpeg and FFprobe 9.x for TTS normalization
- Catch2 v3 when BUILD_TESTING is enabled
- a C++ standard library with the IANA time-zone database

CMake compiles narrow D++ voice and receive probes. Missing output voice is a
configuration error; missing receive support builds a disabled input adapter
while preserving text and output-only Vox.

The Discord application needs Guilds, Guild Messages, Message Content, and,
when Vox is enabled, Guild Voice States intents. Voice channels require View
Channel, Connect, and Speak; a full channel also requires Move Members. Stage
channels are rejected.

## Configure

Install and edit the complete safe template:

    umask 077
    install -m 0600 config/sanguinius.env.example /secure/path/sanguinius.env

Required identities are canonical nonzero decimal snowflakes:

- SANGUINIUS_GUILD_ID
- SANGUINIUS_PRIMARY_CHANNEL_ID
- SANGUINIUS_OWNER_USER_ID

Keep the Discord token and OpenAI API key in mode-0600 files and configure:

- SANGUINIUS_TOKEN_FILE
- SANGUINIUS_OPENAI_API_KEY_FILE

Do not commit secrets or environment files. ChatGPT subscriptions do not fund
OpenAI API usage. Configure API billing/limits separately and verify current
official model pricing before setting the required micro-USD rates.

Safe public defaults keep Chronicle, Tarot, appearances, Vox, TTS, and voice
input disabled until deliberately configured. The example lists every feature,
path, rate, budget, timeout, and cache setting with no real identity or secret.
Boolean values accept only lowercase true or false.

The application reads the process environment; it does not parse this file.
After confirming the file is owned by the operator and mode 0600, load only
this trusted, operator-authored shell input into the current shell:

    test -O /secure/path/sanguinius.env
    test "$(stat -c '%a' /secure/path/sanguinius.env)" = 600
    set -a
    . /secure/path/sanguinius.env
    set +a

Validate without constructing D++, contacting Discord/OpenAI, or printing
secrets, IDs, paths, prompts, or file contents:

    set -a
    . /secure/path/sanguinius.env
    set +a
    ./build/debug/sanguinius --check-config

## Build and test

    cmake --preset debug
    cmake --build --preset debug
    ctest --preset debug

Ordinary tests use deterministic fakes and temporary SQLite files; they require
no Discord, OpenAI, SSH, credentials, or network.

Build presets also support Release and sanitizer workflows. Use one build per
revision unless a distinct toolchain/sanitizer risk requires another.

After providing secure configuration, run the Debug binary directly for a
foreground development session:

    set -a
    . /secure/path/sanguinius.env
    set +a
    ./build/debug/sanguinius

Alternatively, use the background helper with a Release build instead of the
Debug workflow above:

    ./scripts/build_bot.bash release
    set -a
    . /secure/path/sanguinius.env
    set +a
    ./scripts/start_bot.bash
    ./scripts/stop_bot.bash

The helper uses `build/release/sanguinius`, validates configuration and the
exact database schema, and never runs a migration.

## Database maintenance

Database commands construct neither D++ nor provider clients and use only
SANGUINIUS_DATABASE_FILE:

    ./build/debug/sanguinius db status
    ./build/debug/sanguinius db migrate
    ./build/debug/sanguinius db check
    ./build/debug/sanguinius db integrity
    ./build/debug/sanguinius db relationships check
    ./build/debug/sanguinius db tarot check
    ./build/debug/sanguinius db invariants check
    ./build/debug/sanguinius db backup /restricted/path/backup.sqlite3

Normal startup never creates or upgrades a database. db migrate is the only
creation/upgrade path and requires exclusive access. Existing migration files
are immutable checksummed inputs; add a new ordered migration for every schema
change.

Guarded rebuild commands exist only for rebuildable relationship/Tarot/FTS
projections and require the service inactive. The immutable event, Chronicle,
ledger, wager, safety, and delivery histories are not rebuilt or rewritten.

## Production packaging and operation

The release tool builds in a digest-pinned target-compatible Arch environment,
runs Release tests, and emits an immutable rooted tar.zst archive with release
metadata, a complete SHA-256 payload manifest, and an external archive hash:

    ./scripts/release.bash image
    ./scripts/release.bash package
    ./scripts/release.bash verify --archive dist/sanguinius-<release-id>.tar.zst

Deployment verifies the archive and target ABI, stages a new immutable release,
creates a verified online backup, rehearses migrations and rollback on
disposable copies, switches symlinks atomically, waits for systemd READY and
catalog synchronization, and retains only recognized safe artifacts.

See [Production operations](docs/OPERATIONS.md) for bootstrap, deployment,
backup, restore, rollback, incidents, readiness, resources, and production
safety flags.

## Architecture

sanguinius_core contains domain/application services, project-owned
interfaces, shared JSON/OpenSSL support, and the libsystemd service-notifier
adapter. sanguinius_persistence owns SQLite and migrations.
sanguinius_runtime owns D++, provider HTTP clients, media, and application
composition.

Committed cross-feature work flows through the immutable event journal and
feature-owned idempotent observers. Scheduled jobs and public delivery are
durable. Gateway callbacks submit bounded work and never wait on providers or
the database.

All provider attempts use bounded admission, persistent circuits, conservative
unknown handling, and sanitized operational metadata. Prompts, responses,
transcripts, private payloads, credentials, headers, and provider bodies are
excluded from durable diagnostics.

## Message log and privacy

Each visible guild message-create event is appended as one escaped line with
receipt time, username, and message text. This log is separate from the
Chronicle and is not imported automatically.

The log and database contain user-generated content and durable social state.
Restrict access, retain only as agreed by server members, disclose provider
processing, and use the supported retraction/privacy controls. Never expose
these files through release archives, health output, or routine diagnostics.
