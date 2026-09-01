# Sanguinius Production Operations

This runbook defines the portable release, deployment, backup, restore, and
incident mechanism supplied by the repository. Production mutation must use
these helpers rather than ad hoc remote edits.

## Fixed layout

    /etc/sanguinius/
      sanguinius.env
      bot.token
      openai.key

    /opt/sanguinius/
      current -> releases/<release-id>
      previous -> releases/<previous-id>
      operations/
      releases/<release-id>/

    /var/lib/sanguinius/
      sanguinius.sqlite3
      runtime/
      state-version

    /var/cache/sanguinius/tts/
    /var/log/sanguinius/messages.log
    /var/backups/sanguinius/

Credentials are 0600 root:root and loaded with systemd LoadCredential.
Environment configuration contains no secret values. Releases are root-owned
and non-writable; state, cache, logs, credentials, and backups never reside in
a release.

## Build and package

Requirements:

- clean committed source;
- rootless Podman;
- enough disk/inodes for the pinned builder and dist;
- no credentials, databases, logs, caches, or build output in tracked files.

Build the pinned image once when required:

    ./scripts/release.bash image

Create one target-compatible artifact for the current revision:

    ./scripts/release.bash package

Verify an existing artifact:

    ./scripts/release.bash verify \
      --archive dist/sanguinius-<release-id>.tar.zst

The builder pins the Arch base/snapshot and D++ source, compiles for
x86-64-v3 with generic tuning and deterministic path/time mappings, runs the
Release suite, and stages:

- bin/sanguinius;
- lib/libdpp.so.10.1.7;
- validated configuration/catalog/persona assets;
- Vox fallback assets;
- readable immutable migrations;
- backup/restore helpers;
- systemd/sysusers/tmpfiles definitions;
- this runbook.

The rooted tar.zst has deterministic ordering, ownership, modes, and
timestamps. RELEASE-METADATA.json identifies the safe build contract.
SHARE-MANIFEST.sha256 covers the payload, and an external .sha256 covers the
archive. Existing destinations are never overwritten.

For a same-binary drill label, derive a labeled release from the verified
archive rather than rebuilding:

    ./scripts/release.bash label \
      --archive dist/sanguinius-<release-id>.tar.zst \
      --deployment-label rollback-drill

## Inspect

Read-only host inspection:

    ./scripts/deploy_nuln.bash inspect

This first-pass helper asserts the short hostname `nuln` and prints the machine
architecture, service/process state, and filesystem block availability for the
managed state and backup paths. It does not inspect CPU feature flags, required
tool versions, inode availability, release links, schema, or ownership. Those
checks remain mandatory before mutation and are performed again by the
privileged deploy preflight. Use approved redacted host inspection for any
additional evidence; never print the environment or credential files.

## Legacy host bootstrap

The checked-in bootstrap path is the one-time legacy adoption helper used for
the original schema-13 host. It requires that exact stopped database baseline
and a schema-13 compatibility archive; it is not a general provisioner for a
new host or a current schema-16 installation. The existing production host is
already bootstrapped, so ordinary maintenance must use `deploy` instead. A new
or rebuilt host requires a separately reviewed, version-aware bootstrap change.

For recovery or audit of that legacy path, its interface is:

    ./scripts/deploy_nuln.bash bootstrap \
      --rollback-archive <compatible-archive> \
      --environment <restricted-remote-environment-path> \
      --token <restricted-remote-token-path> \
      --openai-key <restricted-remote-openai-key-path> \
      --message-log <restricted-remote-message-log-path>

The archive is local, but the other four paths must already exist on `nuln`.
The wrapper uploads only the archive, its checksum, and the verified remote
helper; it passes the restricted source paths unchanged to that helper.

It validates host identity, the exact stopped schema-13
source/service/process state, and restricted inputs; creates only the
Sanguinius account/directories/units; copies credentials and the existing
message log without displaying contents; creates and verifies a schema-13
backup; stages the compatibility release; and leaves unrelated
services/packages unchanged.

Bootstrap is resumable and refuses conflicts rather than overwriting them.

