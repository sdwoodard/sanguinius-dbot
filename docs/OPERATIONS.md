# Sanguinius production operations

This runbook covers the single production host `nuln`. Build and test on the
development host. The target receives only verified immutable release archives;
never edit source on `nuln`, print either secret, or alter unrelated services.

## Fixed production layout

```text
/etc/sanguinius/
  sanguinius.env                 0600 root:root
  bot.token                      0600 root:root
  openai.key                     0600 root:root
/opt/sanguinius/
  current -> releases/<deployment-id>
  previous -> releases/<previous-deployment-id>
  operations -> releases/<schema-16-operations-deployment-id>
  releases/<deployment-id>/      root-owned, non-writable
/var/lib/sanguinius/
  sanguinius.sqlite3
  runtime/
  state-version
/var/cache/sanguinius/tts/
/var/log/sanguinius/messages.log
/var/backups/sanguinius/
```

The `sanguinius` no-login system user owns the database, cache, and logs. Root
owns configuration, releases, operation-control files, and backup contents.
`/var/lib/sanguinius/runtime` is `0750 root:sanguinius`; its lock, status, and
state-version staging files are created without following links.
`/var/backups/sanguinius` is `0710 root:sanguinius`, which lets the service read
filesystem capacity without listing or reading backup files. The service
receives the two secret files only through `LoadCredential=`. The environment
file contains IDs, fixed paths, rates, and flags, never token or API-key values.
Each service command sets the two credential paths after `EnvironmentFile=` is
loaded, so an environment-file path cannot override the credential mount.

## Build and package

The builder definition is `packaging/Containerfile`; every target package and
D++ input is pinned in `packaging/release-toolchain.lock`. The build targets
x86-64-v3 with a generic tune, embeds the commit timestamp, uses deterministic
path mappings, and installs `$ORIGIN/../lib` as its RPATH. It bundles only
`libdpp.so.10.1.7`; glibc, libstdc++, OpenSSL, curl, SQLite, Opus, zlib, and
FFmpeg come from `nuln`.

From a clean committed revision:

```bash
./scripts/release.bash image
./scripts/release.bash package
./scripts/release.bash verify \
  --archive "$PWD/dist/sanguinius-<release-id>.tar.zst"
```

Before the schema-16 deployment, also produce the accepted schema-13 recovery
release:

```bash
./scripts/release.bash package \
  --revision 8a5f7cf7582c0012a6da6cc5d94ffb8ccf61c296 \
  --schema-target 13 --catalog 12 --version 2.1.0 --compat
```

Packaging refuses a dirty tree or overwrite. Each archive has one rooted tree,
internal `RELEASE-METADATA.json` and `SHARE-MANIFEST.sha256`, an external
`.sha256`, and matching `.test-evidence.json`. Verification rejects traversal,
links, unmanaged files, credentials, state, logs, caches, build output, a wrong
ISA/interpreter/RPATH, missing dependencies, or disagreement between release
metadata and the binary's own identity. Compatibility packaging additionally
derives version, schema target, and command catalog from the selected source;
its legacy binary must report the matching version/revision through redacted
configuration output and the matching schema through `db status`. One verified
target build is sufficient for each future committed release.

For the same-revision rollback drill, keep the canonical build identity and add
only an operational deployment label:

```bash
./scripts/release.bash label \
  --archive "$PWD/dist/sanguinius-<release-id>.tar.zst" \
  --deployment-label rollback-drill
```

The resulting metadata has a distinct `deployment_id`, while `release_id` and
the executable bytes remain identical to the first package from that revision;
the label operation reuses its successful target-build evidence and does not
perform a second build.

## Inspect, bootstrap, and deploy

Read-only inspection is always the first remote action:

```bash
./scripts/deploy_nuln.bash inspect
```

`bootstrap` is privileged and mutating. Run it only after explicit approval.
The restricted environment source must already contain the production IDs,
paths, rates, and conservative flags, but no secret values:

```bash
./scripts/deploy_nuln.bash bootstrap \
  --rollback-archive "$PWD/dist/sanguinius-2.1.0-g8a5f7cf7582c.tar.zst" \
  --environment <restricted-remote-environment-path> \
  --token /home/sigmar/.config/sanguinius/bot.token \
  --openai-key /home/sigmar/.config/sanguinius/openai.key \
  --message-log /home/sigmar/git/sanguinius-dbot/logs/messages.log
```

