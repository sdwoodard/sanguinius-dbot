# Sanguinius

Sanguinius is a Discord bot built with modern C++ and [D++](https://dpp.dev/).
It supports guild-scoped slash commands, sealed notices, an optional Living
Chronicle, a persistent original Emperor's Tarot and Fate economy, and
persistent unsolicited appearances with off, dry-run, and conservatively
budgeted live modes, plus feature-flagged output-only Discord voice sessions
with owner-requested, budgeted TTS, approved static fallbacks, and optional
post-commit Chronicle, Tarot, and appearance narration. It exposes member help
and repository information through root slash commands, answers messages that
begin with a bot mention through the OpenAI Responses API, and writes every
visible guild message-create event to an append-only text log. Typed
configuration fixes the bot's feature boundary to one guild, one primary text
channel, and one owner.

## Commands

| Command | Description |
| --- | --- |
| `/help [topic]` | Show an ephemeral, feature-aware member command guide. Owner, admin, debug, and test operations are always omitted. |
| `/repo` | Show the source repository ephemerally. |

The configured guild receives command catalog version 15. Chronicle, Tarot,
and Vox
commands are registered only when their corresponding feature flag is enabled;
owner commands remain separately gated:

| Command | Visibility and behavior |
| --- | --- |
| `/sanguinius status` | Ephemeral readiness, feature-mode, and unopened-notice summary. |
| `/sanguinius inbox` | Ephemerally opens the oldest pending sealed notice. Duplicate Discord interaction IDs replay the same result. |
| `/sanguinius privacy` | Ephemeral identity/preference, voice-input, no-DM, and raw-voice-retention summary. |
| `/sanguinius appearance-callbacks <on\|off>` | Ephemerally opts the invoking member into or out of public appearance callbacks. This remains available while appearance evaluation is `off`. |
| `/sanguinius appearance-feedback response:<more\|less\|not_relevant> [reference]` | Privately records sentiment for an exact delivered appearance. Without a reference, it selects the latest eligible delivery. |
| `/sanguinius quiet for duration:2h` | Starts or extends server-wide appearance and automatic Vox-narration quiet for two hours. |
| `/sanguinius quiet tonight` | Starts or extends server-wide quiet until 10:00 AM on the following `America/New_York` calendar day. |
| `/sanguinius quiet until time:HH:MM` | Starts or extends server-wide quiet to the next valid local occurrence within 24 hours. |
| `/sanguinius quiet off` | Ends quiet early only for its latest setter or the owner. |
| **Canonize in the Chronicle** | Message context action that privately previews a bounded proposal. Bot-authored sources must be an exact delivered appearance; canon still requires the ordinary approval path. |
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
| `/tarot balance` | Ephemerally shows the invoking member's current Fate balance from the immutable ledger. |
| `/tarot history` | Ephemerally shows up to 50 immutable ledger entries in invoker-bound five-item pages. |
| `/tarot standings` | Publicly ranks opted-in participants by Fate balance and canonical user ID. |
| `/tarot standings-visibility public\|private` | Ephemerally changes whether the invoking member appears in public standings without changing the ledger. |
| `/tarot grace [visibility:public\|private]` | Ephemerally prepares Grace of the Throne when eligible; successful public claims enqueue only neutral flavor. |
| `/tarot trial [visibility:public\|private]` | Ephemerally prepares three persisted Renewal vows plus Abandon; any vow awards the preselected reward exactly once. |
| `/tarot draw [visibility:public\|private]` | Persists one deterministic draw from the versioned 22-card original deck before returning it. Ordinary draws share a 24-hour cooldown; retries replay the exact card and flavor. |
| `/tarot house offers` | Ephemerally lists currently claimable House auguries without exposing another member's terms. |
| `/tarot house play template:<template> choice:<choice> stake:<0\|1\|5\|10> [visibility] [offer]` | Privately checks eligibility and atomically funds one one-person augury. Public visibility affects only bounded flavor; balances, stake, odds, and payouts stay ephemeral. |
| `/tarot house history [reference]` | Shows only the invoking member's private House history or exact authorized wager. |
| `/tarot record` | Ephemerally shows the invoking member's wins, losses, streaks, House count, and pending title proposals. |
| `/tarot wager target:<user> [visibility] [resolution] [judge] [outcome-in]` | Starts the ephemeral equal-stake peer-wager form and immutable preview. Confirmation creates a public offer or a sealed target-bound notice. |
| `/tarot wagers [reference]` | Shows participant-only five-item history or exact private details with current role-authorized controls. |
| `/tarot wager-action reference:<uuid> action:<accept\|decline\|cancel\|agree\|dispute\|void>` | Slash fallback for wager buttons. Acceptance atomically funds both equal stakes. |
| `/tarot outcome reference:<uuid> winner:<creator\|target>` | Records one participant's mutual-resolution submission; the equivalent **Submit outcome** button opens a bounded ephemeral winner modal. |
| `/tarot evidence reference:<uuid> evidence:<text>` | Appends immutable private participant evidence. |
| `/tarot judgment reference:<uuid> result:<creator\|target\|void> reason:<text>` | Allows the designated judge before dispute or the owner after dispute to enter a bounded reasoned result. |
| `/tarot disputes [reference]` | Shows only disputes visible to a participant or the owner. |
| `/vox summon` | Ephemerally resolves the invoker's current ordinary voice channel, persists one output-only session, connects with required DAVE/E2EE, and plays one safely prepared contextual entrance when ready in time or the approved static entrance otherwise. |
| `/vox status` | Publicly reports only the active state, voice-channel mention, elapsed seconds, reconnect count, static-proof state, selected voice, mute state, bounded queue depth, and speech-service availability. |
| `/vox say text:<line>` | Owner-only ephemeral admission of a normalized, budgeted generated line while Vox is ready or muted. Distinct interactions remain distinct queue items; cache reuse consumes no provider budget. |
| `/vox mute duration:<15m\|1h\|4h\|session\|off>` | Owner or active summoner control that preserves the connection, durably expires timed mute, blocks automatic/flavor speech, and still permits direct owner speech. |
| `/vox voice [voice:onyx]` | Any in-scope member may inspect the selected voice ephemerally; only the owner may change it to the configured allowlist. |
| `/vox listen-start duration:<5\|10\|15>` | Starts one disabled-by-default, public-indicated anonymous channel-mix window only while Vox is ready, the requester is present, consent is attested, TTS is idle, and exact `gpt-transcribe` capability is ready. The final transcript or sanitized failure edits only the deferred ephemeral response. |
| `/vox listen-stop` | Any in-scope guild human may immediately abort the active/arming window; buffered audio is scrubbed and no partial transcript is produced. |
| `/sang-admin vox speech-test scenario:<queue\|provider-failure\|budget-limit\|narration-stale>` | Owner-only, admin/test-mode deterministic queue, fallback, and stale-restart acceptance scenarios. Simulated failures never call the live provider. |
| `/sang-admin vox narration-preview reference:<event UUID>` | Owner-only ephemeral inspection of one fresh, public-safe narration projection. It performs no TTS and consumes no session budget. |
| `/sang-admin vox narration-enqueue reference:<event UUID>` | Owner-only, test-mode-gated durable observation of an explicitly test-tagged fresh event; every ordinary visibility, counterpart, mute, quiet, budget, expiry, and deduplication gate still applies. |
| `/sang-admin vox narration-recent` | Owner-only last-ten sanitized narration states and reasons, without generated line content. |
| `/vox leave` | Ephemerally dismisses the active session for its summoner or the configured owner. |
| `/sang-admin safety status` | Owner-only, public-safe view of the appearances, text AI, TTS, Vox-output, and voice-input operator kills. This safety group remains registered when the broader admin catalog is disabled. |
| `/sang-admin safety set target:<appearances\|text-ai\|tts\|vox-output\|voice-input> mode:<enabled\|disabled>` | Owner-only durable safety control. Disable preempts the corresponding work; enable clears only the operator kill and cannot bypass configuration, budget, capability, or circuit state. |
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
| `/sang-admin appearance trigger fixture:owner_live_safe` | Owner-only, admin/test-mode-gated, visibly tagged one-person live delivery through the normal transaction and outbox. Configured mode must be `live`. |
| `/sang-admin tarot adjust amount:<integer> reason:<text>` | Owner-only, admin/test-mode-gated balanced adjustment of the owner's account. |
| `/sang-admin tarot reverse transaction:<uuid> reason:<text>` | Owner-only, admin/test-mode-gated exact reversal of one eligible unreversed `TEST_ADJUSTMENT`; wager transfers require terminal wager cleanup. |
| `/sang-admin tarot wager-role reference:<uuid> role:<creator\|target\|judge\|owner>` | Owner-only, test-mode-gated selection of one simulated role for a self-wager. |
| `/sang-admin tarot wager-deadline reference:<uuid> phase:<draft\|offer\|reminder\|outcome\|grace>` | Owner-only, test-mode-gated forcing of one persisted deadline phase. |
| `/sang-admin tarot wager-cleanup reference:<uuid> reason:<text>` | Owner-only, test-mode-gated exact reversal of every terminal test-wager transfer; audit rows are retained. |
| `/sang-admin tarot economy` | Owner-only read-only Fate issuance, escrow, House exposure, and invariant health without member balances or wager terms. |
| `/sang-admin tarot draw-test [visibility]` | Owner-only, admin/test-mode-gated persisted test draw that bypasses the ordinary cooldown. |
| `/sang-admin tarot draw-replay reference:<uuid>` | Owner-only read-only replay of the caller's exact persisted draw without sampling again. |
| `/sang-admin tarot house-offer` | Owner-only, admin/test-mode-gated deterministic public test offer for the next Last Standard slot. |
| `/sang-admin tarot house-resolve reference:<uuid> outcome:<yes\|no\|void> reason:<text>` | Owner-only reasoned observation for Last Standard. Real manual wagers may be resolved without test mode; other mutations remain test-gated. |
| `/sang-admin tarot house-deadline` | Owner-only, admin/test-mode-gated processing of due test House wagers. |
| `/sang-admin tarot house-cleanup reference:<uuid> reason:<text>` | Owner-only, admin/test-mode-gated exact reversal of terminal test House transfers while retaining audit. |
| `/sang-admin tarot integration-preview` | Owner-only redacted inspection of pending or failed Tarot integration work. |
| `/sang-admin tarot integration-retry reference:<uuid>` | Owner-only, admin/test-mode-gated retry of one failed test observation. |
| `/sang-admin vox disconnect` | Owner-only, admin/test-mode-gated one-shot reconnect exercise; it never replays the proof chime. |

Command names are case-insensitive. Messages written by bots are logged but are
not treated as commands.

When owner administration is explicitly enabled, the transitional
`!sang-admin health` command continues to provide a public but strictly
redacted health snapshot in the configured primary channel. It is not listed
by `/help` and is silent for other users or channels. Prefer the ephemeral
slash-command form for routine use.

The retired exact messages `!help` and `!repo` are silent and have no redirect
aliases. They are excluded from unsolicited-appearance observation. The
leading-mention AI behavior and append-only message log remain unchanged.

Feature interactions are accepted only in the configured guild and primary
channel. Ordinary slash responses are ephemeral. Discord cannot proactively
send an arbitrary user an ephemeral message, and Sanguinius never uses DMs;
private proactive content is stored as a pending notice. A neutral public card
may mention only its target and contains no private title/body or database
identifier. The target retrieves the content by clicking its opaque,
expiring button or by running `/sanguinius inbox`.

Peer wagers use one immutable equal stake of 1–100 Fate per participant; M12
does not support asymmetric stakes, counteroffers, pools, House odds, or AI
judgment. A confirmed offer expires unfunded after 24 hours by default.
Acceptance debits both participants and credits `ESCROW` in one `BEGIN
IMMEDIATE` transaction. Matching mutual outcomes pay the complete escrow once;
conflicting outcomes or the 48-hour post-deadline grace boundary dispute the
wager without moving Fate. Both participants may instead consent to an exact
refund. A designated judge may decide only before dispute; once disputed, only
participant agreement, mutual void, or a reasoned owner judgment can settle it.
Mutual participants may submit through an opaque, revision-bound **Submit
outcome** control and bounded modal or use the slash fallback. Designated
self-test wagers pin the simulated judge to the owner, so an external member is
never named as a judge who cannot exercise that role.
Scheduler retries, duplicate interactions, and restarts replay durable state and
never award Fate by timeout.
The offer duration, outcome window, and resolution-grace duration are
snapshotted before the offer is confirmed, so a configuration change cannot
alter previewed or accepted terms.
Authorized ephemeral responses and participant history expose the complete UUID
needed by slash fallbacks; public cards use only public-safe shortened labels.
An offered sealed wager is omitted from the target's participant history, and
exact lookup fails closed, until the target's sealed notice has actually been
delivered. Reserving or failing an ephemeral reveal does not unlock the terms.
Sealed acceptance uses the same delivery fence and moves no Fate until the
target has successfully received the complete offer.

The curated `emperor-tarot-v1` deck contains 22 original cards with no
reversals or conventional Tarot names. Catalog snapshots and every draw are
immutable; reusing a version with different canonical content is a startup
error. Card meanings are flavor only and never determine Fate movement,
permissions, or House outcomes.

House play is one member against deterministic system accounts, never a pool.
Returning Dawn is a zero-stake recovery augury; Herald's Call and Final Hour
resolve from persisted public draws or deadlines; Last Standard is offered at
18:00 Friday in `America/New_York` and records whether game-night play remains
underway at midnight Saturday. Missed, quiet, degraded, duplicate, or
exposure-blocked weekly slots are skipped rather than caught up. Its public
reminder and linked unclaimed-offer expiry are durable and idempotent; expiry
cancels the opaque control and releases reserved exposure. Funding
collateralizes the exact integral profit in the same `BEGIN IMMEDIATE`
transaction as the user's stake. Non-test open House profit exposure is capped
at 100 Fate globally and profit at 20 Fate per wager. A win, loss, void, retry,
or restart always settles the same escrow once. Fate has no purchase, cash,
cryptocurrency, or other real-money value.

Public House cards contain only neutral proposition/status text and opaque
controls. Eligibility, balances, selected stakes, odds, payouts, records,
recovery status, sealed peer terms, and hidden relationship values remain
ephemeral. Post-settlement Tarot observations are source/sink-idempotent:
notable public events may submit Chronicle proposals, relationship events,
appearance candidates, title proposals, and expiring text-only future Vox
intents. They never create canon, activate titles, or deliver voice directly.

Public offers show the proposition, equal stake, participants, resolution
method, deadlines, and public-safe status. Sealed public cards contain only a
neutral target-bound prompt and later neutral states; proposition, stake,
evidence, judge, and result remain in ephemeral notices and, after the initial
reveal, participant history. The original card is updated through a revisioned
durable message-edit outbox and loses its controls after the state changes.

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

The appearance engine observes only the configured guild and primary channel
after messages enter the serialized application worker. It retains at most 24
activity rows, 500 UTF-8 bytes per row, and 12 KiB total, and purges activity
and copied candidate excerpts at immutable deadlines set by the policy active
when each row or candidate was created. A later policy cannot extend those
deadlines. Unrelated bots are ignored;
only human messages and Sanguinius's own output enter appearance activity.
Each candidate retains a prose-free summary after excerpt purge. Deterministic scope, expiry,
participation, quiet, consent, sensitivity, cooldown, and reservation-budget
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
final decision. Conversation matches retain an authoritative
Chronicle-entry source link, and selected memories retain exact revision
references; revoked, changed, private, or unavailable sources fail closed on
restart and final revalidation.

Modes are `off`, `dry_run`, and `live`; changing mode creates a persistent
epoch, so candidates cannot cross an activation boundary. Dry-run can produce
only `reject` or `hypothetical` and the schema still proves it cannot create a
public appearance outbox row. In live mode, one `BEGIN IMMEDIATE` finalization
transaction repeats every gate, reserves the conservative automatic or
isolated owner-test budget, records the decision and used-memory references,
persists immutable opt-out dependencies for original and final-window active
humans, creates four opaque feedback controls, and inserts exactly one causally
linked primary-channel public outbox row. Competing candidates serialize, and
the reservation is consumed once that transaction commits even if delivery
later fails or becomes ambiguous.

Automatic live policy remains fixed at one appearance per rolling 24 hours, a
90-minute gap, eight intervening human messages, two opted-in active humans, a
seven-day theme cooldown, and a 30-day used-memory cooldown. Public text is one
validated line of at most 500 Unicode code points with no allowed mentions.
Every live line offers private `More like this`, `Less like this`, `Not
relevant`, and `Quiet for tonight` controls. Quiet is server-wide: any member
may start or extend it, but only its latest setter or the owner may clear it
early. The owner kill switch and opt-out/retraction paths cancel pending or
claimed-but-never-submitted appearance rows; already submitted rows, including
retry-pending unknown outcomes, remain fenced for normal reconciliation.

Delivery reuses the transactional outbox's stable nonce, enforced Discord
deduplication, fenced leases, provider message-ID receipt, safe retry window,
and stale-unknown quarantine. The candidate/model worker never sends directly.
An exact delivered appearance can be proposed through **Canonize in the
Chronicle**, but only its public text is copied and the ordinary explicit
approval path still governs canon. Feedback changes only deterministic counts
and recommendations; it never rewrites policy automatically. Activity retained
under an older policy version is never reused under a new policy, direct prefix
and leading-mention invocations are excluded, and events observed while mode is
`off` are audited without later replay.

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
- D++ 10.1.7 or newer with shared-library voice, Opus, OpenSSL, zlib, and
  integrated DAVE/MLS support enabled
- libcurl
- OpenSSL libcrypto (for SHA-256 cache and asset verification)
- FFmpeg and FFprobe 9.x at fixed absolute paths for bounded WAV inspection and
  conversion to 48 kHz signed 16-bit stereo PCM; libav is not linked
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
CMake compiles and links a configure-time probe for the Guild Voice States
intent, DAVE-enabled `connect_voice`, E2EE status, raw PCM send, marker, and
disconnect APIs. Configuration fails with a voice-support diagnostic when the
installed package cannot supply that surface. Milestone 15 adds fixed-argument
FFmpeg/FFprobe child processes; provider text is never placed in argv or a
temporary media filename.
Milestone 17 performs a separate nonfatal compile probe for D++'s combined
receive callback. A failed receive probe builds the disabled adapter and leaves
all output-only Vox behavior available.

Catch2 is a development-only dependency. CMake requires it only when
`BUILD_TESTING=ON`; a prebuilt production executable does not require Catch2 on
the runtime host.

The Discord application must have the **Message Content Intent** enabled in the
Developer Portal. The gateway always requests guild, guild-message, and message
content intents and never requests direct-message intents. When
`SANGUINIUS_VOX_ENABLED=true`, it additionally requests **Guild Voice States**;
when Vox is disabled that intent and all voice callbacks are omitted. The bot
needs the ordinary text permissions plus effective **View Channel**,
**Connect**, and **Speak** permissions in the invoker's current ordinary voice
channel. A full channel additionally requires **Move Members**. Stage channels
are rejected. Output-only voice still requires bidirectional UDP reachability.

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

The default model is `gpt-5.6-luna`, selected for low API cost. It does not have
a free API tier. Review its [current model pricing and rate
limits](https://developers.openai.com/api/docs/models/gpt-5.6-luna) and set usage
limits in the OpenAI platform before running the bot. The example configuration
records the [official standard short-context
rates](https://developers.openai.com/api/docs/pricing) verified on
2026-08-27—$0.20 input and $1.20 output per million tokens. Re-audit whenever
the model, service tier, region, or context class changes. Process defaults
remain zero so an omitted rate always fails closed.

Runtime settings are:

| Variable | Default | Purpose |
| --- | --- | --- |
| `SANGUINIUS_LOG_FILE` | `logs/messages.log` | Message log path. |
| `SANGUINIUS_COMMAND_PREFIX` | `!` | Prefix (1–8 non-space characters) retained only for the emergency `!sang-admin health` fallback. |
| `SANGUINIUS_OPENAI_API_KEY_FILE` | Set by start script | OpenAI API key file. |
| `SANGUINIUS_OPENAI_MODEL` | `gpt-5.6-luna` | Responses API model. |
| `SANGUINIUS_OPENAI_INPUT_MICRO_USD_PER_MILLION_TOKENS` | `0` | Audited model input rate. Must be explicitly set above zero for text generation; unknown/unpriced models fail closed. |
| `SANGUINIUS_OPENAI_OUTPUT_MICRO_USD_PER_MILLION_TOKENS` | `0` | Audited model output rate. Must be explicitly set above zero for text generation; prompts and responses are never stored in budget rows. |
| `SANGUINIUS_PERSONA_FILE` | `config/persona.txt` | Plaintext persona instructions. |
| `SANGUINIUS_APPEARANCE_POLICY_FILE` | `config/appearance-policy-v2.json` | Strict versioned appearance policy with the `tarot_event` candidate family. Paths are redacted in configuration output. |
| `SANGUINIUS_TAROT_DECK_FILE` | `config/emperor-tarot-v1.json` | Strict versioned original deck catalog. Configuration output reports only its origin and version. |
| `SANGUINIUS_TAROT_HOUSE_FILE` | `config/tarot-house-v1.json` | Strict versioned House-template catalog. Configuration output reports only its origin and version. |
| `SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS` | `10` | Discord REST timeout, from 1 through 300 seconds. |
| `SANGUINIUS_TIMEZONE` | `America/New_York` | IANA time zone used for the daily 10:00 Chronicle anniversary scan. |
| `SANGUINIUS_DATABASE_FILE` | `state/sanguinius.sqlite3` | SQLite state file. Production should use an absolute path outside release directories. |
| `SANGUINIUS_ADMIN_COMMANDS_ENABLED` | `false` | Register the full owner diagnostic/test catalog and enable transitional prefix health. Unified owner-only `/sang-admin safety` controls remain available when this is `false`. |
| `SANGUINIUS_TEST_MODE` | `false` | Enable auditable, self-targeted durable-work test controls. |
| `SANGUINIUS_CHRONICLE_ENABLED` | `false` | Register and enable the Living Chronicle context/slash flows. Durable memory expiry remains safe while UI access is disabled. |
| `SANGUINIUS_TAROT_ENABLED` | `false` | Register and enable Fate balance, history, standings, recovery, peer wagers, and owner test-ledger controls. |
| `SANGUINIUS_TAROT_STARTING_FATE` | `100` | Balanced first-use starting grant (1–1,000,000,000). |
| `SANGUINIUS_TAROT_GRACE_THRESHOLD` | `10` | Grace is eligible strictly below this Fate balance. |
| `SANGUINIUS_TAROT_GRACE_TARGET` | `25` | Grace tops the account up to this balance; must exceed its threshold. |
| `SANGUINIUS_TAROT_GRACE_COOLDOWN_HOURS` | `72` | Grace cooldown (1–8,760 hours). |
| `SANGUINIUS_TAROT_TRIAL_THRESHOLD` | `50` | Trial is eligible strictly below this Fate balance. |
| `SANGUINIUS_TAROT_TRIAL_REWARD_MIN` | `5` | Inclusive deterministic Trial reward minimum. |
| `SANGUINIUS_TAROT_TRIAL_REWARD_MAX` | `15` | Inclusive deterministic Trial reward maximum. |
| `SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS` | `24` | Trial cooldown (1–8,760 hours). |
| `SANGUINIUS_TAROT_WAGER_MINIMUM_STAKE` | `1` | Inclusive peer-wager stake minimum; must not exceed the maximum. |
| `SANGUINIUS_TAROT_WAGER_MAXIMUM_STAKE` | `100` | Inclusive equal-stake maximum; schema v10 caps this at 100. |
| `SANGUINIUS_TAROT_WAGER_OFFER_EXPIRY_HOURS` | `24` | Confirmed offer lifetime (1–8,760 hours). |
| `SANGUINIUS_TAROT_WAGER_DEFAULT_OUTCOME_HOURS` | `24` | Default outcome window after acceptance (1–168 hours). |
| `SANGUINIUS_TAROT_WAGER_RESOLUTION_GRACE_HOURS` | `48` | Grace after the outcome deadline before owner escalation (1–168 hours). |
| `SANGUINIUS_TAROT_DRAW_COOLDOWN_HOURS` | `24` | Cooldown shared by ordinary public and private deck draws (1–744 hours). |
| `SANGUINIUS_TAROT_HOUSE_ENABLED` | `true` | Enable deck draws, House play, and the persisted Friday offer schedule when Tarot is enabled. |
| `SANGUINIUS_TAROT_HOUSE_EXPOSURE_CAP` | `100` | Maximum non-test profit collateral reserved by open House offers and funded House wagers. |
| `SANGUINIUS_TAROT_HOUSE_PROFIT_CAP` | `20` | Maximum integral profit promised by any one House wager. |
| `SANGUINIUS_TAROT_INTEGRATION_ENABLED` | `true` | Enable idempotent post-settlement integration observations and derived effects. While disabled, new observations are terminally audited as `integration_disabled` and are not replayed if the feature is later enabled. |
| `SANGUINIUS_APPEARANCES_MODE` | `off` | Appearance engine mode: `off`, inspection-only `dry_run`, or conservatively budgeted `live`. |
| `SANGUINIUS_VOX_ENABLED` | `false` | Register output-only Vox commands/callbacks and the Guild Voice States intent. Disabled startup still closes stale persisted sessions. |
| `SANGUINIUS_VOX_NARRATION_ENABLED` | `false` | Enable post-commit Chronicle, Tarot, appearance, and contextual boundary narration. Requires Vox output. Disabled observations are durably suppressed rather than backlogged; direct `/vox say` and approved static controls remain available. |
| `SANGUINIUS_VOICE_INPUT_ENABLED` | `false` | Master gate for experimental 5/10/15-second combined-channel listening windows. Requires Vox; ships off. |
| `SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED` | `false` | Owner attestation that every present and subsequently joining human gave prior consent outside Discord. Never infer this value. |
| `SANGUINIUS_TRANSCRIPTION_PROVIDER` | `disabled` | `disabled` or `openai`; disabled preserves all text/TTS behavior. |
| `SANGUINIUS_TRANSCRIPTION_MODEL` | `gpt-transcribe` | The only Milestone 17 model. Any other value reports voice input unavailable; there is no fallback. |
| `SANGUINIUS_TRANSCRIPTION_REQUEST_TIMEOUT_MS` | `30000` | Lowerable transcription request deadline, 1–30,000 ms. |
| `SANGUINIUS_VOICE_INPUT_ROLLING_DAY_WINDOWS` | `50` | Lowerable accepted-window ceiling per rolling 24 hours. |
| `SANGUINIUS_VOICE_INPUT_ROLLING_DAY_MICRO_USD` / `SANGUINIUS_VOICE_INPUT_MONTHLY_MICRO_USD` | `250000` / `5000000` | Lowerable estimated-cost ceilings ($0.25 rolling day and $5 UTC month), reserving 75 micro-USD/requested second. |
| `SANGUINIUS_TTS_PROVIDER` | `disabled` | `disabled` or the fixed OpenAI speech adapter. Disabled keeps approved static/text fallback available. |
| `SANGUINIUS_TTS_MODEL` / `SANGUINIUS_TTS_VOICE` | `tts-1` / `onyx` | Exact allowed production pair; other values fail configuration. |
| `SANGUINIUS_TTS_CACHE_DIRECTORY` | `/var/cache/sanguinius/tts` | Absolute cache path outside releases, state, and backups. |
| `SANGUINIUS_FFMPEG_PATH` / `SANGUINIUS_FFPROBE_PATH` | `/usr/bin/ffmpeg` / `/usr/bin/ffprobe` | Absolute tested FFmpeg 9 executables. |
| `SANGUINIUS_TTS_FALLBACK_DIRECTORY` | `/usr/local/share/sanguinius/vox` | Absolute directory containing the approved `fallbacks-v1.json` and normalized PCM clips. |
| `SANGUINIUS_TTS_MAXIMUM_TEXT_SCALARS` | `350` | Lowerable direct-speech input ceiling. |
| `SANGUINIUS_TTS_ROLLING_DAY_ATTEMPTS` | `100` | Lowerable rolling 24-hour provider-attempt ceiling. |
| `SANGUINIUS_TTS_ROLLING_DAY_MICRO_USD` / `SANGUINIUS_TTS_MONTHLY_MICRO_USD` | `500000` / `10000000` | Lowerable estimated-cost ceilings ($0.50 rolling day and $10 UTC month). |
| `SANGUINIUS_TTS_CACHE_MAXIMUM_MIB` / `SANGUINIUS_TTS_CACHE_MAXIMUM_DAYS` | `128` / `30` | Lowerable cache size and age ceilings. |
| `SANGUINIUS_TTS_MAXIMUM_DURATION_SECONDS` | `20` | Lowerable decoded-duration ceiling. |
| `SANGUINIUS_TTS_CONNECT_TIMEOUT_MS` / `SANGUINIUS_TTS_REQUEST_TIMEOUT_MS` | `5000` / `30000` | Lowerable verified-HTTPS connect and total synthesis deadlines. |
| `SANGUINIUS_FFPROBE_TIMEOUT_MS` / `SANGUINIUS_FFMPEG_TIMEOUT_MS` | `5000` / `10000` | Lowerable fixed-process probe and normalization deadlines. |

Boolean variables accept only the exact lowercase values `true` and `false`.
Every explicitly supplied variable must have a nonempty value; omit an
optional variable to select its default. Empty values do not silently fall
back to the command prefix, model, persona, path, or credential defaults.
The sample [configuration environment](config/sanguinius.env.example) contains
all fields without real IDs or credentials.

Experimental voice input uses D++'s anonymous combined receive callback and is
not a supported dependency of any core feature. It confirms a conspicuous
public indicator before capture, keeps PCM only in locked/nondumpable anonymous
memory, streams one in-memory WAV request to `/v1/audio/transcriptions`, and
returns validated transcript text only through the requester's ephemeral
interaction. A Chronicle draft exists only when that integration is available;
it is securely scrubbed by a monotonic five-minute reaper, on eviction, after
use, and during shutdown. Discord must confirm private transcript delivery;
failed or unknown receipts are followed by a sequenced transcript-free edit,
and shutdown keeps that redaction path open until it resolves. If neither the
ended-status edit nor its replacement can be confirmed, listening becomes
unavailable and TTS remains excluded until repair or restart. `/sang-admin
safety set target:voice-input mode:disabled` is the immediate durable kill
switch even when the broader admin catalog is disabled. Stop and
disable preempt capture before Discord acknowledgement or worker admission;
their audit work uses a dedicated privacy queue. A durable provider-attempt
marker is committed before entering the network client so restart cannot
refund an ambiguously transmitted request. D++ warns that Discord does not officially support bot
audio reception, so a build or runtime without receive capability degrades to
text/TTS-only.

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
./build/release/sanguinius db tarot check
./build/release/sanguinius db tarot rebuild --confirm
./build/release/sanguinius db invariants check
./build/release/sanguinius db invariants rebuild --confirm-rebuildable-projections
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
`db tarot check` folds the complete committed ledger and prints only invariant
status/counts. When Tarot is enabled, normal startup performs the same check
and fails closed on prepared rows, imbalance, negative human history, overflow,
illegal reversal, recovery mismatch, orphaned linkage, malformed wager
transfers/deadlines, or a difference between the `ESCROW` balance and unresolved
peer-plus-House funded obligations. It also verifies House transfer shapes,
exposure, and combined escrow reconciliation. Health reports only open-funded
counts, escrow/obligation totals, House exposure, dispute count, and redacted
invariant counts—never terms, evidence, member balances, or ledger identifiers.
`db invariants check` is the schema-v16 read-only umbrella for these domain
checks plus SQLite/FK integrity, durable work, consumer lag/receipts, Chronicle
FTS, complete appearance public-outbox target/provenance/payload validation,
public-delivery dependency order, Vox speech/narration, voice kill-switch and
listening-window transition audit chains, active-window consent/privacy gates,
recomputed AI charges/budgets, provider-circuit and runtime-control transition
chains, and immutable list snapshots.
It reports and rejects undrained relationship, House, Tarot-integration,
appearance, and Vox-narration consumer checkpoints independently.
Its guarded rebuild refuses a recently active
instance and changes only Chronicle FTS, relationship projections, and Tarot
player integration projections before rerunning the umbrella check.

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
Migration `0008_appearance_live` advances the mode fence to `off`, `dry_run`,
or `live`; rebuilds the decision family without losing v7 audit rows; and adds
concurrency-safe budget reservations, server-wide quiet/kill state, opaque
feedback controls and append-only feedback, restart-safe alert throttles, and
verified Chronicle appearance sources. Its fail-closed trigger permits only a
fully linked `discord.public.v1` row for a current-epoch live decision,
reservation, configured guild, and primary channel; direct SQL proofs continue
to reject appearance outbox inserts in `off` and `dry_run`, including unrelated
rows inserted after a deferred reservation. It is forward-only: binary rollback
restores a verified schema-v7 backup and the accepted Milestone 9 artifact
rather than applying reverse SQL.
Migration `0009_tarot_ledger` adds unique human/system accounts, monotonic
prepared/sealed double-entry transactions, immutable postings and Trial draws,
single-terminal recovery claims, and bounded invoker-scoped history snapshots.
It has no balance column, cache, wager table, or direct balance setter. Starting
grants, Grace, Trial rewards, owner test adjustments, and test reversals are
balanced exact-shape transactions protected by SQL constraints/triggers,
checked arithmetic, non-negative human balances, and idempotency keys. Public
recovery flavor is linked atomically to one neutral durable outbox row. It is
forward-only: binary rollback restores a verified schema-v8 backup and the
accepted Milestone 10 artifact/catalog rather than applying reverse SQL.
Migration `0010_peer_wagers` rebuilds the ledger family with count-verified
copy/swap steps so schema-v9 order and `AUTOINCREMENT` continuity survive while
adding exact-shape `WAGER_ESCROW_FUND`, `WAGER_PAYOUT`, and `WAGER_REFUND`
transactions. It adds immutable equal-stake terms, append-only actions,
outcomes, void consents, evidence and resolutions, revision/user-bound controls
and receipts, linked deadline jobs/notices, revisioned public-card edits,
participant history snapshots, simulated roles, and exact test-cleanup
reversals. SQL triggers enforce role/state authority, term immutability,
terminal cancellation, exact terminal-resolution audit metadata,
owner-contained self-test roles, event/transfer linkage, and public/sealed
payload rules.
It is forward-only: rollback preserves the failed v10 database, restores the
checksum-verified schema-v9 backup, and runs the accepted Milestone 11
artifact/catalog rather than applying reverse SQL.
Migration `0011_tarot_house_integration` adds immutable versioned deck and
House catalogs, persisted draw receipts, snapshotted House offers/wagers,
deadline and weekly-offer jobs, exact House transfer links, public-card audit,
test cleanup, and a rebuildable player-result projection. It widens the
accepted wager ledger shapes to exactly one peer or House owner link and checks
combined escrow. A count-verified Chronicle copy/swap adds unique
`tarot_event` provenance; title provenance admits `tarot_system` proposals.
Durable source observations drive once-only Chronicle proposals, relationship
events, Tarot appearance candidates, title thresholds, and expiring text-only
future Vox intents after Fate settlement has committed. Existing schema-v10
peer results seed baseline statistics before observation triggers exist, so no
retroactive effects are emitted. It is forward-only: rollback preserves the
v11 database and diagnostics, restores the checksum-verified database captured
immediately before migration, and selects the matching accepted binary and
catalogs rather than applying reverse SQL.
Migration `0012_vox_foundation` adds the revisioned `voice_session` state
machine, append-only transition history, immutable interaction receipts,
partial uniqueness for one active session, and links to durable connect,
reconnect, leave-cleanup, and empty-channel timeout jobs. Ready and terminal
public cards use the existing `discord.public.v1` outbox with an immutable
ready-before-terminal dependency; shutdown quarantines an already-submitted
card as an unknown outcome rather than relabeling it unsent. It stores session
metadata and static fixture state only—never D++ objects, TTS data, received
audio, or transcripts.
It is forward-only: rollback preserves the failed schema-v12 database and
diagnostics, then restores a checksum-verified schema-v11 backup and its
matching catalog-v10 artifact rather than applying reverse SQL.
Migration `0013_vox_tts` adds snapshotted speech items and append-only
transitions, pessimistic provider-attempt accounting, normalized-cache
metadata, one-guild voice selection, timed mute metadata, and the durable
hourly TTS purge. Raw line text is cleared when normalized media is published
or the item becomes terminal. Sanitized usage remains for 13 months; terminal
speech metadata remains for 30 days. It is forward-only: rollback preserves
schema-v13 diagnostics/cache for inspection, restores the verified schema-v12
backup, and activates the accepted M14 artifact and catalog v11. Never reverse
schema 13 in place.

Migration `0014_vox_narration` adds a migration-head journal cursor, durable
feature intents and append-only transition audit, immutable 0–100 narration
rank on speech items, and catalog-v13 owner controls. A bounded scanner observes
only committed Chronicle, Tarot, and appearance events; it creates at most one
intent and speech item per source event/slot, waits for confirmed public text,
and admits at most two feature lines per Vox session with at most one per
feature. Session open/close uses fixed public state cards. Sealed/recovery
wagers, balances, notices, memories, summaries/excerpts, transcripts, evidence,
ledger identifiers, and relationship dimensions never enter narration prompts
or speech rows. Server quiet suppresses all automatic narration; direct speech
and approved static control clips remain independent. Legacy Tarot Vox intents
are migrated to terminal `pre_m16_not_replayed` audit, and the old table is
removed. It is forward-only: preserve schema-v14 diagnostics, restore the
checksum-verified schema-v13 backup, activate accepted commit `8a5f7cf`, and
restore catalog v12 rather than reversing schema 14 in place.

Migration `0015_voice_input` adds immutable owner consent-attestation revisions,
a durable kill switch/history, one revisioned listening window and append-only
transition stream, and fixed `gpt-transcribe` operational usage. It stores no
audio, transcript, transcript hash, detected language, provider body,
interaction token, or arbitrary JSON. A count-checked Chronicle rebuild adds
`voice_transcript` provenance while preserving row IDs, FTS, children, views,
indexes, and every later Tarot/appearance trigger. Catalog v14 adds bounded
listen/stop and owner kill controls. The feature-off M17 binary can keep schema
v15; a binary rollback restores the checksum-verified schema-v14 backup and
catalog v13 together. Never reverse schema 15 in place.

Migration `0016_cross_feature_reliability` adds the prompt-free text-generation
budget ledger, persistent OpenAI/TTS/transcription circuits, durable text-AI,
TTS, and Vox-output operator controls, generic invoker-bound list snapshots,
and retention-run accounting. The list snapshot stores only stable Chronicle
title or House-wager identifiers for five-item Previous/Next pages; the
retention job removes it after the 15-minute interaction lifetime plus the
24-hour replay buffer. A single
bounded cross-feature orchestrator now wakes the accepted relationship, House,
Tarot, appearance, and Vox consumers in dependency order while retaining their
existing durable receipts and cursors. It also owns 60-second recovery passes;
one consumer failure is contained and does not prevent later consumers from
running; owner health reports each consumer as ready, backlogged, or degraded.
A daily 04:00 UTC retention job tombstones or redacts only terminal,
age-eligible payloads, including every terminal durable-work copy of sealed
notice prose. Notice tombstones retain a content fingerprint so an exact
idempotent retry still replays safely while conflicting content is rejected.
Each run records bounded per-category counts, including TTS-cache removals or
safe cache-failure counts; cache failures do not block database retention, and
an atomic database-cleanup failure is recorded as a failed run. It also removes
terminal speech rows after 30 days, terminal AI/TTS/transcription usage after
13 months, and expired/orphaned TTS cache entries even while Vox is configured
off. It never purges Chronicle canon, the
event journal, relationship history, the Tarot ledger/wagers, delivery
provenance, safety transitions, active/unknown work, or raw voice audio.
Catalog v15 retires
`!help`/`!repo`, adds root `/help` and `/repo`, and consolidates operator kills
under `/sang-admin safety`. Schema v16 is forward-only: preserve the v16 copy
and redacted diagnostics, restore the checksum-verified v15 backup, activate
accepted M17 `2e11130`, and restore catalog v14. Never reverse schema 16 in
place.

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
Chronicle session/anniversary and appearance scan/purge handlers. Live
appearance finalization is the only appearance path that may enqueue public
delivery, and it commits before waking the existing outbox workers.

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
Rejected requests generate no Discord response. The retired exact `!help` and
`!repo` messages are silent; leading mentions retain their configured scope and
deterministic safety checks.

## Architecture

`sanguinius_core` contains application services and project-owned interfaces;
it has no SQLite, D++, curl, or JSON dependency. `sanguinius_persistence`
contains the move-only SQLite RAII layer, transaction/migration/backup support,
and concrete core repositories. `sanguinius_runtime` contains the D++ gateway
and OpenAI adapters plus the production composition root. Tests use temporary
SQLite files and deterministic fakes, so ordinary CTest runs need no Discord or
OpenAI credentials and make no network calls.

Cross-feature work is driven by `event_journal` and the accepted feature-owned
observation rows, source keys, and cursors. Gateway/post-commit paths submit an
O(1) wake hint only. The orchestrator coalesces wakes, limits each consumer to
50 records per pass, self-wakes while backlog remains, and runs a recovery pass
every 60 seconds. Raw message observation is similarly normalized once before
identity, Chronicle-session context, and appearance-activity evaluation.

All text-model work passes through transactional admission before reaching a
two-call provider boundary. The policy caps estimated text cost at $1.25 per
rolling 24 hours and $25 per UTC month, 300 accepted generations per rolling
day, 30 direct requests per user per ten minutes, 16,000 UTF-8 input bytes, and
500 output tokens. Direct, explicit-feature, and optional prose use bounded
4:2:1 lanes with 16 of 64 queue slots reserved for direct work. Unknown or
unpriced models fail closed. Provider records contain only safe categories,
bounded request/correlation identifiers, timing, and usage—not prompts,
responses, transcripts, headers, or provider bodies. Three retryable failures
within five minutes open a five-minute persistent circuit; authentication and
configuration failures remain open until corrected and restarted. Cancelled or
crashed half-open probes are durably returned to an immediately retryable open
state so one abandoned probe cannot disable a provider permanently.
Admission reserves the full 16,000-input-token ceiling so provider framing is
also covered, then finalizes successful attempts downward from reported usage.
The durable sent fence is written by the provider adapter at its final
pre-transfer boundary; local validation, cancellation, or setup failure before
that callback releases the reservation. OpenAI text responses are bounded to
one MiB during receipt before parsing.
An open TTS circuit is reported as degraded and releases its known-unsent usage
reservation, so it consumes neither a provider attempt nor estimated spend.
TTS success resets its circuit only after media normalization succeeds, and an
operator TTS kill interrupts active provider work before its audio can be
cached or played; approved static/text fallbacks remain available.

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