## Deploy

    ./scripts/deploy_nuln.bash deploy \
      --archive "$PWD/dist/sanguinius-<release-id>.tar.zst" \
      --expected-schema <current-schema> \
      --target-schema <candidate-schema>

The local helper verifies clean revision/test evidence, archive checksum,
metadata, payload manifest, and helper hashes. It uploads to a unique
mode-0700 temporary directory.

The privileged remote helper:

1. acquires /var/lib/sanguinius/runtime/operations.lock nonblocking;
2. rechecks host, disk/inodes, current links, service/PID, process count, and
   expected schema;
3. verifies the archive before extraction and rejects traversal, unexpected
   links, roots, names, and release conflicts;
4. extracts under .incoming-<id>, verifies every payload entry, ELF
   interpreter/ISA/symbol/RPATH/ldd, and safe version JSON;
5. atomically renames the verified immutable tree;
6. stops the service when required and proves exclusive database ownership;
7. creates a verified pre-change online backup;
8. rehearses migration/check/invariants and compatible backup/restore rollback
   on disposable database copies;
9. migrates production exactly once when needed and rechecks schema,
   integrity, foreign keys, and domain invariants;
10. atomically updates previous/current and installs the matching unit;
11. reloads systemd, starts the service, and waits for READY, catalog
    synchronization, expected revision/schema, stable PID/restart state, and
    bounded health;
12. enables the backup timer and retains current plus at most three recognized
    inactive releases (all of them when fewer than three exist) after health
    succeeds;
13. removes only its validated upload/staging directory and releases the lock.

The compatibility release is prioritized but counts inside the three inactive
slots. Retention never removes current, previous, special backups, legacy
files, or unrecognized names.

## Service

The normal service uses Type=notify and NotifyAccess=main. READY is sent once
after Discord READY and successful/no-op guild command synchronization.
STOPPING is sent during SIGTERM shutdown.

A 60-second systemd watchdog is refreshed only while the gateway remains
connected and the catalog synchronized. Restart is on failure with bounded
start attempts.

ExecStartPre performs redacted configuration validation, exact schema/WAL,
integrity/foreign-key, and umbrella domain-invariant checks. It never migrates.

The unit uses strict filesystem/kernel protections, empty capabilities,
restricted namespaces/address families compatible with HTTPS/WebSocket/UDP
voice, UMask 0077, LimitCORE 0, bounded descriptors/tasks/memory/CPU, and
systemd-managed state/cache/log directories.

## Backup

Manual backup:

    sudo /opt/sanguinius/operations/libexec/sanguinius-backup.bash manual

The daily timer runs at 03:15 UTC with persistence and bounded random delay.
Each backup:

- locks operations;
- runs SQLite online backup through the matching release;
- verifies source/copy schema, integrity, foreign keys, and domain invariants;
- records safe metadata and original/compressed SHA-256;
- compresses and tests zstd;
- restores to a disposable copy and verifies it;
- deletes only its own raw staging file after all checks pass.

Managed rolling retention keeps seven complete rolling triples. Pre-migration,
failure, and manual backups are retained. Legacy/unrecognized backup names are
never pruned. A failed backup performs no retention.

## Restore verification

Verification never touches production:

    sudo /opt/sanguinius/operations/libexec/sanguinius-restore.bash verify \
      /var/backups/sanguinius/<archive>.sqlite3.zst \
      --release /opt/sanguinius/releases/<compatible-release>

It validates metadata and hashes, decompresses to a unique runtime path, checks
schema/integrity/foreign keys/domain invariants, converts the disposable copy
to WAL through db migrate, and runs exact db check.

## Restore apply

Apply is destructive and requires explicit confirmation:

    sudo /opt/sanguinius/operations/libexec/sanguinius-restore.bash apply \
      /var/backups/sanguinius/<archive>.sqlite3.zst \
      --release /opt/sanguinius/releases/<compatible-release> \
      --confirm