Bootstrap verifies the stopped schema-13 baseline and preserves the legacy
state-directory owner until a checked backup and restore copy exist. It then
adopts the planned state ownership, transfers the database and every present
`.lock`, WAL, SHM, or journal companion to the `sanguinius` account with mode
`0600` and verifies that handoff, copies secrets and the append-only log without
displaying them, stages the compatibility release as `previous`, and does not
start it. Deploy repeats this ownership-and-mode reconciliation under the
operations lock, so a host bootstrapped by an earlier helper is upgraded before
the candidate or root backup helper opens the database.

Deploy schema 16 with:

```bash
./scripts/deploy_nuln.bash deploy \
  --archive "$PWD/dist/sanguinius-<release-id>.tar.zst" \
  --expected-schema 13 --target-schema 16
```

If a later retention audit finds that the immutable schema-13 compatibility
release is absent while its pre-migration backup remains, restage it without
changing the service, database, or active release:

```bash
./scripts/deploy_nuln.bash stage-compatibility \
  --archive "$PWD/dist/sanguinius-2.1.0-g8a5f7cf7582c.tar.zst"
```

This command requires production schema 16, the operations lock, a single
systemd-owned bot process, exact compatibility metadata, and a matching
pre-migration backup. It re-verifies that backup through the stable restore
helper before committing retention.

The local wrapper revalidates the clean source revision, archive, manifest, and
container evidence. The privileged helper verifies its own expected checksum,
acquires `/var/lib/sanguinius/runtime/operations.lock`, checks host/process/disk
state, installs to `.incoming-<release-id>`, verifies ELF requirements, and
atomically renames the immutable tree. Before any backup, migration, service, or
symlink mutation, a transient credential-backed `sanguinius` process runs the
candidate's complete `--check-config` against the exact production environment
and candidate assets. The helper first uses an online snapshot for disposable
migration and schema-13 recovery rehearsal. It then stops the service, proves
that no managed or unmanaged process can open the database, takes a fresh
authoritative snapshot, repeats the complete forward/rollback rehearsal, and
only then publishes that snapshot as the pre-migration backup and migrates
once. It checks integrity/invariants before switching `current`. The
rollback-managed setup transaction publishes `operations` before the candidate main
process can start, so the documented recovery helper remains available if a
schema-changing candidate reaches its main process and then must be fenced.
Success requires systemd `READY=1` after Discord READY and catalog
synchronization, followed by a fresh verified backup made by the active release.
The helper snapshots `NRestarts` before startup and, after backup and timer
finalization, rechecks the exact READY status, main-PID ownership, and unchanged
restart count before recording success. Recognized releases are pruned to
current plus three inactive releases. The schema-13 compatibility release is
selected inside those three slots before newer same-schema candidates, because
it is the executable half of a forward-only restore. Rollback-required targets
are protected before commit and any newly expired former target is removed
immediately after the successful deployment transaction commits.
For a same-schema error in that post-readiness finalization window, the helper
stops the candidate and restores the prior current/previous/operations links,
matching units, enablement, timer state, and prior active state. For a
schema-changing error, it stops and disables the candidate while preserving the
new schema and diagnostics for an explicit restore decision. Directories
without matching release metadata do not consume a retention slot and are not
removed.

The production service is `Type=notify`, restarts on failure, and uses strict
filesystem/device/kernel/namespace/capability protections. Its intentional
network surface is limited to Unix, IPv4, and IPv6 sockets for HTTPS,
WebSocket, and Discord UDP voice. The configured resource ceilings are 768 MiB
memory, 64 tasks, 2,048 descriptors, and 200% CPU; `MemoryHigh` begins at
512 MiB. The normal and compatibility bot units retain no capabilities. The
root backup unit retains only `CAP_DAC_OVERRIDE`, which is required to open the
mode-0600 service-owned SQLite database through the mode-0750 state directory;
its filesystem write allowlist remains limited to state and backups.
Before privileged deployment, backup, or restore code opens the database, it
requires the database and every present `.lock`, `-wal`, `-shm`, and `-journal`
companion to be a single-link regular file, never a symbolic link. The binary
also opens its `.lock` with no-follow semantics and validates the opened file
descriptor before taking the lock. It changes permissions only when the opened
file is not already mode `0600`; this lets the restricted root backup helper
operate on the service-owned lock with `CAP_DAC_OVERRIDE` and no ownership
capability.