It requires root, inactive service, the operations lock, exact host/state root,
a schema-compatible release, successful verification, and a fresh safety
backup. The existing database and sidecars move to a timestamped quarantine
directory before the verified restored file is atomically installed. Apply
also moves `previous` to the old active release when necessary, points
`current` at the selected compatible release, installs that release's service
unit, updates `state-version`, and reloads systemd. It intentionally leaves
`sanguinius.service` inactive.

After reviewing the reported schema, release, deployment identity, and
quarantine name, inspect the root-owned immutable
`RELEASE-METADATA.json`. Compare its release ID, revision, schema, catalog,
service unit, and compatibility flag with the retained verified archive and
restore evidence. For a modern release whose metadata selects
`sanguinius.service` and `compatibility_release:false`, also compare the
binary's version JSON with that metadata:

    sudo /opt/sanguinius/current/bin/sanguinius --version --json

The schema-13 compatibility release predates version JSON. When its metadata
selects `sanguinius-compat.service` and `compatibility_release:true`, establish
binary identity from the already verified archive, manifest, immutable
metadata, and restore output instead; do not pass the unsupported option.

For either release type, verify exact database compatibility while the service
remains stopped:

    sudo -u sanguinius env \
      SANGUINIUS_DATABASE_FILE=/var/lib/sanguinius/sanguinius.sqlite3 \
      /opt/sanguinius/current/bin/sanguinius db check

Then start it explicitly and inspect the loaded type and result:

    sudo systemctl start sanguinius.service
    sudo systemctl is-active --quiet sanguinius.service
    sudo systemctl show sanguinius.service \
      -p Type -p ActiveState -p SubState -p Result -p ExecMainStatus

For the normal `Type=notify` unit, successful start includes application
readiness and command synchronization. A compatibility release may install
`Type=simple`; active state then proves only that its process is running.
Inspect its bounded startup journal, confirm Discord connected presence, and
exercise a safe command supported by that release before declaring it ready.
For either type, confirm a normal redacted member status/smoke response before
declaring the restore complete.

Never reverse an applied schema through SQL.

## Rollback

Same-schema rollback changes only the immutable release link/unit:

    ./scripts/deploy_nuln.bash rollback-same-schema \
      --release-id <previous-release-id>

If a schema-changing candidate fails before gateway start, preserve its
database/diagnostics, restore the verified prior-schema backup, and activate
the matching prior release.

If it fails after gateway start, stop it and preserve the database because new
writes may exist. Restoration requires an explicit operator decision.

Migration statement failure must leave the old schema transactionally intact.
Restore verification failure leaves production untouched.

## Incidents

Use the narrow durable safety control first when doing so contains the risk:
appearances, text AI, TTS, Vox output, or voice input.

Stop the service immediately for possible privacy breach, raw-audio retention,
ledger/escrow inconsistency, schema/invariant failure, uncontrolled duplicate
delivery, or runaway resource use.

Preserve only redacted evidence:

- version/release/schema/catalog and current/previous links;
- systemd active/substate, PID, restart count, watchdog/status;
- bounded health, consumer lag, queue/circuit categories;
- operations status and backup age/result;
- CPU, RSS, threads, descriptors, cache/database/WAL sizes, child processes.

Do not collect credentials, environment contents, prompts, responses,
transcripts, message-log content, private notices/wager evidence, full Discord
IDs, provider bodies, or raw audio.

Reproduce locally or with disposable database copies, fix and test locally,
then use the ordinary release/deploy path.

## Feature safety state

Public configuration examples ship optional features disabled. A production
operator may enable Chronicle, Tarot, appearances, Vox/TTS/narration, and voice
input only as a coherent reviewed policy.

Voice input additionally requires current external consent for every present
and later-joining human. If consent or receive/transcription capability is
uncertain, disable the voice-input safety target immediately.

Admin commands and test mode remain false in ordinary production. After live
testing, ensure there is no active listening window/session, test wager/escrow,
recovery claim, pending test speech/job, or stale admin/test catalog.

## Reboot

Reboot only with explicit current approval and a fresh verified backup. After
SSH returns, verify changed boot ID, enabled/active service and timer, READY,
expected release/schema, invariants, and no duplicate durable effects.