## Backup and restore

`sanguinius-backup.timer` runs at 03:15 UTC with persistence and at most 15
minutes of stable random delay. It executes the helper through the stable
`operations` pointer, independent of the schema-compatible binary selected by
`current`. A manual verified backup is:

```bash
sudo /opt/sanguinius/operations/libexec/sanguinius-backup.bash manual
```

The helper uses SQLite online backup, checks the source and copy, compresses and
tests the stream, restores a disposable copy, and writes mode-0600 archive,
metadata, and checksum files. Seven complete recognized rolling triples are
retained; an incomplete sidecar set never consumes one of those seven slots.
Manual, pre-migration, failure, legacy, incomplete, and unrecognized files are
never pruned. Expired rolling triples are first moved into a private retention
stage; a failed final status publication restores every staged file, and only a
committed successful backup discards that stage. If the process or host dies
before the EXIT trap can run, the next lock-holding backup validates and moves
every safe staged artifact back before doing new work. A collision or unsafe
staged entry fails closed and preserves the stage for investigation.

Verification never touches production. Invoke the restore helper from a
schema-16 operations release even when the compatible database binary is the
schema-13 rollback release:

```bash
sudo /opt/sanguinius/operations/libexec/sanguinius-restore.bash verify \
  /var/backups/sanguinius/<backup>.sqlite3.zst \
  --release /opt/sanguinius/releases/<compatible-release-id>
```

Applying a restore additionally requires the service inactive, an exclusive
operations lock, a schema-compatible release, a fresh manual safety backup, and
the literal confirmation flag:

```bash
sudo systemctl stop sanguinius.service
sudo /opt/sanguinius/operations/libexec/sanguinius-restore.bash apply \
  /var/backups/sanguinius/<backup>.sqlite3.zst \
  --release /opt/sanguinius/releases/<compatible-release-id> --confirm
```

The helper reads the authoritative `schema_migrations` journal to select the
release compatible with the currently active database, then makes its safety
backup through its own schema-aware backup code; it does not depend on the
selected rollback release containing new helpers. The replacement is staged on
the state filesystem and synced before the old database and sidecars move to a
unique mode-0700 runtime quarantine and the staged file is atomically renamed
into place. After database checks, apply atomically selects the requested
release, installs its normal or compatibility unit, and updates `state-version`;
the service remains inactive. Any failed database or activation check restores
the quarantined database and the prior release/unit/state marker. If durable
activation rollback cannot be fully verified, the helper disables and fences the
service and retains its private runtime diagnostics for operator recovery. The
same fence applies if any original database or sidecar cannot be moved back and
verified against its pre-switch checksum; both the restore stage and nonempty
quarantine are retained. Reverse SQL is never used.

## Rollback and incidents

- A failure before production migration changes no database or symlink.
- A failed migration transaction must leave the old schema exact and verified.
- A pre-gateway post-migration failure, including a release-link, unit,
  sysusers/tmpfiles, or daemon-reload failure, first verifies a self-contained
  restored database, then quarantines the schema-16 database and atomically
  installs the schema-13 copy. An interrupted switch restores the quarantine; a
  successful rollback keeps the failed database as diagnostics and restores the
  complete pre-deployment host state. Post-migration verification failures use
  this same durable-state recovery transaction; if any restore or host-state
  reversal fails, the service is stopped and disabled, the timer is stopped,
  and the deployment fails with retained diagnostics instead of falling through
  an unmatched cleanup state.
- After a schema-changing main process starts, stop it, preserve schema 16 and
  diagnostics, disable it so a reboot cannot restart it, and require Stephen's
  explicit restore decision; an automatic restore could discard new writes.
- For a same-schema failure, the deployment transaction restores the prior
  durable links, units, sysusers/tmpfiles configuration, enablement, timer state,
  and active state. A later operator rollback additionally proves the stopped
  service has no unmanaged process or database opener before switching releases:

Automatic recovery treats service termination as a mandatory fence. The stop
command must succeed, systemd must report the unit inactive or failed, no
`sanguinius` process may remain, and no process may hold the database or a
sidecar open before a database restore, release switch, or previous-service
start. Schema-changing failures also require verified disablement. If any part
of that fence fails, the helper leaves the database and release selection alone,
does not start the previous release alongside the candidate, records failure,
emits a critical operator-intervention diagnostic, and retains its private
deployment stage behind an explicit operator-recovery marker. A later deployment
refuses to discard that marked evidence automatically.

```bash
./scripts/deploy_nuln.bash rollback-same-schema \
  --release-id <previous-release-id>
```

Use only redacted operational views:

```bash
systemctl status sanguinius.service
journalctl -u sanguinius.service --since today
systemctl show sanguinius.service \
  -p ActiveState -p SubState -p StatusText -p NRestarts \
  -p MemoryCurrent -p MemoryPeak -p TasksCurrent
sudo -u sanguinius env SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3 \
  /opt/sanguinius/current/bin/sanguinius db check
sudo -u sanguinius env SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3 \
  /opt/sanguinius/current/bin/sanguinius db integrity
sudo -u sanguinius env SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3 \
  /opt/sanguinius/current/bin/sanguinius db invariants check
```

Never dump `/etc/sanguinius`, `/proc/<pid>/environ`, database rows, private
notices, prompts, transcripts, provider bodies, or raw voice data. Owner health
shows only bounded release/schema/catalog/readiness, aggregate queues, backup
age/result, and disk warnings; production revisions are shortened and runtime
IDs are omitted.

## Final acceptance and resource evidence

Follow the governing Milestone 19 one-person sequence. Record idle, peak-TTS,
post-restart, and crash-recovery samples without capturing payloads:

```bash
systemctl show sanguinius.service \
  -p MainPID -p MemoryCurrent -p MemoryPeak -p TasksCurrent -p NRestarts
ps -p <main-pid> -o %cpu=,rss=,nlwp=
find /proc/<main-pid>/fd -maxdepth 1 -type l | wc -l
du -sb /var/cache/sanguinius/tts
stat -c %s /var/lib/sanguinius/sanguinius.sqlite3 \
  /var/lib/sanguinius/sanguinius.sqlite3-wal
pgrep -a -P <main-pid>
```

Require idle CPU within one percentage point of baseline, warmed RSS within
32 MiB, final threads/descriptors within +2/+4, no monotonic growth, zombie,
decoder leak, core, or systemd throttling. If evidence shows `MemoryHigh` was
crossed, change only to `MemoryHigh=640M` and `MemoryMax=896M`, then repeat the
observation and record why.

A reboot is optional. It requires Stephen's explicit same-session approval and
a fresh verified backup. If approved, verify SSH return, enabled service/timer,
READY status, schema/invariants, and no duplicate effects; otherwise record it
as skipped.

The owner-only `owner_live_safe` appearance fixture has a fixed, visibly tagged
test payload. It ignores prior bot-speech recency so the acceptance delivery is
deterministic, while live mode, the operator kill, quiet, consent, scope,
readiness, budget, cooldown, and transactional delivery gates remain active.
Prove quiet suppression with a second trigger, then immediately restore
`dry_run`.

## Required final state

Before ending acceptance, resynchronize catalog 16 after setting:

```text
SANGUINIUS_ADMIN_COMMANDS_ENABLED=false
SANGUINIUS_TEST_MODE=false
SANGUINIUS_CHRONICLE_ENABLED=true
SANGUINIUS_TAROT_ENABLED=true
SANGUINIUS_TAROT_HOUSE_ENABLED=true
SANGUINIUS_TAROT_INTEGRATION_ENABLED=true
SANGUINIUS_APPEARANCES_MODE=dry_run
SANGUINIUS_VOX_ENABLED=true
SANGUINIUS_VOX_NARRATION_ENABLED=true
SANGUINIUS_TTS_PROVIDER=openai
SANGUINIUS_TTS_CACHE_MAXIMUM_MIB=64
SANGUINIUS_TTS_CACHE_MAXIMUM_DAYS=14
SANGUINIUS_VOICE_INPUT_ENABLED=false
SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED=false
SANGUINIUS_TRANSCRIPTION_PROVIDER=disabled
```

Also require no active Vox/listening session, test wager, recovery claim,
pending test speech, or test escrow, and require the durable voice-input safety
kill to be cleared while configured voice input remains off. Do not switch
appearances to `live` without Stephen's explicit conservative-rollout approval.
